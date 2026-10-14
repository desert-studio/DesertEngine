#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/ShadowCascades.hpp>

#include <glm/glm.hpp>

#include <cstdint>

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
    };
} // namespace Desert::Graphic::System
