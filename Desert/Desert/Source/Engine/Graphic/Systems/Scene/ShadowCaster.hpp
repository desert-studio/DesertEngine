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
    // One thing a caster draws into the sun's virtual shadow map: a hash of what it draws there (it changes whenever
    // the depth it writes would) and its world box. The virtual shadow map keeps its pages across frames, so the mesh
    // renderer diffs these between frames (UE: a primitive added, removed or updated invalidates the pages its bounds
    // cover) - a caster that changed drops the cached pages under its old and its new box, and nothing else.
    struct ShadowCasterEntry
    {
        uint64_t  Signature = 0;
        glm::vec3 Min{ 0.0f };
        glm::vec3 Max{ 0.0f };
    };

    // One draw a caster records into a virtual shadow map level, for the level's GPU cull ("VSM: Cull Casters"): its
    // world box and its indirect argument record (Graphic/MeshDrawArguments.hpp: a VkDrawIndirectCommand in the first
    // four words, its instance count 0 - the cull counts the draw's one instance in where its box covers a page drawn
    // this frame).
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
     * @brief A renderer whose geometry casts into the sun's cascaded shadow map.
     *
     * The cascade targets and their depth passes belong to the MeshRenderer, which clears each cascade once
     * and records its own casters; a caster that is not a mesh (the tessellated terrain) is recorded INSIDE
     * that same pass rather than in a second graph pass on the same target. A second pass would need an
     * order against the first inside one render phase, and the graph gives none: whichever began the
     * target last would clear the other's depth.
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

        // Draws the level through the blocks DeclareVirtualShadow declared (the first at @p firstBlock), projecting
        // with @p levelViewProj (world -> the level's standard-Z clip; no caster bias: the virtual shadow map biases
        // its receivers, VirtualShadowMapProject), draw k from the culled record @p firstRecord + k of
        // @p arguments (the node reads it as Access::IndirectArgs).
        [[nodiscard]] virtual Common::BoolResultStr RecordVirtualShadow( const RDG::PassContext& context,
                                                                         uint32_t                firstBlock,
                                                                         const glm::mat4&        levelViewProj,
                                                                         RDG::BufferRef          arguments,
                                                                         uint32_t                firstRecord ) = 0;
    };
} // namespace Desert::Graphic::System
