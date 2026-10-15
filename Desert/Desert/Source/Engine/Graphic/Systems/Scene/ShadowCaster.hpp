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
    // What a caster puts into one point / spot light's shadow views this frame (UE: a primitive's relevance to a
    // light's whole-scene shadow, and whether it is movable): MeshRenderer keys the light's cached atlas tiles by it.
    struct LocalShadowReach
    {
        bool     Casts     = false; // it draws into the light's views (Declare/RecordShadowCascade with their index)
        bool     Moving    = false; // what it draws there changes every frame: the light's views are redrawn each frame
        uint64_t Signature = 0;     // otherwise, a key of what it draws there: a change redraws the views
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
     * kMaxShadowCascades + slot a local light's view (a spot, or one cube face of a point; Graphic/LocalShadows.hpp).
     * A local view's pass is "MeshShadowLocal<slot>" on the local atlas, its viewport the view's tile, and its
     * `bias` zero (a local view's bias is the receiver's, Mesh/LocalLightShadow.glslh); it is called only for a
     * caster whose LocalShadowReachOf the light Casts, and only on the frames the light's views are redrawn.
     *
     * Called once per cascade per frame, with the cascade's render pass open, only when shadows are on.
     * `cascadeViewProj` is the matrix the CASTERS draw the cascade with (standard-Z, clip from world): the
     * receivers' matrix with the light's constant depth bias folded in (Graphic::CasterViewProj), so a caster
     * that projects with it is biased like every mesh caster without knowing the bias exists. `bias` is the
     * same cascade's CascadeCasterBias: its DepthBias is the one already in `cascadeViewProj`; its SlopeBias /
     * SlopeClamp are the slope term the caster's depth stage adds (Mesh/Surface/Pass_ShadowDepth.glslh for the
     * mesh cells, Programs/Terrain/TerrainShadow.shader for the terrain).
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
                                                                         const glm::mat4&         cascadeViewProj,
                                                                         const CascadeCasterBias& bias ) = 0;

        // Whether this caster draws into the views of a point / spot light at @p centre reaching @p radius (cm) this
        // frame, and the key of what it draws there (LocalShadowReach). Asked by MeshRenderer::UpdateLocalShadows
        // in BeginScene, after the caster's own per-frame preparation and before the frame graph is built.
        [[nodiscard]] virtual LocalShadowReach LocalShadowReachOf( const glm::vec3& centre, float radius ) const = 0;
    };
} // namespace Desert::Graphic::System
