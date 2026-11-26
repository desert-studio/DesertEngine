#pragma once

/**
 * THE LEVEL SEQUENCE'S BONE KEYS (UE: an FK Control Rig track on a skeletal mesh binding).
 *
 * A bone of an actor is a Bone binding whose Parent is the actor's Entity binding (Track.hpp, the host
 * table) with ONE Transform track (Property ""), played as the actor Animator's sequencer layer
 * (LevelSequencePlayback.hpp, `PoseBones`). Every function edits the Sequence in place; undo is the caller's
 * document transaction. The pose-key edits shared with actors (move, remove, shape, easing) are the
 * `…EntityTransformKey…` functions of LevelSequenceAuthoring.hpp, which take a bone binding as well
 * (`IsLevelPoseTrack`).
 */

#include <Engine/Animation/BoneTransform.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>

#include <Common/Core/ResultStr.hpp>

#include <entt/entt.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace Desert::ECS
{
    /// The entity of the Entity binding @p actor in @p registry; refuses by name a binding that is not an actor
    /// of @p sequence or names no entity of the scene.
    [[nodiscard]] Common::ResultStr<entt::entity> BoneActorEntity( const Animation::Timeline::Sequence& sequence,
                                                                   entt::registry&                      registry,
                                                                   Animation::Timeline::BindingGuid     actor );

    /// The bone names of @p actor's skeleton in skeleton order (UE: the FK rig's controls); empty without an
    /// Animator.
    [[nodiscard]] std::vector<std::string> ActorBoneNames( const entt::registry& registry, entt::entity actor );

    /**
     * @brief The Bone binding for @p bone under the Entity binding @p actor, made (with its Transform track and
     * one Absolute section spanning the playback range) if absent. The section's channel defaults are the
     * bone's pose under the layer now, so an unkeyed section shows the animation it covers rather than a bone
     * collapsed onto its parent. Refuses by name: @p actor not an Entity binding of @p sequence; its entity
     * absent or without an Animator; the skeleton has no @p bone. Revision++ when made.
     */
    [[nodiscard]] Common::ResultStr<Animation::Timeline::BindingGuid>
    AddBoneBinding( Animation::Timeline::Sequence& sequence, entt::registry& registry,
                    Animation::Timeline::BindingGuid actor, std::string_view bone );

    /// Removes @p bone's binding and its track. Refuses a binding that is not a Bone binding. Revision++.
    [[nodiscard]] Common::BoolResultStr RemoveBoneBinding( Animation::Timeline::Sequence&   sequence,
                                                           Animation::Timeline::BindingGuid bone );

    /// The Bone binding of @p bone under @p actor, null when the sequence has none.
    [[nodiscard]] Animation::Timeline::BindingGuid FindBoneBinding( const Animation::Timeline::Sequence& sequence,
                                                                    Animation::Timeline::BindingGuid     actor,
                                                                    std::string_view                     bone );

    /// The bone's local transform as the viewer sees it NOW (the animator's published pose, layer included)
    /// — what Key Bone writes, as KeyLevelTransform keys the live transform.
    [[nodiscard]] Common::ResultStr<Animation::BoneTransform>
    BonePose( const entt::registry& registry, entt::entity actor, std::string_view bone );

    /// The bone's local transform UNDER the sequencer layer (the animation alone) — what an Additive key is
    /// measured from.
    [[nodiscard]] Common::ResultStr<Animation::BoneTransform>
    BonePreLayerPose( const entt::registry& registry, entt::entity actor, std::string_view bone );

    /**
     * @brief Writes a Transform key (9 channels) at @p tick into the section covering it (the highest row, the
     * one that wins; a new Absolute section over the gap when none does). Absolute sections take @p local as
     * is; Additive sections take local ∘ inverse(@p preLayer), so the visible pose is @p local at weight 1 (UE
     * FKControlRig Additive keying).
     */
    [[nodiscard]] Common::BoolResultStr SetBoneKey( Animation::Timeline::Sequence&   sequence,
                                                    Animation::Timeline::BindingGuid bone,
                                                    Animation::FrameNumber           tick,
                                                    const Animation::BoneTransform&  local,
                                                    const Animation::BoneTransform&  preLayer );

    /**
     * @brief Writes @p value as is into the channels of the section covering @p tick (the Details' Location /
     * Rotation / Scale of a bone row: the channel value, an offset in an Additive section). Same section rule
     * as `SetBoneKey`.
     */
    [[nodiscard]] Common::BoolResultStr SetBoneChannelKey( Animation::Timeline::Sequence&   sequence,
                                                           Animation::Timeline::BindingGuid bone,
                                                           Animation::FrameNumber           tick,
                                                           const Animation::BoneTransform&  value );

    /// The channel value of the section covering @p tick (what `SetBoneChannelKey` writes); refuses a tick no
    /// section covers.
    [[nodiscard]] Common::ResultStr<Animation::BoneTransform>
    BoneChannelValue( const Animation::Timeline::Sequence& sequence, Animation::Timeline::BindingGuid bone,
                      Animation::FrameNumber tick );

    [[nodiscard]] std::vector<Animation::FrameNumber> BoneKeyTicks( const Animation::Timeline::Sequence& sequence,
                                                                    Animation::Timeline::BindingGuid     bone );

    /// Blend and a constant weight on every section of the bone's track (the row's Blend and Weight fields).
    /// A section whose blend CHANGES has its keys and defaults re-expressed over @p preLayer (the bone under
    /// the layer at the playhead), so the pose it shows there is kept and an Additive section holds offsets,
    /// not absolute values (UE: FK Control Rig Convert to Additive / Absolute). Refuses a weight that is not
    /// finite. A weight of 1 clears the weight channel (full weight).
    [[nodiscard]] Common::BoolResultStr SetBoneSectionBlend( Animation::Timeline::Sequence&        sequence,
                                                             Animation::Timeline::BindingGuid      bone,
                                                             Animation::Timeline::SectionBlendType blend,
                                                             float                                 weight,
                                                             const Animation::BoneTransform&       preLayer );

    /// The blend and weight (at the section start) of the bone track's first section; refuses a bone with no
    /// section.
    struct BoneSectionBlend
    {
        Animation::Timeline::SectionBlendType Blend  = Animation::Timeline::SectionBlendType::Absolute;
        float                                 Weight = 1.0F;
    };
    [[nodiscard]] Common::ResultStr<BoneSectionBlend>
    BoneSectionBlendOf( const Animation::Timeline::Sequence& sequence, Animation::Timeline::BindingGuid bone );
} // namespace Desert::ECS
