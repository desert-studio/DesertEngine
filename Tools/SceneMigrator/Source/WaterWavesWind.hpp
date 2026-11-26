#pragma once

#include <Common/Core/ResultStr.hpp>

#include <string>

namespace Desert::Migration
{
    // WAVS 1 -> 2 (WATER-UNDERWATER; UE AWaterBody takes its waves' direction from the wind source, not the
    // asset): a `.dwaves` no longer states `Generator.WindAngleDeg` - the Gerstner waves run with the scene's wind
    // (ECS::WindAt at the body, WaterWavesService::SetWind), as the FFT ocean already did. The step drops the
    // member and writes the set through the engine's own writer, which states WAVS 2; the result must read back
    // through ParseWaterWaves or the step fails, naming why. A file stating anything but WAVS 1 is refused.
    Common::ResultStr<std::string> MigrateWaterWavesV1ToV2( const std::string& text );
} // namespace Desert::Migration
