#pragma once

#include <Common/Core/Entt.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/UUID.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <CoreReflection/ReflectionMacros.hpp>

#include <Engine/Assets/Common.hpp>
#include <Engine/ECS/RenderCacheRetire.hpp>

namespace Desert::Graphic::Render
{
    struct GenericMeshDrawParams; // DrawGenericMeshCommand.hpp
}

namespace Desert::Runtime
{
    struct Font; // FontService.hpp
}

namespace Desert
{
    class DynamicMesh;
} // namespace Desert

namespace Desert::ECS
{
    // World-space SDF text. FontService bakes the .ttf into an SDF atlas; TextECSSystem lays the
    // string out into a per-entity quad mesh and draws it through the generic path with the TextSDF
    // shader — into the HDR composite, so EmissiveIntensity > ~1 blooms like any emissive surface.
    struct TextComponent
    {
        std::string         Text = "Text";
        Assets::AssetHandle Font; // SDF font asset (drag a .ttf or pick a preloaded one);
                                  // unset = the engine's built-in default (Roboto).
        glm::vec4 Color             = glm::vec4( 1.0f );
        float     Size              = 1.0f;  // world units per em (scales the baked metrics)
        float     EmissiveIntensity = 1.0f;  // >1 => the text blooms
        bool      Billboard         = false; // face the camera (added by the system per frame)

        // Transient: the label's resolve (UE: FSlateFontInfo -> cached FFontData, the shaped text cached until the
        // string or style changes). TextECSSystem redoes it only when the authored string, the font asset, the
        // language (Localization::Generation) or the font cache (FontService::Generation) moved; a steady frame
        // compares these and resolves nothing, decodes nothing and allocates nothing.
        std::string                    ResolvedSource;    // the authored Text ResolvedText came from
        std::string                    ResolvedText;      // what is drawn (the localised string)
        Assets::AssetHandle            ResolvedFontAsset; // the authored Font it was resolved for
        uint32_t                       ResolvedLocaleGeneration = 0;
        uint64_t                       ResolvedFontGeneration   = 0;
        std::shared_ptr<Runtime::Font> ResolvedFont; // co-owned: valid however the cache moves on

        // Transient: the laid-out glyph-quad mesh, rebuilt only when the text/font/size changes.
        std::shared_ptr<DynamicMesh> RuntimeMesh;
        std::string                  BuiltText;
        float                        BuiltSize = 0.0f;
        uint64_t                     BuiltFontRevision = 0; // Runtime::Font::Revision the mesh's UVs were laid on

        // Transient: the draw's material overrides + ray hit, rebuilt only when the colour, the emission or the
        // font bake moves. The draw names it by raw pointer, so a rebuild retires the old one (RetireRenderCache).
        std::shared_ptr<const Graphic::Render::GenericMeshDrawParams> RuntimeDraw;
        glm::vec4                                                     DrawColor             = glm::vec4( 0.0f );
        float                                                         DrawEmissiveIntensity = 0.0f;
        uint64_t                                                      DrawFontRevision      = 0;
        uint64_t DrawAtlasGeneration = 0; // the atlas's Image2D::GetResourceGeneration (its bindless slot's life)
    };

    inline void RetireRenderCachesOf( const TextComponent& c )
    {
        RetireRenderCache( c.RuntimeMesh );
        RetireRenderCache( c.RuntimeDraw );  // the draw's overrides + ray hit, named by raw pointer
        RetireRenderCache( c.ResolvedFont ); // its atlas is the draw's DirectTexture
    }
} // namespace Desert::ECS
