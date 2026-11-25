#pragma once

#include <Common/Core/ResultStr.hpp>

#include <string>
#include <vector>

namespace Desert::Migration
{
    // WAVS 1 or 2 -> 3. v2 (WATER-UNDERWATER; UE AWaterBody takes its waves' direction from the wind source, not
    // the asset): no `Generator.WindAngleDeg` - the waves run with the scene's wind. v3 (WATER-WHOLE): the FFT
    // ocean has four cascades, so `FFT.CascadeSizes` of three is dropped for the four-cascade default: three
    // authored patches say nothing of where a fourth band would start. An array of any other length is not v2's and
    // is left to the typed read. The set is written through the engine's own
    // writer, which states WAVS 3; the result must read back through ParseWaterWaves or the step fails, naming why.
    struct WaterWavesRaised
    {
        std::string              Text;
        std::vector<std::string> Dropped; // the retired members as the file stated them, for the report
    };
    Common::ResultStr<WaterWavesRaised> MigrateWaterWavesToV3( const std::string& text );
} // namespace Desert::Migration
