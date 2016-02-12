//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#define _CRT_SECURE_NO_WARNINGS

#include "LegacyBlockRandomizer.h"
#include <algorithm>
#include <utility>
#include <iostream>

#include "DataReader.h"

#ifndef UNREFERENCED_PARAMETER
#define UNREFERENCED_PARAMETER(P) (P)
#endif

namespace Microsoft { namespace MSR { namespace CNTK {

static inline size_t rand(const size_t begin, const size_t end)
{
    // eldak: this has already been changed by Alexey(alrezni)
    // still only covers 32-bit range
    const size_t randomNumber = ::rand() * RAND_MAX + ::rand();
    return begin + randomNumber % (end - begin);
}

// Shuffle a vector into random order by randomly swapping elements
// TODO: This functionality will be changed to std::shuffle - this work will be done during merging to master,
// and based on the work Alexey(R) has already done.
template <typename TVector>
void RandomShuffle(TVector& v, size_t randomSeed)
{
    if (v.size() > RAND_MAX * static_cast<size_t>(RAND_MAX))
    {
        RuntimeError("RandomShuffle: too large set: need to change to different random generator!");
    }

    srand(static_cast<unsigned int>(randomSeed));
    foreach_index (currentLocation, v)
    {
        // Pick a random location a location and swap with current
        const size_t randomLocation = rand(0, v.size());
        std::swap(v[currentLocation], v[randomLocation]);
    }
}

bool LegacyBlockRandomizer::TimelineIsValidForRandomization(const SequenceDescriptions& timeline) const
{
    SequenceDescription previous = { SIZE_MAX, 0, 0, true };

    auto it = std::find_if_not(timeline.begin(), timeline.end(),
                               [&](const SequenceDescription* current)
                               {
                                   bool result = current->m_isValid
                                       && previous.m_id + 1 == current->m_id
                                       && previous.m_chunkId <= current->m_chunkId
                                       && current->m_chunkId <= previous.m_chunkId + 1
                                       && 0 < current->m_numberOfSamples;
                                   previous = *current;
                                   return result;
                               });
    return it == timeline.end();
}

void LegacyBlockRandomizer::RandomizeChunks()
{
    // Create vector of chunk indices and shuffle them using current sweep as seed
    std::vector<size_t> randomizedChunkIndices;
    randomizedChunkIndices.reserve(m_numChunks);
    for (size_t i = 0; i < m_numChunks; i++)
    {
        randomizedChunkIndices.push_back(i);
    }
    RandomShuffle(randomizedChunkIndices, m_sweep);

    // Place randomized chunks on global time line
    m_randomizedChunks.clear();
    m_randomizedChunks.reserve(m_numChunks + 1);
    size_t chunkId, samplePosition, sequencePosition;
    for (chunkId = 0, samplePosition = m_sweepStartInSamples, sequencePosition = 0; chunkId < m_numChunks; chunkId++)
    {
        const size_t originalChunkIndex = randomizedChunkIndices[chunkId];
        const size_t numSequences =
            m_chunkInformation[originalChunkIndex + 1].m_sequencePositionStart -
            m_chunkInformation[originalChunkIndex].m_sequencePositionStart;
        const size_t numSamples =
            m_chunkInformation[originalChunkIndex + 1].m_samplePositionStart -
            m_chunkInformation[originalChunkIndex].m_samplePositionStart;
        m_randomizedChunks.push_back(RandomizedChunk { sequencePosition, samplePosition, originalChunkIndex });
        samplePosition += numSamples;
        sequencePosition += numSequences;
    }

    // Add sentinel
    m_randomizedChunks.push_back(RandomizedChunk { sequencePosition, samplePosition, SIZE_MAX });

    // For each chunk, compute the randomization range (w.r.t. the randomized chunk sequence)
    size_t halfWindowRange = m_randomizationRangeInSamples / 2;
    for (size_t chunkId = 0; chunkId < m_numChunks; chunkId++)
    {
        auto& chunk = m_randomizedChunks[chunkId];
        // start with the range of left neighbor
        if (chunkId == 0)
        {
            chunk.m_windowBegin = 0;
            chunk.m_windowEnd = 1;
        }
        else
        {
            chunk.m_windowBegin = m_randomizedChunks[chunkId - 1].m_windowBegin; // might be too early
            chunk.m_windowEnd = m_randomizedChunks[chunkId - 1].m_windowEnd; // might have more space
        }
        while (chunk.m_info.m_samplePositionStart - m_randomizedChunks[chunk.m_windowBegin].m_info.m_samplePositionStart > halfWindowRange)
            chunk.m_windowBegin++; // too early
        // TODO m_randomizedChunks[chunk.windowend + 1].info.samplePositionStart - m_randomizedChunks[chunk.windowbegin].info.samplePositionStart < m_randomizationRangeInSamples
        while (chunk.m_windowEnd < m_numChunks &&
               m_randomizedChunks[chunk.m_windowEnd + 1].m_info.m_samplePositionStart - chunk.m_info.m_samplePositionStart < halfWindowRange)
            chunk.m_windowEnd++; // got more space
    }

    // Compute the randomization range for sequence positions.
    m_sequencePositionToChunkIndex.clear();
    m_sequencePositionToChunkIndex.reserve(m_numSequences);
    for (size_t k = 0; k < m_numChunks; k++)
    {
        const size_t numSequences =
            m_randomizedChunks[k + 1].m_info.m_sequencePositionStart -
            m_randomizedChunks[k].m_info.m_sequencePositionStart;
        for (size_t i = 0; i < numSequences; i++)
        {
            m_sequencePositionToChunkIndex.push_back(k);
        }
    }
    assert(m_sequencePositionToChunkIndex.size() == m_numSequences);
}

bool LegacyBlockRandomizer::IsValidForPosition(size_t targetPosition, const SequenceDescription& seqDesc) const
{
    const auto& chunk = m_randomizedChunks[m_sequencePositionToChunkIndex[targetPosition]];
    return chunk.m_windowBegin <= seqDesc.m_chunkId && seqDesc.m_chunkId < chunk.m_windowEnd;
}

void LegacyBlockRandomizer::Randomize()
{
    const auto& timeline = m_deserializer->GetSequenceDescriptions();
    RandomizeChunks();

    // Set up m_randomTimeline, shuffled by chunks.
    m_randomTimeline.clear();
    m_randomTimeline.reserve(m_numSequences);
    for (size_t chunkId = 0; chunkId < m_numChunks; chunkId++)
    {
        auto originalChunkIndex = m_randomizedChunks[chunkId].m_originalChunkIndex;

        for (size_t sequencePosition = m_chunkInformation[originalChunkIndex].m_sequencePositionStart;
             sequencePosition < m_chunkInformation[originalChunkIndex + 1].m_sequencePositionStart;
             sequencePosition++)
        {
            SequenceDescription randomizedSeqDesc = *timeline[sequencePosition];
            randomizedSeqDesc.m_chunkId = chunkId;
            m_randomTimeline.push_back(randomizedSeqDesc);
        }
    }
    assert(m_randomTimeline.size() == m_numSequences);

    // Check we got those setup right
    foreach_index (i, m_randomTimeline)
    {
        assert(IsValidForPosition(i, m_randomTimeline[i]));
    }

    // Now randomly shuffle m_randomTimeline, while considering the
    // constraints of what chunk range needs to be in memory.
    srand(static_cast<unsigned int>(m_sweep + 1));
    foreach_index (i, m_randomTimeline)
    {
        // Get valid randomization range, expressed in chunks
        const size_t chunkId = m_sequencePositionToChunkIndex[i];
        const size_t windowBegin = m_randomizedChunks[chunkId].m_windowBegin;
        const size_t windowEnd = m_randomizedChunks[chunkId].m_windowEnd;

        // Get valid randomization range, expressed in sequence positions.
        size_t posBegin = m_randomizedChunks[windowBegin].m_info.m_sequencePositionStart;
        size_t posEnd = m_randomizedChunks[windowEnd].m_info.m_sequencePositionStart;

        for (;;)
        {
            // Pick a sequence position from [posBegin, posEnd)
            const size_t j = rand(posBegin, posEnd);

            // Try again if the sequence currently at j cannot be placed at position i.
            if (!IsValidForPosition(i, m_randomTimeline[j]))
                continue;

            // Try again if the sequence currently at i cannot be placed at position j.
            if (!IsValidForPosition(j, m_randomTimeline[i]))
                continue;

            // Swap and break out.
            std::swap(m_randomTimeline[i], m_randomTimeline[j]); // TODO old swap was perhaps more efficient
            break;
        }
    }

    // Verify that we got it right
    foreach_index (i, m_randomTimeline)
    {
        // TODO assert only
        if (!IsValidForPosition(i, m_randomTimeline[i]))
            LogicError("LegacyBlockRandomizer::Randomize: randomization logic mangled!");
    }
}

void LegacyBlockRandomizer::RandomizeIfNewSweepIsEntered()
{
    if (m_sequencePositionInSweep >= m_numSequences)
    {
        if (m_verbosity > 0)
            std::cerr << __FUNCTION__ << ": re-randomizing for sweep " << m_sweep
                      << " in " << (m_frameMode ? "frame" : "utterance") << " mode" << endl;
        m_sweep++;
        m_sweepStartInSamples += m_numSamples;
        Randomize();
        m_sequencePositionInSweep = 0;
    };
}

void LegacyBlockRandomizer::RandomizeForGlobalSamplePosition(const size_t samplePosition)
{
    size_t sweep = samplePosition / m_numSamples;

    if (m_sweep != sweep)
    {
        m_sweep = sweep;
        m_sweepStartInSamples = sweep * m_numSamples;
        Randomize();
    }
    m_sequencePositionInSweep = samplePosition % m_numSamples;
};

//
// Public methods
//

LegacyBlockRandomizer::LegacyBlockRandomizer(int verbosity, size_t randomizationRangeInSamples, DataDeserializerPtr deserializer)
    : m_verbosity(verbosity), m_randomizationRangeInSamples(randomizationRangeInSamples), m_deserializer(deserializer), m_sweep(SIZE_MAX), m_sequencePositionInSweep(SIZE_MAX), m_samplePositionInEpoch(SIZE_MAX), m_epochSize(SIZE_MAX)
{
    assert(deserializer != nullptr);
    const SequenceDescriptions& timeline = m_deserializer->GetSequenceDescriptions();
    assert(TimelineIsValidForRandomization(timeline));

    // TODO let timeline keep this info?
    m_numSequences = timeline.back()->m_id + 1;
    m_numChunks = timeline.back()->m_chunkId + 1;

    // Generate additional information about physical chunks
    assert(m_chunkInformation.size() == 0);
    m_chunkInformation.reserve(m_numChunks + 1);
    m_chunkInformation.insert(m_chunkInformation.begin(),
                              m_numChunks + 1,
                              ChunkInformation{SIZE_MAX, SIZE_MAX});

    size_t maxNumberOfSamples = 0;

    m_numSamples = 0;
    for (const auto& seqDesc : timeline)
    {
        // TODO let timeline keep this info?
        auto& chunkInformation = m_chunkInformation[seqDesc->m_chunkId];
        chunkInformation.m_sequencePositionStart =
            min(chunkInformation.m_sequencePositionStart, seqDesc->m_id);
        chunkInformation.m_samplePositionStart =
            min(chunkInformation.m_samplePositionStart, m_numSamples);
        maxNumberOfSamples = max(maxNumberOfSamples, seqDesc->m_numberOfSamples);
        m_numSamples += seqDesc->m_numberOfSamples;
    }

    // Add sentinel
    m_chunkInformation[m_numChunks] = {m_numSequences, m_numSamples};

    // Frame mode to the randomizer just means there are only single-sample sequences
    m_frameMode = (maxNumberOfSamples == 1);
}

void LegacyBlockRandomizer::Initialize(TransformerPtr next, const ConfigParameters& readerConfig)
{
    // Not used for the block randomizer.
    UNREFERENCED_PARAMETER(next);
    UNREFERENCED_PARAMETER(readerConfig);
}

void LegacyBlockRandomizer::StartEpoch(const EpochConfiguration& config)
{
    m_deserializer->StartEpoch(config);

    m_workerRank = config.m_workerRank;
    m_numberOfWorkers = config.m_numberOfWorkers;

    // eldak: check partial minibatches.
    if (config.m_totalEpochSizeInSamples == requestDataSize)
    {
        m_epochSize = m_numSamples;
    }
    else
    {
        m_epochSize = config.m_totalEpochSizeInSamples;
    }

    // TODO add some asserts on EpochConfiguration
    m_samplePositionInEpoch = 0;
    size_t timeframe = m_epochSize * config.m_epochIndex;
    assert(m_frameMode);           // TODO not (tested) yet
    assert(timeframe != SIZE_MAX); // used as special value for init
    RandomizeForGlobalSamplePosition(timeframe);
};

bool LegacyBlockRandomizer::AdvanceToNextPositionForThisWorker()
{
    while (m_samplePositionInEpoch < m_epochSize)
    {
        RandomizeIfNewSweepIsEntered();

        const auto& seqDesc = m_randomTimeline[m_sequencePositionInSweep];

        if ((seqDesc.m_chunkId % m_numberOfWorkers) == m_workerRank)
        {
            // Got one
            break;
        }

        m_samplePositionInEpoch += seqDesc.m_numberOfSamples;
        m_sequencePositionInSweep++;
    }

    return m_epochSize <= m_samplePositionInEpoch;
}

Sequences LegacyBlockRandomizer::GetNextSequences(size_t count)
{
    assert(m_samplePositionInEpoch != SIZE_MAX); // SetEpochConfiguration() must be called first

    std::vector<size_t> ids;
    bool endOfEpoch = false;
    Sequences result;

    while (ids.size() < count)
    {
        endOfEpoch = AdvanceToNextPositionForThisWorker();
        if (endOfEpoch)
        {
            break;
        }
        else
        {
            assert(m_sequencePositionInSweep < m_numSequences);
            ids.push_back(m_sequencePositionInSweep);
            const auto& seqDesc = m_randomTimeline[m_sequencePositionInSweep];
            m_samplePositionInEpoch += seqDesc.m_numberOfSamples;
            m_sequencePositionInSweep++;
        }
    };

    result.m_endOfEpoch = endOfEpoch;

    if (ids.size() == 0)
    {
        return result;
    }

    // Require and release chunks from the data deserializer
    const size_t windowBegin = m_randomizedChunks[m_sequencePositionToChunkIndex[ids[0]]].m_windowBegin;
    const size_t windowEnd = m_randomizedChunks[m_sequencePositionToChunkIndex[ids.back()]].m_windowEnd;

    for (size_t chunkId = 0; chunkId < m_numChunks; chunkId++)
    {
        if ((chunkId % m_numberOfWorkers) == m_workerRank)
        {
            auto originalChunkIndex = m_randomizedChunks[chunkId].m_originalChunkIndex;

            if (windowBegin <= chunkId && chunkId < windowEnd)
            {
                m_deserializer->RequireChunk(originalChunkIndex);
            }
            else
            {
                m_deserializer->ReleaseChunk(originalChunkIndex);
            }
        }
    }

    // Construct vector of original IDs and request data
    std::vector<size_t> originalIds;
    for (auto id : ids)
    {
        const auto& seqDesc = m_randomTimeline[id];
        originalIds.push_back(seqDesc.m_id);
    }

    result.m_data = m_deserializer->GetSequencesById(originalIds);
    return result;
};
} } }
