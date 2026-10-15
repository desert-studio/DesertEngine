#pragma once

#include <Engine/ECS/Components.hpp>
#include <Engine/Animation/Skeleton.hpp>

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

    // ---------------------------------------------------------------------------------------------------
    // Which of a mesh entity's draws carries the SHADOW CASTER
    // ---------------------------------------------------------------------------------------------------
    //
    // One static-mesh entity can produce up to three kinds of draw: a whole-entity generic draw (a
    // MaterialComponent naming a non-lit shader), one generic draw per custom-shader material slot, and
    // the batched lit draw for whatever submeshes are left. The shadow pass, though, draws a mesh WHOLE:
    // MeshRenderer::ShadowCascadePasses takes a Mesh* and a transform and has no submesh mask, because
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

    // Which submeshes of a mesh leave the batched lit draw for a slot-material draw (v3 per-slot shaders), and
    // which single draw carries the caster. ONE rule for every static draw: a static mesh entity
    // (MeshECSSystem) and each fracture piece (FracturePieceDraw) split the same way.
    //
    // @p slotMaterialOf( si ) -> the custom-shader material submesh si draws with (a non-mesh-cell
    // DataDrivenMaterial), or a null Material when it stays on the batched lit path. Submeshes sharing one such
    // material merge into one slot draw. @p submeshCount is at most 64 (the masks' width).
    template <class Material>
    struct MeshSlotDraw
    {
        Material Mat{};
        uint64_t VisibleMask = 0; // the submeshes this draw draws (hidden ones removed)
        bool     CastShadows = false;
    };
    template <class Material>
    struct MeshSlotSplit
    {
        std::vector<MeshSlotDraw<Material>> SlotDraws; // emitted in this order
        uint64_t SurfaceHidden      = 0;               // the lit draw's hidden mask (hidden | slot submeshes)
        bool     SurfaceDrawEmitted = false;
        bool     SurfaceCastShadows = false;
    };
    template <class Material, class SlotMaterialOf>
    MeshSlotSplit<Material> SplitMeshSlotDraws( size_t submeshCount, uint64_t hiddenSubmeshes, bool castShadows,
                                                SlotMaterialOf&& slotMaterialOf )
    {
        MeshSlotSplit<Material>             split;
        std::vector<MeshSlotDraw<Material>> grouped;
        uint64_t                            customMask = 0;
        for ( size_t si = 0; si < submeshCount && si < 64; ++si )
        {
            const Material mat = slotMaterialOf( si );
            if ( !mat )
                continue;
            customMask |= ( 1ull << si );
            bool merged = false;
            for ( auto& d : grouped )
                if ( d.Mat == mat )
                {
                    d.VisibleMask |= ( 1ull << si );
                    merged = true;
                    break;
                }
            if ( !merged )
                grouped.push_back( { mat, 1ull << si, false } );
        }

        // Decided BEFORE anything is emitted, because the caster belongs to the ENTITY: the shadow pass draws a
        // mesh whole, so the lit draw and the slot draws are candidates for the same silhouette and only one of
        // them may record it.
        const uint64_t allMask   = submeshCount >= 64 ? ~0ull : ( ( 1ull << submeshCount ) - 1ull );
        split.SurfaceHidden      = hiddenSubmeshes | customMask;
        split.SurfaceDrawEmitted = submeshCount == 0 || ( ~split.SurfaceHidden & allMask ) != 0;
        const auto shadowRoute   = RouteMeshShadowCaster( castShadows, /*shaderOverride*/ false, grouped.size(),
                                                          split.SurfaceDrawEmitted );
        split.SurfaceCastShadows = shadowRoute == MeshShadowCaster::SurfaceDraw;

        bool slotCasterPlaced = false;
        for ( auto& d : grouped )
        {
            d.VisibleMask &= ~hiddenSubmeshes;
            if ( !d.VisibleMask )
                continue;
            // "First slot draw" means the first one actually EMITTED — a leading slot whose submeshes are all
            // hidden emits nothing, and routing the caster to it would drop the entity's shadow, not move it.
            d.CastShadows    = !slotCasterPlaced && shadowRoute == MeshShadowCaster::FirstSlotDraw;
            slotCasterPlaced = slotCasterPlaced || d.CastShadows;
            split.SlotDraws.push_back( d );
        }
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

    // A DIRECTIONAL LIGHT'S DIRECTION IS ITS ROTATION, as in UE (ULightComponent::GetDirection =
    // GetComponentTransform().GetUnitAxis( EAxis::X )). The light travels along the entity's forward axis,
    // which in this engine is -Z for every entity (cameras, spot lights, projectiles); Translation is only
    // where the editor draws the light's icon. A rotation is always a direction, so there is no degenerate
    // sun any more: the old Translation-as-direction model had an epsilon, a "light emits nothing" report
    // and a migration-era comment explaining why the rotation gizmo did nothing — all gone with it.
    inline constexpr glm::vec3 kLightForwardAxis = glm::vec3( 0.0f, 0.0f, -1.0f );

    // The direction the light TRAVELS (sun -> scene), from the entity's WORLD rotation.
    inline glm::vec3 DirectionalLightTravel( const glm::quat& worldRotation )
    {
        return glm::normalize( worldRotation * kLightForwardAxis );
    }

    // THE ENGINE'S ONE NEGATION. The atmosphere and the IBL bake want the direction TOWARD the sun; every
    // one of them goes through here. Two negations is how a sky ends up lit from below. The travel form is
    // for lights that are not entities (the asset previews' fixed key light).
    inline glm::vec3 AtmosphereSunDirection( const glm::vec3& travel )
    {
        return -glm::normalize( travel );
    }

    inline glm::vec3 AtmosphereSunDirection( const glm::quat& worldRotation )
    {
        return AtmosphereSunDirection( DirectionalLightTravel( worldRotation ) );
    }

    // The rotation whose forward axis is `travel` — the shortest arc from kLightForwardAxis, so roll stays
    // zero for every heading except the exact opposite one (a light aimed along +Z), which turns about +Y.
    // Roll does not change a directional light. This is the inverse of DirectionalLightTravel and the one
    // way the clock, the editor's direction widget and the scene migrator turn a heading into a Rotation.
    inline glm::quat RotationForTravel( const glm::vec3& travel )
    {
        const glm::vec3 to = glm::normalize( travel );
        const float     d  = glm::dot( kLightForwardAxis, to );
        if ( d < -0.999999f )
            return glm::quat( 0.0f, glm::vec3( 0.0f, 1.0f, 0.0f ) ); // 180 deg about +Y
        const glm::vec3 axis = glm::cross( kLightForwardAxis, to );
        return glm::normalize( glm::quat( 1.0f + d, axis ) );
    }

    struct SunCandidate
    {
        // Entity UUID. A plain integer rather than Common::UUID so the rule stays link-free: the tests that
        // exercise it deliberately do not link the engine, and UUID's constructors live in a .cpp.
        uint64_t Id     = 0;
        bool     Marked = false; // DirectionalLightData::AtmosphereSunLight
        int      Index  = 0;     // DirectionalLightData::AtmosphereSunLightIndex
    };

    // The selection AND everything worth complaining about, so the rule itself stays pure. Logging belongs
    // to the caller — a LOG_WARN in here would drag the logger into every test that asks a question about
    // the sun, and would fire once per frame instead of once per scene load.
    struct AtmosphereSunSelection
    {
        std::optional<size_t> Chosen;           // index into the candidate span
        bool                  Fallback = false; // rule 4: nothing was marked, lowest id taken
        std::vector<size_t>   Collisions;       // rule 2: further marked candidates at wantedIndex
        std::vector<size_t>   WrongIndex;       // rule 3: marked, but at an index v1 does not render
    };

    // Rules, in order:
    //  1. prefer marked candidates at `wantedIndex`;
    //  2. several of those -> lowest Id wins, the rest are reported as collisions;
    //  3. marked at another index -> treated as unmarked and reported (the field is authorable, but the
    //     engine renders exactly one directional light, so index != 0 is not a thing that can work);
    //  4. nothing marked -> lowest-Id candidate, reported as a fallback, because the sky must not go
    //     missing just because nobody ticked a box;
    //  5. no candidates -> no selection: the scene has NO sun, and every caller says so instead of lighting
    //     the sky from a default direction nobody authored (ECS::PickAtmosphereSun, AtmosphereSunLight.hpp).
    inline AtmosphereSunSelection SelectAtmosphereSun( std::span<const SunCandidate> candidates, int wantedIndex )
    {
        AtmosphereSunSelection result;

        for ( size_t i = 0; i < candidates.size(); ++i )
        {
            const SunCandidate& c = candidates[i];
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
            if ( !result.Chosen || candidates[i].Id < candidates[*result.Chosen].Id )
                result.Chosen = i;
        }

        result.Fallback = result.Chosen.has_value();
        return result;
    }
} // namespace Desert::ECS::Rules
