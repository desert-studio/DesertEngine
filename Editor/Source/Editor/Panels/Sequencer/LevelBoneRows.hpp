#pragma once

/**
 * THE BONE TRACKS OF A LEVEL SEQUENCE, AS DOCUMENT PROPERTIES (UE: the FK Control Rig track's controls in the
 * Sequencer's Details — Location / Rotation / Scale of the selected control, and the section's Blend Type and
 * Weight).
 *
 * Per Bone binding (ECS/LevelSequenceBoneKeys.hpp) five properties, named "Bone.<actor>.<bone>.<field>":
 *
 *     Location  float3  cm, the channel value at the playhead
 *     Rotation  float3  Euler degrees (pitch, yaw, roll — glm's order), the channel value at the playhead
 *     Scale     float3
 *     Weight    float   the section weight (constant over the section)
 *     Blend     int     0 Absolute, 1 Additive (the channel carries numbers only, EditableProperty.hpp)
 *
 * A Location / Rotation / Scale write is a key at the playhead of the CHANNEL VALUE — an offset in an Additive
 * section, the pose in an Absolute one (`ECS::SetBoneChannelKey`); "Key Bone" keys the pose the viewer sees
 * (`ECS::SetBoneKey`). The rows, the palette and `set` all reach these through SequencerPanel's LevelBone*
 * members (LevelBoneRows.cpp), one undo step each.
 *
 * No ImGui here: the census and the name rule are plain functions of the sequence.
 */

#include <Editor/Core/EditableProperty.hpp>

#include <Common/Core/ResultStr.hpp>

#include <Engine/Animation/TimeModel.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace Desert::Editor::LevelBoneEdit
{
    inline constexpr std::string_view kPrefix = "Bone.";

    /// "Bone.<actor>.<bone>.<field>".
    [[nodiscard]] std::string PropertyName( std::string_view actor, std::string_view bone,
                                            std::string_view field );

    /// What a property name addresses: the actor's Entity binding, the bone's name and the field. The bone may
    /// have no binding yet (`Bone` null): the caller refuses that by name.
    struct Address
    {
        Animation::Timeline::BindingGuid Actor;
        Animation::Timeline::BindingGuid Bone; ///< null: the sequence has no track for this bone
        std::string                      ActorLabel;
        std::string                      BoneName;
        std::string                      Field;
    };

    /// @p name read against the sequence's actors: "Bone.", an actor label of an Entity binding, ".", a bone
    /// name, ".", one of the five fields. Refuses by name an unknown actor or field.
    [[nodiscard]] Common::ResultStr<Address> Parse( const Animation::Timeline::Sequence& sequence,
                                                    std::string_view                     name );

    /// Five properties per Bone binding, valued at @p tick.
    [[nodiscard]] std::vector<EditableProperty> Describe( const Animation::Timeline::Sequence& sequence,
                                                          Animation::FrameNumber               tick );

    /// glm's Euler angles of @p rotation, in degrees.
    [[nodiscard]] glm::vec3 EulerDegrees( const glm::quat& rotation );
    [[nodiscard]] glm::quat FromEulerDegrees( const glm::vec3& degrees );
} // namespace Desert::Editor::LevelBoneEdit
