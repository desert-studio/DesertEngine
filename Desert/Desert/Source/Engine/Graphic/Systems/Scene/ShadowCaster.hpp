#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/ShadowCascades.hpp>

#include <glm/glm.hpp>

#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <array>
#include <cstdint>
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

    // What a caster puts into one point / spot light's shadow views this frame (UE: a primitive's relevance to a
    // light's whole-scene shadow, and whether it is movable): MeshRenderer keys the light's cached atlas tiles by
    // it.
    struct LocalShadowReach
    {
        bool Casts  = false; // it draws into the light's views (Declare/RecordShadowCascade with their index)
        bool Moving = false; // what it draws there changes every frame: the light's views are redrawn each frame
        uint64_t Signature = 0; // otherwise, a key of what it draws there: a change redraws the views
    };

    /**
     * @brief A renderer whose geometry casts into the sun's cascaded shadow map, and (when it says so,
     * LocalShadowReachOf) into the point / spot lights' views of the local shadow atlas.
     *
     * The cascade targets and their depth passes belong to the MeshRenderer, which clears each cascade once
     * and records its own casters; a caster that is not a mesh (the tessellated terrain) is recorded INSIDE
     * that same pass rather than in a second graph pass on the same target. A second pass would need an
     * order against the first inside one render phase, and the graph gives none: whichever began the
     * target last would clear the other's depth.
     *
     * THE VIEW INDEX. `cascade` below is MeshRenderer's shadow view index: 0..kMaxShadowCascades-1 a sun cascade,
     * kMaxShadowCascades + slot a local light's view (a spot, or one cube face of a point;
     * Graphic/LocalShadows.hpp). A local view's pass is "MeshShadowLocal<slot>" on the local atlas, its viewport
     * the view's tile, and its `bias` zero (a local view's bias is the receiver's, Mesh/LocalLightShadow.glslh);
     * it is called only for a caster whose LocalShadowReachOf the light Casts, and only on the frames the light's
     * views are redrawn.
     *
     * Called once per cascade per frame, with the cascade's render pass open, only when shadows are on.
     * `cascadeViewProj` is the matrix the CASTERS draw the cascade with (standard-Z, clip from world): the
     * receivers' matrix with the light's constant depth bias folded in (Graphic::CasterViewProj), so a caster
     * that projects with it is biased like every mesh caster without knowing the bias exists. `bias` is the
     * same cascade's CascadeCasterBias: its DepthBias is the one already in `cascadeViewProj`; its SlopeBias /
     * SlopeClamp are the slope term the caster's VERTEX stage adds (Common/ShadowCasterBias.glslh: the mesh
     * cells' vertex headers, Programs/Terrain/TerrainVertex.glslh for the terrain).
     */
    class IShadowCaster
    {
    public:
        virtual ~IShadowCaster() = default;

        // Whether this caster has anything to draw into the cascades this frame (asked before the graph is built,
        // after the frame's draws were submitted). A non-mesh caster is drawn in the cascades' MOVABLE layer,
        // every frame it casts, so a caster that casts nothing must say so or no cascade could ever stay cached.
        [[nodiscard]] virtual bool CastsShadowThisFrame() const = 0;

        // SETUP: declares on the cascade node one binding block per material this caster draws the cascade with
        // (the shader's reflected layout and the material's route fill), so ValidatePassBindings judges the
        // caster before anything is recorded. The blocks follow the ones already declared on the node; returns
        // how many it declared (0: nothing to draw this frame).
        [[nodiscard]] virtual uint32_t DeclareShadowCascade( RenderPassDeclaration& declared,
                                                             uint32_t               cascade ) = 0;

        // Draws through the blocks DeclareShadowCascade declared, the first of them at @p firstBlock
        // (RDG::PassBindings( context, context.GetBindingBlock( firstBlock + i ) )); the first refused draw is the
        // error.
        [[nodiscard]] virtual Common::BoolResultStr RecordShadowCascade( const RDG::PassContext& context,
                                                                         uint32_t cascade, uint32_t firstBlock,
                                                                         const glm::mat4&         cascadeViewProj,
                                                                         const CascadeCasterBias& bias ) = 0;

        // Whether this caster draws into the views of a point / spot light at @p centre reaching @p radius (cm)
        // this frame, and the key of what it draws there (LocalShadowReach). Asked by
        // MeshRenderer::UpdateLocalShadows in BeginScene, after the caster's own per-frame preparation and before
        // the frame graph is built.
        [[nodiscard]] virtual LocalShadowReach LocalShadowReachOf( const glm::vec3& centre,
                                                                   float            radius ) const = 0;
    };

    /**
     * A NON-MESH CASTER OF THE SUN'S VIRTUAL SHADOW MAP (the terrain, the opaque particles): registered beside
     * IShadowCaster with MeshRenderer::AddVirtualShadowCaster. A separate interface, not more methods on
     * IShadowCaster, because a caster of the cascades and the local lights need not draw into the virtual shadow
     * map's pages.
     */
    class IVirtualShadowCaster
    {
    public:
        virtual ~IVirtualShadowCaster() = default;

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
        // @p level is the clipmap level the node draws (0..kVsmClipmapLevelCount): a caster whose material holds
        // the level's matrix (a camera block written at record time) keeps one per level, since every level's
        // node is recorded before any of them executes.
        [[nodiscard]] virtual uint32_t DeclareVirtualShadow( RDG::PassBuilder& pass, uint32_t level,
                                                             const VirtualShadowBlockParameters& parameters ) = 0;

        // Draws the level through the blocks DeclareVirtualShadow declared (the first at @p firstBlock),
        // projecting with @p levelViewProj (world -> the level's standard-Z clip; no caster bias: the virtual
        // shadow map biases its receivers, VirtualShadowMapProject), draw k from the culled record @p firstRecord
        // + k of
        // @p arguments (the node reads it as Access::IndirectArgs).
        [[nodiscard]] virtual Common::BoolResultStr
        RecordVirtualShadow( const RDG::PassContext& context, uint32_t level, uint32_t firstBlock,
                             const glm::mat4& levelViewProj, RDG::BufferRef arguments, uint32_t firstRecord ) = 0;
    };
} // namespace Desert::Graphic::System
