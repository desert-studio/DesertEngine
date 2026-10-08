#pragma once

#include <Engine/ECS/Components.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Desert::ECS::Rules
{
    // The DECISIONS the gameplay systems make, as pure functions of their inputs.
    //
    // The systems themselves hold a `Core::Scene*`, and Scene drags in the renderer, so a test that wants
    // to ask "does a character at 3 m/s pick the walk clip?" would have to link the whole graphics stack
    // and stand up a device. The rule is the part worth testing; the system is the part that fetches the
    // arguments. Same split that made the shadow cascades testable.

    // Which locomotion clip a character should be playing. Ordering matters and is the rule itself:
    // airborne wins over any ground speed, then idle / walk / run by the component's own thresholds.
    inline const std::string& LocomotionClipFor( const LocomotionComponent& loco, float planarSpeed,
                                                 bool onGround )
    {
        if ( !onGround )
            return loco.JumpClip;
        if ( planarSpeed < loco.WalkSpeed )
            return loco.IdleClip;
        if ( planarSpeed <= loco.RunSpeed )
            return loco.WalkClip;
        return loco.RunClip;
    }

    // Where a socket-attached entity ends up: the bone's model-space transform lifted into the target's
    // world space, then the local grip offset. @p parentWorld is the attached entity's parent world matrix
    // (identity when it has none) — TransformComponent stores a LOCAL transform, so a parented weapon must
    // be brought back into its parent's space or it doubles the parent's motion.
    inline glm::mat4 SocketLocalTransform( const glm::mat4& targetWorld, const glm::mat4& boneModel,
                                           const glm::vec3& offsetTranslation, const glm::vec3& offsetRotation,
                                           const glm::vec3& offsetScale,
                                           const glm::mat4& parentWorld = glm::mat4( 1.0f ) )
    {
        const glm::mat4 offset = glm::translate( glm::mat4( 1.0f ), offsetTranslation ) *
                                 glm::toMat4( glm::quat( offsetRotation ) ) *
                                 glm::scale( glm::mat4( 1.0f ), offsetScale );
        return glm::inverse( parentWorld ) * targetWorld * boneModel * offset;
    }

    // Splits a transform matrix into the three fields TransformComponent stores (rotation in RADIANS,
    // like the component).
    struct DecomposedTransform
    {
        glm::vec3 Translation = glm::vec3( 0.0f );
        glm::vec3 Rotation    = glm::vec3( 0.0f ); // euler radians
        glm::vec3 Scale       = glm::vec3( 1.0f );
    };

    inline DecomposedTransform DecomposeTransform( const glm::mat4& m )
    {
        DecomposedTransform out;
        glm::vec3           skew;
        glm::vec4           perspective;
        glm::quat           rotation;
        glm::decompose( m, out.Scale, rotation, out.Translation, skew, perspective );
        out.Rotation = glm::eulerAngles( rotation );
        return out;
    }

    // ---------------------------------------------------------------------------------------------------
    // Which of a mesh entity's draws carries the SHADOW CASTER
    // ---------------------------------------------------------------------------------------------------
    //
    // One static-mesh entity can produce up to three kinds of draw: a whole-entity generic draw (a
    // MaterialComponent naming a non-lit shader), one generic draw per custom-shader material slot, and
    // the batched lit draw for whatever submeshes are left. The shadow pass, though, draws a mesh WHOLE:
    // MeshRenderer::RegisterShadowPass takes a Mesh* and a transform and has no submesh mask, because
    // depth is material-independent. So the caster is a property of the ENTITY, not of a draw, and
    // exactly one draw may carry it — a mesh with one custom slot and one lit slot would otherwise be
    // rasterized into every cascade twice, self-shadowing along the seam and paying double.
    enum class MeshShadowCaster
    {
        None,           // the entity does not cast (CastShadows off, or it produces no draw at all)
        SurfaceDraw,    // the batched lit draw carries it — the only case before generic meshes cast
        ShaderOverride, // whole-entity Shader Override: there is no lit draw to carry it
        FirstSlotDraw,  // every submesh went to a custom slot material; the first slot draw carries it
    };

    // @p shaderOverride   the entity has a MaterialComponent naming a non-lit shader (whole-entity draw)
    // @p slotDrawCount    number of per-slot custom-material draws emitted for this entity
    // @p surfaceDrawEmitted   the batched lit draw was emitted (some submesh stayed on the lit path)
    inline MeshShadowCaster RouteMeshShadowCaster( bool castShadows, bool shaderOverride, size_t slotDrawCount,
                                                   bool surfaceDrawEmitted )
    {
        if ( !castShadows )
            return MeshShadowCaster::None;
        // The override draw REPLACES the entity's whole lit draw, so it is the only candidate.
        if ( shaderOverride )
            return MeshShadowCaster::ShaderOverride;
        // Otherwise the lit draw wins whenever it exists: it is the pre-existing caster, and keeping it
        // means this rule cannot change what an all-lit scene puts in the cascades.
        if ( surfaceDrawEmitted )
            return MeshShadowCaster::SurfaceDraw;
        if ( slotDrawCount > 0 )
            return MeshShadowCaster::FirstSlotDraw;
        return MeshShadowCaster::None;
    }

    // ---------------------------------------------------------------------------------------------------
    // Which directional light is THE SUN
    // ---------------------------------------------------------------------------------------------------
    //
    // There was no notion of "the sun" in this engine: identity was uniqueness, selection was "whichever
    // one the registry visited first", and the sky and the lighting used DIFFERENT iteration orders (an
    // entt group in Scene, a view in the sky collector) — so in a scene with two directional lights they
    // could pick different ones and nothing said so. The rule below is the single answer, and because it
    // is a function of plain numbers, the test can hold it to all six cases including the tie-break.

    // Below this length a Translation is not a direction at all — normalizing it yields NaN and the sun
    // ends up pointing nowhere. Two different epsilons for this existed in the engine; this is the one.
    inline constexpr float kSunDirectionEpsilon = 1e-4f;

    inline bool IsSunDirectionValid( const glm::vec3& lightTranslation )
    {
        return glm::length( lightTranslation ) > kSunDirectionEpsilon;
    }

    // THE DIRECTION A DIRECTIONAL LIGHT TRAVELS, or nothing at all when its Translation is not a
    // direction — the LIGHTING half of the same question `IsSunDirectionValid` answers for the sky.
    //
    // It exists because the two halves disagreed. The comment on kSunDirectionEpsilon above says "two
    // different epsilons for this existed in the engine; this is the one", and that consolidation
    // reached the sky and stopped: `Engine/Core/Scene.cpp` went on open-coding
    // `glm::length(rawDir) > 0.001f` when it collected directional lights for the renderer — TEN TIMES
    // this epsilon. A sun whose Translation measured, say, 5e-4 was therefore VALID TO THE SKY and
    // INVALID TO THE RENDERER at the same instant: the atmosphere took a sun direction from it and lit
    // the dome, while the deferred composite received no directional light at all. Both sides were
    // individually defensible and neither could see the other. Now there is one gate and the question
    // cannot be answered twice.
    //
    // Returning an optional rather than a bool is the point: the caller that wants the direction and the
    // caller that wants the verdict get the SAME answer, and a caller cannot normalize a vector this
    // function has just called unusable.
    inline std::optional<glm::vec3> DirectionalLightTravel( const glm::vec3& lightTranslation )
    {
        if ( !IsSunDirectionValid( lightTranslation ) )
            return std::nullopt;
        return glm::normalize( lightTranslation );
    }

    // THE ENGINE'S ONE NEGATION. TransformComponent::Translation on a directional light is the direction
    // the light TRAVELS (sun -> scene); the atmosphere and the IBL bake both want the
    // direction TOWARD the sun. Every one of them goes through here. Two negations is how a sky ends up
    // lit from below — which is precisely the bug the viewport's light gizmo shipped with.
    inline glm::vec3 AtmosphereSunDirection( const glm::vec3& lightTranslation )
    {
        return -glm::normalize( lightTranslation );
    }

    // Used when a scene has no usable directional light at all, so the sky is still lit rather than black.
    inline glm::vec3 FallbackAtmosphereSunDirection()
    {
        return glm::normalize( glm::vec3( 0.3f, 0.9f, 0.3f ) );
    }

    struct SunCandidate
    {
        // Entity UUID. A plain integer rather than Common::UUID so the rule stays link-free: the tests that
        // exercise it deliberately do not link the engine, and UUID's constructors live in a .cpp.
        uint64_t Id             = 0;
        bool     Marked         = false; // DirectionalLightData::AtmosphereSunLight
        int      Index          = 0;     // DirectionalLightData::AtmosphereSunLightIndex
        bool     DirectionValid = false; // IsSunDirectionValid( Translation )
    };

    // The selection AND everything worth complaining about, so the rule itself stays pure. Logging belongs
    // to the caller — a LOG_WARN in here would drag the logger into every test that asks a question about
    // the sun, and would fire once per frame instead of once per scene load.
    struct AtmosphereSunSelection
    {
        std::optional<size_t> Chosen;           // index into the candidate span
        bool                  Fallback = false; // rule 5: nothing was marked, lowest id taken
        std::vector<size_t>   Collisions;       // rule 3: further marked candidates at wantedIndex
        std::vector<size_t>   WrongIndex;       // rule 4: marked, but at an index v1 does not render
    };

    // Rules, in order:
    //  1. candidates whose direction is degenerate are ignored entirely;
    //  2. prefer marked candidates at `wantedIndex`;
    //  3. several of those -> lowest Id wins, the rest are reported as collisions;
    //  4. marked at another index -> treated as unmarked and reported (the field is authorable, but the
    //     engine renders exactly one directional light, so index != 0 is not a thing that can work);
    //  5. nothing marked -> lowest-Id valid candidate, reported as a fallback, because the sky must not go
    //     missing just because nobody ticked a box;
    //  6. nothing valid -> no selection; the caller uses FallbackAtmosphereSunDirection().
    inline AtmosphereSunSelection SelectAtmosphereSun( std::span<const SunCandidate> candidates, int wantedIndex )
    {
        AtmosphereSunSelection result;

        for ( size_t i = 0; i < candidates.size(); ++i )
        {
            const SunCandidate& c = candidates[i];
            if ( !c.DirectionValid )
                continue;
            if ( !c.Marked )
                continue;

            if ( c.Index != wantedIndex )
            {
                result.WrongIndex.push_back( i );
                continue;
            }

            if ( !result.Chosen )
            {
                result.Chosen = i;
                continue;
            }

            // Lowest id wins; the loser is a collision either way, so both branches record one.
            if ( c.Id < candidates[*result.Chosen].Id )
            {
                result.Collisions.push_back( *result.Chosen );
                result.Chosen = i;
            }
            else
            {
                result.Collisions.push_back( i );
            }
        }

        if ( result.Chosen )
            return result;

        for ( size_t i = 0; i < candidates.size(); ++i )
        {
            if ( !candidates[i].DirectionValid )
                continue;
            if ( !result.Chosen || candidates[i].Id < candidates[*result.Chosen].Id )
                result.Chosen = i;
        }

        result.Fallback = result.Chosen.has_value();
        return result;
    }
} // namespace Desert::ECS::Rules
