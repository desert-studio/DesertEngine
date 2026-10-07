#pragma once

/**
 * THE MATERIAL PARAMETER TRACKS OF A LEVEL SEQUENCE, AS DOCUMENT PROPERTIES (UE: the value of a Material
 * Parameter key is edited by the same field the track row draws, and reaching it from outside is reaching
 * that field).
 *
 * One property per Material Parameter track, named "<actor>.<slot>.<parameter>" — the binding's label, the
 * mesh slot and the parameter name the track stores. Its value is the track's value at the playhead; a write
 * is a key at the playhead through `Key`, the one setter the row's field, the palette and `set` all call
 * (one undo step each).
 *
 * The census is DERIVED from the sequence's tracks; the scene only lends what the slot's shader declares
 * (label, clamp, colour widget) through `Schema`, and a track whose actor is not in the scene is still listed.
 *
 * No ImGui here: the LevelSequence suite drives this path without a window.
 */

#include <Editor/Core/EditableProperty.hpp>

#include <Common/Core/ResultStr.hpp>

#include <Engine/Animation/TimeModel.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>
#include <Engine/ECS/LevelSequencePlayback.hpp>

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Desert::Editor
{
    struct SequenceOwner;
    class SequenceEditTransaction;
} // namespace Desert::Editor

namespace Desert::Editor::LevelMaterialEdit
{
    /// What the actor's slot shader declares for one parameter, as the Sequencer's menu offers it.
    struct Schema
    {
        Animation::Timeline::BindingGuid    Binding;
        ECS::LevelSequenceMaterialParameter Parameter;
        std::string                         SlotLabel; ///< "Slot 0 (MP_Default)"
        std::string                         Label;     ///< the schema's display name, else its name
        bool                                Color = false;
        std::optional<float>                Min;
        std::optional<float>                Max;
    };

    /// "Slot <n> (<material>)" — the material ASSET the slot holds, as the Details panel names it; "Slot <n>"
    /// when the slot names none.
    [[nodiscard]] std::string SlotLabel( uint32_t slot, const std::string& material );

    /// "<actor>.<slot>.<parameter>".
    [[nodiscard]] std::string PropertyName( const std::string&                         actor,
                                            const ECS::LevelSequenceMaterialParameter& parameter );

    /// The schema row of @p parameter on @p binding, or nullptr when the scene offers none.
    [[nodiscard]] const Schema* FindSchema( const std::vector<Schema>&                 schema,
                                            const Animation::Timeline::BindingGuid&    binding,
                                            const ECS::LevelSequenceMaterialParameter& parameter );

    /// One property per Material Parameter track of every entity binding, valued at @p tick.
    [[nodiscard]] std::vector<EditableProperty> Describe( const Animation::Timeline::Sequence& sequence,
                                                          Animation::FrameNumber               tick,
                                                          const std::vector<Schema>&           schema );

    /// What a `set` of @p name to @p value writes: the track and the packed value (.x scalar, .xyz vector).
    struct Write
    {
        Animation::Timeline::BindingGuid    Binding;
        ECS::LevelSequenceMaterialParameter Parameter;
        glm::vec4                           Value{ 0.0F };
    };

    /// @p name checked against the census: unknown / ambiguous names, a component count that is not the
    /// track's, and a value outside the schema's declared range are refused (never clamped).
    [[nodiscard]] Common::ResultStr<Write> Resolve( const Animation::Timeline::Sequence& sequence,
                                                    const std::vector<Schema>& schema, const std::string& name,
                                                    const std::vector<float>& value );

    /// THE setter: a key @p value at @p tick on @p binding's @p parameter track, one undo step over @p owner.
    [[nodiscard]] Common::BoolResultStr Key( Animation::Timeline::Sequence& sequence,
                                             SequenceEditTransaction& transaction, const SequenceOwner& owner,
                                             const Animation::Timeline::BindingGuid&    binding,
                                             const ECS::LevelSequenceMaterialParameter& parameter,
                                             Animation::FrameNumber tick, const glm::vec4& value );
} // namespace Desert::Editor::LevelMaterialEdit
