#include <Common/Settings/RecommendedQuality.hpp>

#include <Common/Settings/RenderMethods.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace Common::Scalability
{
    namespace
    {
        // The reference GPU (perf index 100, RecommendedQuality.hpp: RTX 3070 Ti) carries 8 GiB. A device-class
        // stand-in index is scaled by VRAM against it: an untimed device with half the reference's memory is
        // not assumed to have the reference's throughput. More memory than the reference never RAISES the
        // index — unified memory (Apple) reports the whole system's share, which says nothing about shading
        // speed. The floor keeps a device that reports no heap size (0) on the lowest tier, not at index 0.
        constexpr double kReferenceVideoMemoryMiB = 8192.0;
        constexpr double kMinVideoMemoryScale     = 0.25;

        uint64_t ToMiB( uint64_t bytes )
        {
            return bytes / ( 1024ull * 1024ull );
        }

        // Does `device` run the method a level names (RenderMethods.hpp: what each method needs from the device)?
        template <typename Method>
        [[nodiscard]] bool DeviceRunsMethod( Method method, const CapabilityCatalog& device )
        {
            if ( NeedsRayQuery( method ) && !CapabilityCatalog::Offers( device.RayTracingModes, RayTracingMode::RayQuery ) )
                return false;
            if ( NeedsRayTracingPipeline( method ) &&
                 !CapabilityCatalog::Offers( device.RayTracingModes, RayTracingMode::RayTracingPipeline ) )
                return false;
            if ( NeedsFragmentStores( method ) && !device.FragmentStores )
                return false;
            return NeedsRenderArea( method ) <= device.MaxRenderArea;
        }

        // PURE. Does `device` run every render-method ceiling that `group`'s `level` sets? Only the method
        // parameters the group OWNS (ParameterSpec::Owner) are read: the other slots of Values[group][level] are
        // the spec Min and mean nothing.
        [[nodiscard]] bool DeviceRunsLevelMethods( const ScalabilityTable& table, std::size_t group, std::size_t level,
                                                   const CapabilityCatalog& device )
        {
            const ParameterValues& values = table.Values[group][level];
            const auto             owned  = [&]( Parameter p ) {
                return static_cast<std::size_t>( SpecOf( p ).Owner ) == group;
            };
            const auto value = [&]( Parameter p ) { return values[static_cast<std::size_t>( p )]; };

            if ( owned( Parameter::GlobalIlluminationMethod ) &&
                 !DeviceRunsMethod( static_cast<GIMethod>( value( Parameter::GlobalIlluminationMethod ) ), device ) )
                return false;
            if ( owned( Parameter::ReflectionsMethod ) &&
                 !DeviceRunsMethod( static_cast<ReflectionMethod>( value( Parameter::ReflectionsMethod ) ), device ) )
                return false;
            if ( owned( Parameter::ShadowsMethod ) &&
                 !DeviceRunsMethod( static_cast<ShadowMethod>( value( Parameter::ShadowsMethod ) ), device ) )
                return false;
            return true;
        }
    } // namespace

    float DeviceClassPerfIndex( DeviceClass deviceClass, uint64_t videoMemory, const ScalabilityTable& table )
    {
        const float base = table.DeviceClassPerfIndex[static_cast<std::size_t>( deviceClass )];
        switch ( deviceClass )
        {
            case DeviceClass::Unknown:
            case DeviceClass::Integrated:
                // Shared x86 memory: the heap figure is a carve-out of system RAM, not a speed signal.
                return base;
            case DeviceClass::AppleUnified:
            case DeviceClass::Discrete:
                break;
        }
        const double scale = std::clamp( static_cast<double>( ToMiB( videoMemory ) ) / kReferenceVideoMemoryMiB,
                                         kMinVideoMemoryScale, 1.0 );
        return static_cast<float>( base * scale );
    }

    std::array<Level, kGroupCount> RecommendLevels( const BenchmarkResult& result, const ScalabilityTable& table,
                                                    const CapabilityCatalog& device )
    {
        const float index =
             result.Timed ? result.GpuPerfIndex : DeviceClassPerfIndex( result.Class, result.VideoMemory, table );
        const uint64_t vramMiB = ToMiB( result.VideoMemory );

        std::array<Level, kGroupCount> levels{};
        for ( std::size_t g = 0; g < kGroupCount; ++g )
        {
            // Thresholds: index >= t[i] -> level i + 1. At most Epic: Cinematic is an offline level and is never
            // recommended.
            std::size_t level = 0;
            for ( std::size_t i = 0; i < table.RecommendThresholds[g].size(); ++i )
                if ( index >= table.RecommendThresholds[g][i] )
                    level = i + 1;

            // The VRAM cap: never a level whose memory requirement this machine does not reach, whatever the
            // index. An unknown heap size (0) reaches only the levels that require nothing. The capability cap in
            // the same walk: never a level whose render-method ceiling this device cannot run (no ray query, no
            // ray tracing pipeline, no fragment stores, a render area too small) — the recommendation is a level
            // the device renders AS NAMED, so the group's other parameters come from a level it can run.
            while ( level > 0 && ( table.MinVideoMemoryMiB[g][level] > vramMiB ||
                                   !DeviceRunsLevelMethods( table, g, level, device ) ) )
                --level;

            levels[g] = static_cast<Level>( level );
        }
        return levels;
    }

    Common::ResultStr<float> GpuPerfIndex( const std::vector<BenchmarkPass>&      passes,
                                           const std::vector<BenchmarkReference>& references )
    {
        if ( passes.empty() )
            return Common::MakeError<float>( "GPU benchmark: no pass was timed" );

        double logSum = 0.0;
        for ( const BenchmarkPass& pass : passes )
        {
            const auto reference =
                 std::find_if( references.begin(), references.end(),
                               [&]( const BenchmarkReference& r ) { return r.Name == pass.Name; } );
            if ( reference == references.end() )
                return Common::MakeError<float>(
                     std::format( "GPU benchmark: pass '{}' has no reference rate", pass.Name ) );
            if ( !( reference->WorkPerMillisecond > 0.0 ) )
                return Common::MakeError<float>(
                     std::format( "GPU benchmark: the reference rate of pass '{}' is not positive", pass.Name ) );
            if ( !( pass.Milliseconds > 0.0 ) || !( pass.Work > 0.0 ) )
                return Common::MakeError<float>(
                     std::format( "GPU benchmark: pass '{}' measured {} ms for {} work units — not a rate",
                                  pass.Name, pass.Milliseconds, pass.Work ) );
            logSum += std::log( ( pass.Work / pass.Milliseconds ) / reference->WorkPerMillisecond );
        }
        return Common::MakeSuccess(
             static_cast<float>( 100.0 * std::exp( logSum / static_cast<double>( passes.size() ) ) ) );
    }

    bool CacheValid( const std::optional<RecommendedQuality>& cached, const BenchmarkCacheKey& now )
    {
        return cached.has_value() && cached->Key == now;
    }
} // namespace Common::Scalability
