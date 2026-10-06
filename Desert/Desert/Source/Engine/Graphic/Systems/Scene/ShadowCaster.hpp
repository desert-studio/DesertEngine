#pragma once

#include <Common/Core/ResultStr.hpp>

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
     * `cascadeViewProj` is the matrix the receivers sample the cascade with (standard-Z, clip from world).
     */
    class IShadowCaster
    {
    public:
        virtual ~IShadowCaster() = default;

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
                                                                         const glm::mat4& cascadeViewProj ) = 0;
    };
} // namespace Desert::Graphic::System
