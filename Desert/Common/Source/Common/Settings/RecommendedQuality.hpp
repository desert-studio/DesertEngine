#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Settings/CapabilityCatalog.hpp>
#include <Common/Settings/Scalability.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// RECOMMENDED SETTINGS — A LEVEL PER GROUP FOR THIS MACHINE, MEASURED ONCE AND REMEMBERED (SCAL1).
//
// UE: Scalability::BenchmarkQualityLevels() runs a synthetic benchmark (FSynthBenchmark: CPU + GPU perf index),
// then maps each index through PerfIndexThresholds_<Group> in BaseScalability.ini. The shape is kept: a GPU perf
// index from a short timestamp-timed run, thresholds from the same Scalability.json as the levels (one data file,
// ScalabilityTable::RecommendThresholds), then a VRAM cap on the memory-bound groups.
//
// DIFFERENCES FROM UE, ON PURPOSE.
//   * No CPU perf index: no group's cost here is CPU-bound yet (view distance is GPU draw cost); a CPU index
//     with no reader is a dead value. It joins when a group's threshold needs it.
//   * The result is CACHED per (device, driver, table version) in machine.json, so a driver update or a table
//     change re-benchmarks, and nothing else does. UE re-runs on demand only.
//   * A device that cannot time passes (GpuTiming != AnyStage — MoltenVK on Apple GPUs, or no timestamps) does NOT
//     produce a perf index from wall-clock guesses: it takes the device-class fallback, and says so in the result.
//
// The ENGINE half (the GPU work) is Engine/Core/GpuBenchmark.hpp; this half is pure and lives in Common so the
// mapping and the cache are testable without a GPU.
namespace Common::Scalability
{
    // What a benchmark result is valid for. Any field differing = re-run.
    struct BenchmarkCacheKey
    {
        uint32_t    VendorId      = 0; // PCI vendor (CrashHandler GpuIdentity has the same numbers)
        uint32_t    DeviceId      = 0;
        uint32_t    DriverVersion = 0; // packed, vendor-specific; a driver update re-benchmarks
        std::string DeviceName;
        uint32_t    TableVersion = 0; // ScalabilityTable::Version — new thresholds re-map (no GPU re-run needed,
                                      // but the cache stores levels, so it is simplest and honest to re-run)

        bool operator==( const BenchmarkCacheKey& ) const = default;
    };

    // One timed workload of the benchmark (fill-rate, ALU, bandwidth, ...), for the log and the editor's panel.
    struct BenchmarkPass
    {
        std::string Name;
        double      Milliseconds = 0.0;
        double      Work         = 0.0; // the pass' work units (pixels, ALU ops, bytes)
    };

    // A pass' rate on the reference GPU, in the pass' work units per millisecond. The benchmark's backend owns
    // these beside its shaders (the workload and its reference rate change together); GpuPerfIndex reads them.
    struct BenchmarkReference
    {
        std::string Name;
        double      WorkPerMillisecond = 0.0;
    };

    // PURE. 100 x the geometric mean, over the passes, of (Work / Milliseconds) / the same-named reference's
    // WorkPerMillisecond — UE FSynthBenchmark's shape (each pass a ratio to the reference, combined). The
    // geometric mean keeps one pass that is ten times faster from hiding another that is ten times slower.
    // An error, never a made-up index: no passes, a pass with no reference, a pass with no time or no work, a
    // reference with no rate.
    [[nodiscard]] Common::ResultStr<float> GpuPerfIndex( const std::vector<BenchmarkPass>&      passes,
                                                         const std::vector<BenchmarkReference>& references );

    struct BenchmarkResult
    {
        // Normalised so the reference GPU (owner's RTX 3070 Ti) is 100, UE-style (GpuPerfIndex).
        // 0 when the device could not be timed (Timed false).
        float                      GpuPerfIndex = 0.0f;
        bool                       Timed        = false; // false = device-class fallback was used
        std::vector<BenchmarkPass> Passes;               // empty when not timed
        DeviceClass                Class       = DeviceClass::Unknown;
        uint64_t                   VideoMemory = 0;
    };

    // The cached answer in machine.json (MachineSettings gains `std::optional<RecommendedQuality> Recommended`).
    struct RecommendedQuality
    {
        BenchmarkCacheKey              Key;
        float                          GpuPerfIndex = 0.0f;
        bool                           Timed        = false;
        std::array<Level, kGroupCount> Levels{};

        bool operator==( const RecommendedQuality& ) const = default;
    };

    // PURE. Index -> level per group through the table thresholds, then the VRAM cap (Textures and Shadows never
    // above the level whose budget fits VideoMemory — thresholds in the table, not in code), Cinematic never.
    // When result.Timed is false the index is replaced by DeviceClassPerfIndex(Class, VideoMemory) — the
    // fallback is a named function, not a silent default.
    [[nodiscard]] std::array<Level, kGroupCount> RecommendLevels( const BenchmarkResult&  result,
                                                                  const ScalabilityTable& table );

    // The device-class stand-in perf index (Integrated low, AppleUnified scaled by VRAM, Discrete by VRAM).
    // Values come from the table's Recommend.DeviceClass section; Unknown -> the Low boundary.
    [[nodiscard]] float DeviceClassPerfIndex( DeviceClass deviceClass, uint64_t videoMemory,
                                              const ScalabilityTable& table );

    // PURE. Does `cached` answer for this device? Re-run when it does not, or when nothing is cached.
    [[nodiscard]] bool CacheValid( const std::optional<RecommendedQuality>& cached, const BenchmarkCacheKey& now );
} // namespace Common::Scalability
