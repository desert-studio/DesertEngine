#pragma once

// THE TWO SHADER NAMES THAT MEAN "THE PBR SURFACE". Every other name a material states is a DSL shader drawn by
// DataDrivenMaterial through the generic per-object path, which has no instanced variant. One statement, read by
// the material asset (SurfaceMaterialAsset::UsesCustomShader) and by the world cook, which has to know the same
// thing from the asset registry without loading the material (WorldCells::CustomShaderFrom).

#include <string_view>

namespace Desert::Assets
{
    inline constexpr std::string_view kStaticMeshPBRShader  = "StaticMeshPBR";
    inline constexpr std::string_view kSkinnedMeshPBRShader = "SkinnedMeshPBR";

    [[nodiscard]] constexpr bool IsPBRSurfaceShader( std::string_view name )
    {
        return name == kStaticMeshPBRShader || name == kSkinnedMeshPBRShader;
    }
} // namespace Desert::Assets
