#pragma once

#include <Engine/Core/Formats/SamplerState.hpp>
#include <Engine/Graphic/BindlessTextureTable.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingProgramHit.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneMetadata.hpp>

#include <glm/glm.hpp>

#include <bit>
#include <cstddef>
#include <string>
#include <string_view>

// 3D TEXT IN THE RAY TRACING SCENE (UE: UTextRenderComponent's proxy puts its glyph mesh in the ray tracing scene
// with its font material - the SDF coverage is the material's opacity mask, run as the any hit). A label's glyph
// quads enter the TLAS as its generic record (TextECSSystem: VisibleInRayTracing), shaded by
// Programs/Text/TextRayHit.shader from the label's row; their inline alpha test reads the same atlas.
namespace Desert::Graphic::RayTracing
{
    // The text's hit group: the `RayTracing.Hit` pass of Programs/Text/TextRayHit.shader.
    inline constexpr std::string_view kTextRayHitTemplate = "TextRayHit";

    // The sampler the rays read the font's SDF atlas through: linear, clamped (a glyph's texels never reach past
    // the atlas edge), as the raster samples a distance field.
    inline constexpr Core::Formats::SamplerState kTextAtlasRaySampler{ .WrapU = Core::Formats::SamplerWrap::Clamp,
                                                                       .WrapV = Core::Formats::SamplerWrap::Clamp,
                                                                       .Filter =
                                                                            Core::Formats::SamplerFilter::Linear };

    // The hit group's row, as Programs/Text/TextRayHit.glslh TextRayHitRowRef declares it: TextColor,
    // (EmissiveIntensity, 0, 0, 0), (atlas bindless slot, its sampler slot, 0, 0) as bits.
    inline constexpr std::size_t kTextRayHitRowSlots = 3;

    // Does a label of @p textColor stop rays at all? The raster blends coverage x TextColor.a; a masked surface is
    // there where that passes 0.5 (UE OpacityMaskClipValue), so a label at alpha under 0.5 is clear to every ray.
    // The inside of a glyph is coverage 1 on the raster; the outline (median 0.5) is where the ray's mask flips.
    [[nodiscard]] inline bool TextRayOpaque( const glm::vec4& textColor )
    {
        return textColor.a >= 0.5f;
    }
    // The inline cutoff of a label no ray stops: above any median an 8-bit atlas holds.
    inline constexpr float kNeverCovered = 2.0f;

    // A label's program hit: @p textColor and @p emissiveIntensity are the TextSDF values its raster draw carries,
    // @p atlas the font's SDF atlas it samples. The opacity is the glyph coverage: the median of the atlas's three
    // distance channels against the outline (kMaterialFlagSdfMedian), from both faces (the quads draw Cull None).
    [[nodiscard]] inline RayTracingProgramHit TextRayHit( const glm::vec4& textColor,
                                                          const float emissiveIntensity, const Image2D& atlas )
    {
        const uint32_t       atlasSlot = atlas.GetBindlessSlot().Index;
        const uint32_t       sampler   = BindlessSamplerFor( kTextAtlasRaySampler ).Index;
        RayTracingProgramHit hit;
        hit.Template = std::string( kTextRayHitTemplate );
        hit.Row      = Core::Formats::MaterialParamRow( kTextRayHitRowSlots );
        hit.Row[0]   = textColor;
        hit.Row[1]   = glm::vec4( emissiveIntensity, 0.0f, 0.0f, 0.0f );
        hit.Row[2]   = glm::vec4( std::bit_cast<float>( atlasSlot ), std::bit_cast<float>( sampler ), 0.0f, 0.0f );
        hit.Opacity  = RayTracingMaterialRecord{ .OpacityTexture = atlasSlot,
                                                 .OpacitySampler = sampler,
                                                 .OpacityCutoff = TextRayOpaque( textColor ) ? 0.5f : kNeverCovered,
                                                 .Flags         = kMaterialFlagTwoSided | kMaterialFlagSdfMedian };
        return hit;
    }
} // namespace Desert::Graphic::RayTracing
