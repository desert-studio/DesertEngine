#pragma once

/**
 * THE LEVEL SEQUENCE DOCUMENT'S ENGINE HALF (ANIM-LSEQ): the edits the Sequencer makes to a `.dseq` and the
 * preview it poses the scene with. UE: FSequencer's "+ Track → Actor" (AddPossessable), the Transform track's
 * key, the Camera Cut track, and the "Restore State" the editor applies when the sequence is closed.
 *
 * Registry-only, like LevelSequencePlayback.hpp: the suite reaches every line here without a Scene or a GPU,
 * and the editor panel is left with drawing. The preview resolves and applies through the SAME host the
 * LevelSequenceSystem plays with (`LevelSequenceEntityHost`), so what the document shows at a tick is what
 * the placed component plays at that tick — one resolver, never a second one for the editor.
 */

#include <Engine/Animation/Pose.hpp>
#include <Engine/Animation/Timeline/Binding.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/LevelSequencePlayback.hpp>

#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

#include <entt/entt.hpp>

#include <string>
#include <vector>

namespace Desert::ECS
{
    /// The property name of an entity's Transform track — the one `LevelSequenceEntityHost::Apply` reads.
    inline constexpr const char* kLevelSequenceTransformProperty = "Transform";
    /// The property name of the sequence-level Camera Cut track.
    inline constexpr const char* kLevelSequenceCameraCutProperty = "CameraCut";

    /**
     * @brief "+ Track → Actor" (UE: a Possessable): the Entity binding naming @p entity, created when missing.
     *
     * The GUID is DERIVED from the entity UUID (`BindingGuid::ForObject`), so binding one entity twice is the
     * same binding and not a second row — the find-or-create the bone tracks already follow. Revision++ when
     * the binding is new. Refuses the null UUID.
     */
    [[nodiscard]] Common::ResultStr<Animation::Timeline::BindingGuid>
    AddEntityBinding( Animation::Timeline::Sequence& sequence, const Common::UUID& entity,
                      const std::string& label );

    /// The entity pose a Transform key stores: TransformComponent's Euler rotation as the channel's quaternion.
    [[nodiscard]] Animation::BoneTransform EntityPose( const TransformComponent& transform );

    /**
     * @brief Upsert @p pose at @p tick on @p binding's Transform track (track and section created as needed,
     * `TrackEditing::ChannelForKey`'s gap rule). Refuses a binding that is not an Entity binding of this
     * sequence, and a non-finite pose. Revision++ on a structural change.
     */
    [[nodiscard]] Common::BoolResultStr SetEntityTransformKey( Animation::Timeline::Sequence&          sequence,
                                                               const Animation::Timeline::BindingGuid& binding,
                                                               Animation::FrameNumber                  tick,
                                                               const Animation::BoneTransform&         pose );

    /**
     * @brief A Camera Cut section over [@p start, @p end] looking through @p camera (an Entity binding), on the
     * sequence-level Camera Cut track (its master binding and track created when missing). Whatever `Validate`
     * refuses — an overlap with another cut on row 0, a camera that is not an Entity binding — leaves the
     * sequence exactly as it was.
     */
    [[nodiscard]] Common::BoolResultStr AddCameraCut( Animation::Timeline::Sequence&          sequence,
                                                      const Animation::Timeline::BindingGuid& camera,
                                                      Animation::FrameNumber start, Animation::FrameNumber end );

    /**
     * @brief The Sequencer's preview of a level sequence over a scene's registry (UE: the editor's sequence
     * player with "Restore State" on close).
     *
     * `Scrub` poses the registry at a tick through `LevelSequenceEntityHost`, having first RECORDED the state of
     * every entity it is about to write (its Transform, and whether it had a VisibilityComponent and its value).
     * `Restore` writes every recorded state back — the component the preview added is removed again — and
     * forgets it. An entity destroyed while previewed is skipped. The destructor does NOT restore: it has no
     * registry, and a registry that died first has nothing to give back to.
     */
    class LevelSequencePreview
    {
    public:
        LevelSequenceStep Scrub( entt::registry& registry, const Animation::Timeline::Sequence& sequence,
                                 Animation::FrameNumber tick );
        void              Restore( entt::registry& registry );

        [[nodiscard]] bool Active() const
        {
            return !m_Saved.empty();
        }

    private:
        struct Saved
        {
            entt::entity       Entity = entt::null;
            TransformComponent Transform;
            bool               HadTransform  = false;
            bool               HadVisibility = false;
            bool               Visible       = true;
        };
        std::vector<Saved> m_Saved;
        /// No overrides: the document is about the ASSET, not about one placed actor's re-pointing of it.
        LevelSequenceComponent m_NoOverrides;
    };
} // namespace Desert::ECS
