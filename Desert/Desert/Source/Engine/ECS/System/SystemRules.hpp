#pragma once

#include <Engine/ECS/Components.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Geometry/MeshTypes.hpp>

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
    // to ask "where does a socketed entity end up?" would have to link the whole graphics stack
    // and stand up a device. The rule is the part worth testing; the system is the part that fetches the
    // arguments. Same split that made the shadow cascades testable.

    // What a SocketAttachmentComponent's name follows on @p skeleton: a SOCKET of the rig first (its bone, and its
    // local transform composed after the bone's), else a bone of that name with an identity socket transform (UE
    // USkinnedMeshComponent::GetSocketTransform). Nullopt = neither - the caller reports it.
    struct AttachPoint
    {
        uint32_t  Bone        = 0;
        glm::mat4 SocketLocal = glm::mat4( 1.0f );
    };
    inline std::optional<AttachPoint> ResolveAttachPoint( const Animation::Skeleton& skeleton,
                                                          const std::string&         socketName )
    {
        if ( const Animation::SkeletonSocket* named = skeleton.FindSocket( socketName ) )
        {
            const auto bone = skeleton.FindBoneIndex( named->Bone );
            if ( !bone )
                return std::nullopt;
            return AttachPoint{ *bone, named->LocalTransform() };
        }
        if ( const auto bone = skeleton.FindBoneIndex( socketName ) )
            return AttachPoint{ *bone, glm::mat4( 1.0f ) };
        return std::nullopt;
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

    // How a mesh's sections become draws (UE: one mesh batch per section, each with its section's material, in
    // every pass). ONE rule for every static draw: a static mesh entity (MeshECSSystem), an ISM and each
    // fracture piece (FracturePieceDraw) split the same way.
    //
    // Section si draws with slot Desert::SectionMaterialSlot( si, slotCount ). @p slotMaterialOf( slot ) -> the
    // custom-shader material that slot draws with (a non-mesh-cell DataDrivenMaterial), or a null Material when
    // the slot is a surface (mesh-cell) material drawn by the batched passes. Sections of one custom material
    // merge into one slot draw; sections of one surface slot merge into one section draw, which every batched
    // pass (lit, G-buffer, translucency, depth and shadow casters) shades with THAT slot. @p submeshCount is at
    // most 64 (the masks' width); @p slotCount 0 draws nothing (there is no material to draw with).
    //
    // Every draw casts its OWN submeshes when the entity casts: the shadow passes honour each draw's mask, so the
    // entity's silhouette is the union of its draws, each section through its own caster route (masked or
    // opaque) - and a hidden submesh casts nothing, as it shows nothing.
    template <class Material>
    struct MeshSlotDraw
    {
        Material Mat{};
        uint64_t VisibleMask = 0; // the submeshes this draw draws (hidden ones removed)
        bool     CastShadows = false;
    };
    struct MeshSectionDraw
    {
        size_t   Slot        = 0; // the material slot every submesh of this draw shades with
        uint64_t HiddenMask  = 0; // every submesh this draw does NOT draw (hidden, other slots, custom)
        bool     CastShadows = false;
    };
    template <class Material>
    struct MeshSlotSplit
    {
        std::vector<MeshSlotDraw<Material>> SlotDraws;    // emitted first, in this order
        std::vector<MeshSectionDraw>        SectionDraws; // then these, in first-section order
    };
    template <class Material, class SlotMaterialOf>
    MeshSlotSplit<Material> SplitMeshSlotDraws( size_t submeshCount, size_t slotCount, uint64_t hiddenSubmeshes,
                                                bool castShadows, SlotMaterialOf&& slotMaterialOf )
    {
        MeshSlotSplit<Material> split;
        if ( slotCount == 0 )
            return split;
        std::vector<uint64_t> sectionVisible; // parallel to split.SectionDraws while grouping
        for ( size_t si = 0; si < submeshCount && si < 64; ++si )
        {
            const uint64_t bit = 1ull << si;
            if ( ( hiddenSubmeshes & bit ) != 0 )
                continue;
            const size_t   slot = Desert::SectionMaterialSlot( si, slotCount );
            const Material mat  = slotMaterialOf( slot );
            if ( mat )
            {
                bool merged = false;
                for ( auto& d : split.SlotDraws )
                    if ( d.Mat == mat )
                    {
                        d.VisibleMask |= bit;
                        merged = true;
                        break;
                    }
                if ( !merged )
                    split.SlotDraws.push_back( { mat, bit, castShadows } );
                continue;
            }
            bool merged = false;
            for ( size_t i = 0; i < split.SectionDraws.size(); ++i )
                if ( split.SectionDraws[i].Slot == slot )
                {
                    sectionVisible[i] |= bit;
                    merged = true;
                    break;
                }
            if ( !merged )
            {
                split.SectionDraws.push_back( { slot, 0, castShadows } );
                sectionVisible.push_back( bit );
            }
        }
        for ( size_t i = 0; i < split.SectionDraws.size(); ++i )
            split.SectionDraws[i].HiddenMask = ~sectionVisible[i];
        return split;
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

    // UE's Atmosphere Sun Light Index range: 0 is the sun, 1 the second atmosphere light (the moon). Both light
    // the atmosphere, draw a disc and light the scene through the deferred pass; the time-of-day clock and the
    // cascaded shadow maps belong to index 0.
    inline constexpr int kAtmosphereLightCount = 2;

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
        std::vector<size_t>   WrongIndex;       // rule 4: marked at an index outside [0, kAtmosphereLightCount)
    };

    // Rules, in order:
    //  1. candidates whose direction is degenerate are ignored entirely;
    //  2. prefer marked candidates at `wantedIndex`;
    //  3. several of those -> lowest Id wins, the rest are reported as collisions;
    //  4. marked at another index -> not this slot's light; reported only when that index is outside
    //     [0, kAtmosphereLightCount), because a light marked at the OTHER slot is that slot's light;
    //  5. nothing marked at index 0 -> lowest-Id valid candidate that is not marked at the other slot,
    //     reported as a fallback, because the sky must not go missing just because nobody ticked a box.
    //     Index 1 has no fallback: a second atmosphere light exists only when one is marked for it;
    //  6. nothing valid -> no selection: the scene has NO sun, and every caller says so instead of lighting
    //     the sky from a default direction nobody authored (ECS::PickAtmosphereSun, AtmosphereSunLight.hpp).
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
                if ( c.Index < 0 || c.Index >= kAtmosphereLightCount )
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

        if ( result.Chosen || wantedIndex != 0 )
            return result;

        for ( size_t i = 0; i < candidates.size(); ++i )
        {
            if ( !candidates[i].DirectionValid )
                continue;
            // A light marked for the other slot is that slot's light, never this one's stand-in.
            if ( candidates[i].Marked && candidates[i].Index != wantedIndex && candidates[i].Index >= 0 &&
                 candidates[i].Index < kAtmosphereLightCount )
                continue;
            if ( !result.Chosen || candidates[i].Id < candidates[*result.Chosen].Id )
                result.Chosen = i;
        }

        result.Fallback = result.Chosen.has_value();
        return result;
    }
} // namespace Desert::ECS::Rules
