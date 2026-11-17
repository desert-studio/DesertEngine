#pragma once

#include <Common/Core/ResultStr.hpp>

#include <cstdint>

namespace Desert::Project
{
    // HOW THE PROJECT'S PARTITIONED WORLDS STREAM: the "WorldPartition" section of `<project>/Config/Engine.json`
    // (UE: the wp.Runtime.* values a project sets in DefaultEngine.ini, under the platform's own ini). Read by the
    // WorldStreamer when Play or a packaged game begins streaming (Engine/Core/WorldStreamer.cpp); the residency
    // rules take it as the base of Rules::ResidencySettings
    // (Engine/Core/Serialize/WorldPartitionResidencyRules.hpp), so these initialisers ARE the engine's defaults
    // and there is no second copy of them.
    struct WorldPartitionSettings
    {
        // Main-thread time one frame may spend activating cells, milliseconds.
        double ActivationBudgetMs = 2.0;
        // Cell loads in flight at once (UE wp.Runtime.MaxLoadingStreamingCells, default 4). At least 1.
        std::uint32_t MaxConcurrentLoads = 4;
        // The hysteresis band: a resident cell is kept while within LoadingRange * RangeScale * (1 +
        // UnloadMargin); a sector's angle and a look-ahead's sweep are widened by the same factor.
        float UnloadMargin = 0.25f;
        // The wait before retrying a failed load, growing by RetryBackoff per consecutive failure, at most
        // MaxRetryDelaySeconds.
        double RetryDelaySeconds    = 1.0;
        double RetryBackoff         = 2.0;
        double MaxRetryDelaySeconds = 30.0;
        // UE wp.Runtime.BlockOnSlowStreaming: while the cell under a streaming source is not resident (Critical,
        // Engine/Core/Serialize/WorldPartitionStreamingPerformance.hpp) the game stops behind the loading overlay.
        // Off: play goes on over the HLOD (or the empty ground) and the log says "slow streaming (blocking
        // disabled)".
        bool BlockOnSlowStreaming = true;
        // UE wp.Runtime.BlockOnSlowStreamingRatio and wp.Runtime.BlockOnSlowStreamingWarningFactor: streaming is
        // SLOW while a cell the sources want is not activated yet within SlowStreamingRatio *
        // SlowStreamingWarningFactor of a source's loading range (the cell's distance over that range). UE also
        // BLOCKS inside the ratio alone; here blocking is the owner's rule O2 (only the cell under the source's
        // feet), so the two values place the start of SLOW, the band UE warns in.
        float SlowStreamingRatio         = 0.25f;
        float SlowStreamingWarningFactor = 2.0f;
    };

    // What this host streams with: the project's section with this platform's layer over it (EngineConfig.hpp).
    // A project without the file streams with the defaults above.
    const WorldPartitionSettings& CurrentWorldPartitionSettings();

    // The project layer alone (Config/Engine.json): what Project Settings > Engine > World Partition edits.
    const WorldPartitionSettings& ProjectWorldPartitionSettings();

    // Writes @p toSave as the project layer's WorldPartition section; the file's other sections are kept. Refuses
    // a value the residency cannot run with (no load in flight, a negative margin, a backoff below 1, ...) naming
    // every value, and then leaves the file as it was.
    Common::BoolResultStr SaveWorldPartitionSettings( const WorldPartitionSettings& toSave );
} // namespace Desert::Project
