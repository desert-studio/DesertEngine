#include "IncludeLists.hpp"

#include <Engine/Graphic/Clouds/CloudLayout.hpp>
#include <Engine/Graphic/Deformation/DeformationLayout.hpp>
#include <Engine/Graphic/Environment/SkyLookLayout.hpp>
#include <Engine/Graphic/Fog/FogLayout.hpp>
#include <Engine/Graphic/Materials/Debug/MaterialCubemapSphereLayout.hpp>
#include <Engine/Graphic/RainOcclusionLayout.hpp>
#include <Engine/Graphic/SkyLayout.hpp>
#include <Engine/Graphic/Systems/Scene/Weather/WeatherLayout.hpp>

#include <array>

namespace Desert::ShaderProtocolGen
{
    namespace
    {
        constexpr std::array<const ShaderIncludeDecl*, 15> kAtmosphereIncludes = {
             &Graphic::kFogParamsShaderInclude,
             &Graphic::kVolumetricFogShaderInclude,
             &Graphic::kHeterogeneousVolumeShaderInclude,
             &Graphic::kCloudParamsShaderInclude,
             &Graphic::kCloudResolveParamsShaderInclude,
             &Graphic::kCloudAuthoredShaderInclude,
             &Graphic::kCloudShadowShaderInclude,
             &Graphic::kCloudSelfShadowShaderInclude,
             &Graphic::Deformation::kDeformationFieldParamsShaderInclude,
             &Graphic::kSkyLookShaderInclude,
             &Graphic::kCubemapSphereShaderInclude,
             &Graphic::kRainOcclusionShaderInclude,
             &Graphic::kSkyPackedShaderInclude,
             &Graphic::kSkyBufferShaderInclude,
             &Graphic::System::kRainStreaksShaderInclude,
        };
    } // namespace

    std::span<const ShaderIncludeDecl* const> AtmosphereIncludes()
    {
        return kAtmosphereIncludes;
    }
} // namespace Desert::ShaderProtocolGen
