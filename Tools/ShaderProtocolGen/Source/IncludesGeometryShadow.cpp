#include "IncludeLists.hpp"

#include <Engine/Graphic/GpuDriven/GpuSceneLayout.hpp>
#include <Engine/Graphic/GpuInstancesLayout.hpp>
#include <Engine/Graphic/LightingLayout.hpp>
#include <Engine/Graphic/Materials/SceneResourcesLayout.hpp>
#include <Engine/Graphic/Systems/Scene/Decals/DecalLayout.hpp>
#include <Engine/Graphic/Systems/Scene/Grass/GrassLayout.hpp>
#include <Engine/Graphic/Systems/Scene/Shadows/DistanceFieldShadowLayout.hpp>
#include <Engine/Graphic/Systems/Scene/Shadows/MegaLightsLayout.hpp>
#include <Engine/Graphic/Systems/Scene/Shadows/VirtualShadowMapLayout.hpp>
#include <Engine/Graphic/Systems/Scene/Terrain/TerrainLayout.hpp>
#include <Engine/Graphic/VirtualGeometry/VirtualGeometryLayout.hpp>

namespace Desert::ShaderProtocolGen
{
    // Run B: GPU-driven scene, virtual geometry, instance draws, the light grid and local shadow views, light
    // functions, the shadow UB, virtual shadow maps, MegaLights, distance field shadows, terrain, decals, grass.
    std::span<const ShaderIncludeDecl* const> GeometryShadowIncludes()
    {
        static constexpr const ShaderIncludeDecl* kIncludes[] = {
             &Graphic::GpuDriven::kGpuSceneShaderInclude,
             &Graphic::VirtualGeometry::kVirtualGeometryShaderInclude,
             &Graphic::kGpuInstancesShaderInclude,
             &Graphic::kLightGridShaderInclude,
             &Graphic::kLocalShadowViewShaderInclude,
             &Graphic::System::kLightFunctionShaderInclude,
             &Graphic::SceneResources::kShadowUBShaderInclude,
             &Graphic::System::kVirtualShadowMapShaderInclude,
             &Graphic::System::kMegaLightsShaderInclude,
             &Graphic::System::kDistanceFieldShadowShaderInclude,
             &Graphic::System::kTerrainShaderInclude,
             &Graphic::System::kDecalShaderInclude,
             &Graphic::System::kGrassShaderInclude,
        };
        return kIncludes;
    }
} // namespace Desert::ShaderProtocolGen
