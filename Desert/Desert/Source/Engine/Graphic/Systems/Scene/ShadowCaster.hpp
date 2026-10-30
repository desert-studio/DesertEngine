#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Geometry/LODSelection.hpp>
#include <Engine/Graphic/ShadowCascades.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <functional>
#include <vector>

namespace Desert::Graphic
{
    class RenderPassDeclaration;
}

namespace Desert::Graphic::RDG
{
    class PassContext;
    class PassBuilder;
    class BindingBlockBuilder;
    struct ShaderBindingLayout;
} // namespace Desert::Graphic::RDG

namespace Desert::Graphic::System
{
    // One thing a caster draws into the sun's virtual shadow map: a hash of what it draws there (it changes
    // whenever the depth it writes would) and its world box. The virtual shadow map keeps its pages across frames,
    // so the mesh renderer diffs these between frames (UE: a primitive added, removed or updated invalidates the
    // pages its bounds cover) - a caster that changed drops the cached pages under its old and its new box, and
    // nothing else.
    struct ShadowCasterEntry
    {
        uint64_t  Signature = 0;
        glm::vec3 Min{ 0.0f };
        glm::vec3 Max{ 0.0f };
    };

    // One draw a caster records into a virtual shadow map level, for the level's GPU cull ("VSM: Cull Casters"):
    // its world box and its indirect argument record (Graphic/MeshDrawArguments.hpp: a VkDrawIndirectCommand in
    // the first four words, its instance count 0 - the cull counts the draw's one instance in where its box covers
    // a page drawn this frame).
    struct VirtualShadowCasterDraw
    {
        glm::vec3               Min{ 0.0f };
        glm::vec3               Max{ 0.0f };
        std::array<uint32_t, 5> Arguments{};
    };

    // The virtual shadow map's pass parameters, added to every binding block a caster declares on a "VSM: Raster"
    // node (the frame data, the page table, the physical pool and the level), handed the block's layout.
    using VirtualShadowBlockParameters =
         std::function<void( RDG::BindingBlockBuilder&, const RDG::ShaderBindingLayout& )>;

    /**
     * @brief A renderer whose geometry casts into the MeshRenderer's shadow views: the sun's cascades and the
     * point/spot lights' views of the local shadow atlas (Graphic/LocalShadows.hpp), one index space
     * (MeshRenderer: the fixed views first - 0..kMaxCascades-1 the cascades, then their movable layers, the
     * virtual shadow map levels and the rain occlusion - and from MeshRenderer::kLocalShadowViewBase the local
     * views, as many as the frame shadows).
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
     * same view's CascadeCasterBias (a local view's: the light's, LocalCasterBias); its DepthBias is the one
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
        uint32_t          FirstView = 0; // its first view (kLocalShadowViewBase + its first row of the view table)
        uint32_t          ViewCount = 0; // 1 for a spot, kPointShadowFaces for a point
        float             Reach     = 0.0f; // the point's radius / the spot's range, cm, around Lod.Origin
        Geometry::LODView Lod; // Origin = the light's position; Projection = its views' (one per light)

        [[nodiscard]] bool Owns( const uint32_t view ) const
        {
            return view >= FirstView && view < FirstView + ViewCount;
        }
    };

    class IShadowCaster
    {
    public:
        virtual ~IShadowCaster() = default;

        // Whether this caster has anything to draw into a fixed view this frame (asked before the graph is built,
        // after the frame's draws were submitted): the rain occlusion view's caster list.
        [[nodiscard]] virtual bool CastsShadowThisFrame() const = 0;

        // THE SUN CASCADES' CACHE KEY (UE: a static landscape is drawn into the cached whole-scene shadow, and its
        // depth redrawn only when the landscape, the light or the view's cascade changes). A hash of what this
        // caster draws into cascade view @p view - every input of its depth there: placement, shape, level of
        // detail, the contents of the textures its depth reads - or nullopt when what it draws there changes by
        // itself every frame. A keyed caster is drawn into the cascade's STATIC layer (only on a frame its key,
        // or the cascade's, changed), an unkeyed one into its movable layer every frame. Asked after this
        // frame's draws into the cascades were resolved (TerrainRenderer::PrepareLocalShadowViews), by
        // MeshRenderer::PlanCascadeCache; a caster with nothing to draw returns the hash of nothing (a constant),
        // never nullopt.
        [[nodiscard]] virtual std::optional<uint64_t> CascadeShadowSignature( uint32_t view ) const = 0;

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

        // THE SUN'S VIRTUAL SHADOW MAP ("VSM: Raster", one attachment-less node per clipmap level). Its pages are
        // cached across frames, so the caster says what it draws (AppendVirtualShadowEntries, before the graph is
        // built) and the mesh renderer invalidates the pages of whatever changed; it is drawn into every level
        // each frame, and only the dirty pages take its depth (Common/VirtualShadowRaster.glslh).
        virtual void AppendVirtualShadowEntries( std::vector<ShadowCasterEntry>& entries ) const = 0;

        // The draws RecordVirtualShadow records this frame, in its order (the same for every level): what the
        // level's cull keeps or drops (UE CullPerPageDrawCommands). Asked in each level's setup.
        virtual void AppendVirtualShadowDraws( std::vector<VirtualShadowCasterDraw>& draws ) const = 0;

        // SETUP of a level's node: one binding block per material this caster draws the level with, each carrying
        // @p parameters; the blocks follow the ones already declared on the node. Returns how many it declared
        // (0: nothing to draw). A caster that cannot draw into the virtual shadow map (its program or pipeline
        // refused) logs why once and declares none.
        [[nodiscard]] virtual uint32_t DeclareVirtualShadow( RDG::PassBuilder&                   pass,
                                                             const VirtualShadowBlockParameters& parameters ) = 0;

        // Draws the level through the blocks DeclareVirtualShadow declared (the first at @p firstBlock),
        // projecting with @p levelViewProj (world -> the level's standard-Z clip; no caster bias: the virtual
        // shadow map biases its receivers, VirtualShadowMapProject), draw k from the culled record @p firstRecord
        // + k of @p arguments (the node reads it as Access::IndirectArgs).
        [[nodiscard]] virtual Common::BoolResultStr
        RecordVirtualShadow( const RDG::PassContext& context, uint32_t firstBlock, const glm::mat4& levelViewProj,
                             RDG::BufferRef arguments, uint32_t firstRecord ) = 0;
    };
} // namespace Desert::Graphic::System
