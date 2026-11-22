#pragma once

#include <Common/Content/DerivedDataCache.hpp>
#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace Desert
{
    class SkinnedMesh;
}
namespace Desert::Animation
{
    class AnimationClip;
}

namespace Desert::Assets
{
    /**
     * @brief THE FAR CROWD'S VERTEX ANIMATION AS DERIVED DATA (WP-FAR-12B; UE 5.5 AnimToTexture / City Sample
     *        crowds: the vertex animation is built OFFLINE from the skeletal mesh and the clip, holding only the
     *        level that draws it; the runtime only samples it).
     *
     * One entry per skinned mesh x clip: the clip sampled at kVertexAnimationFramesPerSecond over its length, each
     * sample's skinning matrices applied on the CPU to the mesh's LOWEST level's vertices exactly as the skin pass
     * applies them (SkinCache.shader: posed position, unnormalised normal / tangent / bitangent). Only that
     * level's vertices are stored - CesiumMan's 27, not its 5358 - in the GPU skin cache's planar stream layout
     * over a vertex RANGE (Common/SkinCacheStreams.glslh), so the far draw fetches a frame as it fetches the live
     * cache.
     *
     * THE KEY: the deriver's version, and a payload hash of everything the bake reads - the lowest level's vertex
     * bytes and its range, the rig as read (Skeleton::GetContentSignature: structure, binds and inverse binds) and
     * the clip's content (its `.anim` body as the writer spells it, header and identity left out) - plus the
     * sampling rate as the settings image. A reimport that changes the level, a re-rig or a clip edit is another
     * key; the same content under two names is one entry.
     *
     * WHERE IT IS BUILT: the package cook derives every skinned mesh x clip that plays on it (PackageCook.cpp
     * CookVertexAnimations) into the bucket the package ships (DDCShipRegister.hpp), and the editor may derive on
     * demand into the DDC (SetVertexAnimationDeriving, EditorStartup). A packaged game registers nothing: it only
     * loads, and a miss is an error (the figure stays skeletal), never a runtime bake.
     */
    inline constexpr Common::DDC::Deriver kVertexAnimationDeriver{
         "VertexAnimation", ".devat", { 0x3C9E57A1D2864B0FULL, 0x8A61F0C4B75E2D93ULL } };
    inline constexpr float kVertexAnimationFramesPerSecond = 30.0f;

    // The entry: this header, then Frames slots of 15 * VertexCount floats each (positions, previous positions =
    // the frame before in the loop, normals, tangents, bitangents; 3 floats a vertex, one plane after another).
    struct VertexAnimationHeader
    {
        uint32_t Magic           = 0x54415644u; // "DVAT"
        uint32_t Version         = 1u;
        uint32_t Frames          = 0;
        uint32_t FirstVertex     = 0; // the baked level's first vertex in the mesh's vertex order (gl_VertexIndex)
        uint32_t VertexCount     = 0; // the baked level's vertex count
        float    FramesPerSecond = kVertexAnimationFramesPerSecond;
    };
    static_assert( sizeof( VertexAnimationHeader ) == 24 );
    inline constexpr uint64_t kVertexAnimationFloatsPerVertex = 15u;

    [[nodiscard]] Common::ResultStr<uint64_t> VertexAnimationKey( const SkinnedMesh&              mesh,
                                                                  const Animation::AnimationClip& clip );

    // The entry's bytes (header + slots), built on the CPU from the mesh's vertices, rig and the clip.
    [[nodiscard]] Common::ResultStr<std::string> BuildVertexAnimation( const SkinnedMesh&              mesh,
                                                                       const Animation::AnimationClip& clip );

    // The header of @p bytes, refused when the bytes are not a whole entry of this version.
    [[nodiscard]] Common::ResultStr<VertexAnimationHeader> ReadVertexAnimationHeader( std::string_view bytes );

    // Whether a DDC miss may be derived here (the editor and the cook: yes; a packaged game: no, the default).
    void SetVertexAnimationDeriving( bool deriving );

    struct LoadedVertexAnimation
    {
        std::string Bytes;
        uint64_t    Key        = 0;
        bool        DerivedNow = false; // false = found in the DDC (or the package's cooked tree)
    };
    // The DDC entry for the pair; on a miss, derived and Put where deriving is allowed, an error otherwise.
    [[nodiscard]] Common::ResultStr<LoadedVertexAnimation>
    LoadVertexAnimation( const SkinnedMesh& mesh, const Animation::AnimationClip& clip );
} // namespace Desert::Assets
