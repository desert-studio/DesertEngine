#pragma once

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/DerivedDataCache.hpp>
#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

// THE GRASS COVER'S LOOK AS DERIVED DATA (GRASS-ASSET p2). The landscape's far grass cover
// (Common/LandscapeGrassCover.glslh) carries on the blades the generator thins out with distance, so its colour is
// the blades' own: the mean of the variety material's albedo map over the texels the blades actually draw - inside
// the blade cards on the atlas, where the material's opacity mask passes its clip value (a mesh variety: the whole
// map under its mask). No author types it and no script measures it: it is cooked like UE's DDC entries, from the
// texture's platform data as the GPU samples it (the cooked chain, BC blocks decoded), keyed by the textures'
// derived-data keys, the mask's channel and clip and the cards. The decoder lives in the editor
// (SetGrassCoverLookBuilder); a packaged game reads the shipped bucket.
namespace Desert::Assets
{
    class AssetManager;

    inline constexpr Common::DDC::Deriver kGrassCoverLookDeriver{
         "GrassCoverLook", ".grasscover", { 0x6A1F3C92D47E0B85ULL, 0x93C5E2170FA84D6BULL } };
    // 3: + the cards' hulls (FPS-GRASS-HULL).
    inline constexpr uint32_t kGrassCoverLookBuilderVersion = 3;

    // What the look is derived from: the material's albedo map (its template's first Texture2D of Intent(Colour))
    // and the opacity mask its template cuts with (BlendMode Masked's OpacityMask), both as their texture assets.
    struct GrassCoverLookInput
    {
        std::filesystem::path AlbedoAsset;
        uint64_t              AlbedoKey = 0; // the texture's derived-data key: source content + import settings
        std::filesystem::path MaskAsset;     // empty = the template cuts nothing: every texel in the rects counts
        uint64_t              MaskKey     = 0;
        uint32_t              MaskChannel = 3;
        float                 MaskClip    = 0.0f;
        // The atlas rectangles the blades draw (u0, v0, u1, v1; UV0 as the GPU samples it: v = 0 is the first
        // row); a mesh variety: the whole map.
        std::vector<glm::vec4> Rects;
    };

    // The derived look, as the drawn cards put it into the world: every card stands as tall as the blade and as
    // wide as its rect's aspect (GrassBlade.glslh BladeAt), and the variety draws its cards uniformly, so a card's
    // world area is its aspect and each of its texels stands for aspect / (the rect's texels) of it. Albedo is the
    // mean linear albedo of the drawn texels weighted by that world area; Coverage is the share of the cards'
    // world area the mask keeps (sum aspect * kept share / sum aspect): a strip is a blade only where its mask
    // passes.
    //
    // THE CARDS' HULLS (FPS-GRASS-HULL; UE Cascade/Niagara SubUV cutout: the bounding geometry of each frame's opaque
    // texels, cooked from the cutout texture). Per card, per height band (t = 0 root .. 1 tip, kGrassCardHullBands
    // even bands), the span across the card (side -1 at u0 .. +1 at u1, BladeCardUV) outside which the opacity mask
    // cannot pass its clip: the side range of every mask texel that reaches the clip, widened by how far a fragment
    // can read it from - one texel of its own level (a bilinear tap) plus the spread of one pixel's taps (2^(c-1)
    // level-0 texels in class c; the last class: 8 texels of the level, a 16x anisotropic pixel). Trilinear and
    // anisotropic filtering average such taps, so a value under the clip at every tap stays under it. Per class c
    // the union of the mask's levels 0..c (the last class: every level) - the vertex stage picks the class by the coarsest level the blade can
    // sample on screen (Vertex_GrassBlade.glslh BladeHullSide). A band no texel reaches is empty (lo > hi).
    inline constexpr uint32_t kGrassCardHullBands   = 8;
    inline constexpr uint32_t kGrassCardHullClasses = 4;
    struct GrassCardHull
    {
        std::array<std::array<glm::vec2, kGrassCardHullBands>, kGrassCardHullClasses> Bands{}; // [class][band] lo, hi
    };

    struct GrassCoverLook
    {
        glm::vec3 Albedo{ 0.0f };
        float     Coverage = 0.0f;
        // One per input rect, in its order; empty when the template cuts nothing (every card is its full rect).
        std::vector<GrassCardHull> Hulls;
        // The mask's level-0 size in texels (the vertex stage's texel density); 0 = no mask.
        glm::uvec2 MaskSize{ 0u };
    };

    // The input of the section drawn with @p material: nullopt when its template declares no Colour map or the
    // material assigns none (a section with no map has no texture look to carry on).
    [[nodiscard]] Common::ResultStr<std::optional<GrassCoverLookInput>>
    ResolveGrassCoverLookInput( AssetManager& manager, const Common::Content::AssetGuid& material,
                                std::span<const glm::vec4> rects );

    // Builder version + both textures' keys + the mask's channel and clip + the rects -> the key.
    [[nodiscard]] uint64_t GrassCoverLookKey( const GrassCoverLookInput& input );

    // One decoded texture level as the GPU samples it: linear RGBA, row-major.
    struct GrassCoverLookLevel
    {
        uint32_t                   Width  = 0;
        uint32_t                   Height = 0;
        std::span<const glm::vec4> Texels;
    };
    // The look from the decoded levels (the builder's arithmetic, shared so every builder computes the same):
    // every albedo texel whose centre lies in a rect, weighted by its mask texel passing the clip.
    [[nodiscard]] GrassCoverLook AverageGrassCoverLook( const GrassCoverLookInput&                input,
                                                        const GrassCoverLookLevel&                albedo,
                                                        const std::optional<GrassCoverLookLevel>& mask );

    // The cards' hulls from the mask's whole decoded chain (level 0 first), as the GPU samples it.
    [[nodiscard]] std::vector<GrassCardHull> ComputeGrassCardHulls( const GrassCoverLookInput&           input,
                                                                    std::span<const GrassCoverLookLevel> chain );

    [[nodiscard]] std::string                       EncodeGrassCoverLook( const GrassCoverLook& look );
    [[nodiscard]] Common::ResultStr<GrassCoverLook> DecodeGrassCoverLook( std::string_view bytes );

    // Where a builder is registered (the editor, at startup) a miss builds and Puts; a packaged game registers
    // none, and a miss there is an error naming the albedo map and the key. Returns EncodeGrassCoverLook bytes.
    using GrassCoverLookBuilder = std::function<Common::ResultStr<std::string>( const GrassCoverLookInput& )>;
    void SetGrassCoverLookBuilder( GrassCoverLookBuilder builder );

    // DDC hit, or build (single-flight per key) and Put.
    [[nodiscard]] Common::ResultStr<GrassCoverLook> LoadGrassCoverLook( const GrassCoverLookInput& input );
} // namespace Desert::Assets
