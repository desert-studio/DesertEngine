#pragma once

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Project
{
    // HOW THE PROJECT'S SERVER REPLICATES BY DISTANCE (WP-FAR-15, NET-ARCH §3.5-§3.6): the "Net" section of
    // `<project>/Config/Engine.json` (UE: AActor NetUpdateFrequency / MinNetUpdateFrequency / NetCullDistance of the
    // pawn class and the Replication Graph's per-class ReplicationPeriodFrame + CullDistance, set in
    // DefaultEngine.ini). Read by DesertServer at start (Server/main.cpp -> Net::RelevancySettings, the session's
    // form of the same values: the Net library is outside the module table, so Projects cannot name its types;
    // this struct is the ONE authored copy, that one is derived from it at start and never edited). Shown and
    // written by the Project Settings panel ("Net" section). These initialisers ARE the defaults: a project without
    // the file replicates with them.
    struct NetRateBand
    {
        // Pawns nearer than this (cm, ground distance from the connection's view) are sent every EverySteps server
        // steps (60 steps = 1 s). The last band's ToCm is not read: it reaches everything farther.
        float         ToCm       = 0.0f;
        std::uint32_t EverySteps = 2;
    };

    struct NetSettings
    {
        // Ascending by ToCm. The first band's EverySteps is the snapshot cadence (the own pawn's ack period):
        // 2 = 30 Hz. Defaults: 50 m at 30 Hz, 150 m at 15 Hz, 400 m at 6 Hz, beyond at 3 Hz.
        std::vector<NetRateBand> Bands{ { 5000.0f, 2 }, { 15000.0f, 4 }, { 40000.0f, 10 }, { 0.0f, 20 } };
        // A pawn whose state has not changed since the connection last received it goes every this many steps
        // (a keep-alive; UE dormancy / MinNetUpdateFrequency).
        std::uint32_t DormantEverySteps = 60;
        // Every connection sees pawns within this circle (cm) whatever its own streaming shape (UE NetCullDistance
        // of the pawn; UE's default is 150 m, a scope's target stands farther). A pawn leaves relevancy only past
        // the circle widened by the World Partition UnloadMargin (WorldPartitionSettings), so the edge never
        // flickers.
        float PawnCullRangeCm = 50000.0f;
        // The farthest a client's reported view shape (its scope sector) may reach, cm: the cap on what one
        // connection can ask the server to replicate.
        float MaxViewRangeCm = 150000.0f;
        // Bytes of pawn state per second per connection the gateway writes into snapshots; due pawns beyond it wait
        // for the next snapshot, most overdue and nearest first (NET-ARCH §3.6; Iris prioritisation).
        std::uint32_t BudgetBytesPerSecond = 65536;
    };

    // Nullopt when usable; otherwise the one reason. THE one rule of the "Net" section: the panel, the save and the
    // server's start (which refuses with exit code 7) all call this, and nothing restates it. At least one and at
    // most 255 bands; ToCm ascending; every EverySteps and DormantEverySteps in 1..255 (one byte on the wire), a
    // multiple of Bands.front().EverySteps (snapshots leave only on that cadence) and non-decreasing across the
    // bands, DormantEverySteps >= the last band's; 0 < PawnCullRangeCm <= MaxViewRangeCm; BudgetBytesPerSecond >=
    // 1024. Implemented in NetSettings.cpp.
    [[nodiscard]] std::optional<std::string> ValidateNetSettings( const NetSettings& settings );

    // What this host replicates with: the project's section with this platform's layer over it (EngineConfig.hpp).
    // A project without the file replicates with the defaults above.
    const NetSettings& CurrentNetSettings();
    // The project layer alone: what Project Settings shows and writes.
    const NetSettings& ProjectNetSettings();

    // Writes @p toSave as the project layer's Net section; the file's other sections are kept. Refuses whatever
    // ValidateNetSettings refuses, naming the reason, and then leaves the file as it was.
    Common::BoolResultStr SaveNetSettings( const NetSettings& toSave );
} // namespace Desert::Project
