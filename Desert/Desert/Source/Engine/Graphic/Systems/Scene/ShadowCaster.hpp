#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Geometry/LODSelection.hpp>
#include <Engine/Graphic/ShadowCascades.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace Desert::Graphic
{
    class RenderPassDeclaration;
}

namespace Desert::Graphic::RDG
{
    class PassContext;
}

namespace Desert::Graphic::System
{
    /**
     * @brief A renderer whose geometry casts into the MeshRenderer's shadow views: the sun's cascades and the
     * point/spot lights' views of the local shadow atlas (Graphic/LocalShadows.hpp), one index space
     * (MeshRenderer: 0..kMaxCascades-1 the cascades, the rest the local views).
     *
     * The cascade targets and their depth passes belong to the MeshRenderer, which clears each cascade once
     * and records its own casters; a caster that is not a mesh (the tessellated terrain) is recorded INSIDE
     * that same pass rather than in a second graph pass on the same target. A second pass would need an
     * order against the first inside one render phase, and the graph gives none: whichever began the
     * target last would clear the other's depth.
     *
     * The same holds for a local view: its tile is cleared and its mesh casters drawn in one pass, and the
     * non-mesh casters are recorded inside it.
     *
     * Called once per drawn view per frame, with the view's render pass open, only when shadows are on.
     * `casterViewProj` is the matrix the CASTERS draw the view with (standard-Z, clip from world): the
     * receivers' matrix with the light's constant depth bias folded in (Graphic::CasterViewProj), so a caster
     * that projects with it is biased like every mesh caster without knowing the bias exists. `bias` is the
     * same view's CascadeCasterBias (zero for a local view: its bias is the receiver's); its DepthBias is the one
     * already in `casterViewProj`; its SlopeBias / SlopeClamp are the slope term the caster's VERTEX stage adds
     * (Common/ShadowCasterBias.glslh: the mesh cells' vertex headers, Programs/Terrain/TerrainVertex.glslh for the
     * terrain).
     */
    // FNV-1a over raw bytes: a shadow cache key (local views, the cascades' static layer) only has to change when
    // what was drawn changes. One hash for the mesh casters and the non-mesh ones (LocalShadowSignature), so a
    // light's key is one value however many renderers fed it.
    struct ShadowSignatureHash
    {
        uint64_t Value = 1469598103934665603ull;

        void Bytes( const void* data, const std::size_t size )
        {
            const auto* bytes = static_cast<const unsigned char*>( data );
            for ( std::size_t i = 0; i < size; ++i )
            {
                Value ^= bytes[i];
                Value *= 1099511628211ull;
            }
        }
        template <typename T>
        void Add( const T& value )
        {
            Bytes( &value, sizeof( T ) );
        }
    };

    // One point/spot light's shadow views as a caster sees them (MeshRenderer::UpdateLocalShadows): which views
    // of the one index space are its own, where it reaches, and the view a caster with levels of detail chooses
    // them from - the light's position and its views' projection, never the main camera. A local view's depth is
    // cached until something in its reach changes, so detail picked from the camera would redraw every local
    // tile the camera moves past; picked from the light, the cached depth holds while the light holds.
    struct LocalShadowCasterLight
    {
        uint32_t          FirstView = 0; // its first view (kMaxShadowCascades + its first row of the view table)
        uint32_t          ViewCount = 0; // 1 for a spot, kPointShadowFaces for a point
        float             Reach     = 0.0f; // the point's radius / the spot's range, cm, around Lod.Origin
        Geometry::LODView Lod;           // Origin = the light's position; Projection = its views' (one per light)

        [[nodiscard]] bool Owns( const uint32_t view ) const
        {
            return view >= FirstView && view < FirstView + ViewCount;
        }
    };

    class IShadowCaster
    {
    public:
        virtual ~IShadowCaster() = default;

        // Whether this caster has anything to draw into the cascades this frame (asked before the graph is built,
        // after the frame's draws were submitted). A non-mesh caster is drawn in the cascades' MOVABLE layer,
        // every frame it casts, so a caster that casts nothing must say so or no cascade could ever stay cached.
        [[nodiscard]] virtual bool CastsShadowThisFrame() const = 0;

        // THE LOCAL VIEWS' CACHE KEY (UE: a stationary light's cached shadow depth is kept until a caster in its
        // reach changes). A hash of what this caster draws into @p light's views — every input of its depth there:
        // placement, shape, the geometry's level of detail (chosen from the light, LocalShadowCasterLight), the
        // contents of the textures its depth reads — or nullopt when what it draws there changes by itself every
        // frame. Asked after the frame's draws were resolved (for this frame's lights), before the local views'
        // nodes are added; a caster with nothing in reach returns the hash of nothing (a constant), never nullopt.
        [[nodiscard]] virtual std::optional<uint64_t>
        LocalShadowSignature( const LocalShadowCasterLight& light ) const = 0;

        // SETUP: declares on the view's node one binding block per material this caster draws the view with
        // (the shader's reflected layout and the material's route fill), so ValidatePassBindings judges the
        // caster before anything is recorded. The blocks follow the ones already declared on the node; returns
        // how many it declared (0: nothing to draw this frame).
        [[nodiscard]] virtual uint32_t DeclareShadowView( RenderPassDeclaration& declared, uint32_t view ) = 0;

        // Draws through the blocks DeclareShadowView declared, the first of them at @p firstBlock
        // (RDG::PassBindings( context, context.GetBindingBlock( firstBlock + i ) )); the first refused draw is the
        // error.
        [[nodiscard]] virtual Common::BoolResultStr RecordShadowView( const RDG::PassContext& context,
                                                                      uint32_t view, uint32_t firstBlock,
                                                                      const glm::mat4&         casterViewProj,
                                                                      const CascadeCasterBias& bias ) = 0;
    };
} // namespace Desert::Graphic::System
