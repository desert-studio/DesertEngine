#pragma once

#include <Editor/Core/EditableProperty.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace Desert::Editor::Core
{
    /**
     * @brief THE SELECTED ENTITY'S TRANSFORM, AS THE CONTROL CHANNEL'S `selection` SUBJECT (WP16c).
     *
     * `run Entity <tag>` selects an entity, and until this subject nothing a script could send moved it: the
     * Details panel's drag has no name, and piloting a camera moves the pilot camera, not the selection. So a
     * frame could prove a move only through the files. The three rows are the TransformComponent's own fields
     * in its own units (centimetres; radians, as the scene file stores them; a factor), so `set` writes the
     * values Details writes and the file saves - no conversion lives here to disagree with either.
     *
     * The write is ONE undo step, like a Details edit (EditorLayer applies it through the same TransformCommand
     * a gizmo drag records), so `Action / Undo` puts the entity back.
     */
    inline constexpr const char* kSelectionTranslation = "Translation";
    inline constexpr const char* kSelectionRotation    = "Rotation";
    inline constexpr const char* kSelectionScale       = "Scale";

    struct SelectionTransform
    {
        glm::vec3 Translation{ 0.0f };
        glm::vec3 Rotation{ 0.0f };
        glm::vec3 Scale{ 1.0f };
    };

    [[nodiscard]] inline std::vector<EditableProperty> DescribeSelectionTransform( const SelectionTransform& trs )
    {
        std::vector<EditableProperty> census;
        const auto row = [&census]( const char* name, const char* label, const glm::vec3& value )
        {
            EditableProperty property;
            property.Name       = name;
            property.Label      = label;
            property.Group      = "Transform";
            property.Type       = "float3";
            property.Components = 3;
            property.Value      = { value.x, value.y, value.z, 0.0f };
            census.push_back( property );
        };
        row( kSelectionTranslation, "Location (cm)", trs.Translation );
        row( kSelectionRotation, "Rotation (radians)", trs.Rotation );
        row( kSelectionScale, "Scale", trs.Scale );
        return census;
    }

    /**
     * @brief @p current with @p property set to @p value - or a refusal that names what would have fixed it:
     * an unknown name (it would otherwise write nothing and report success), the wrong number of components
     * (padding would move the entity somewhere nobody asked for), a NaN or an infinity (it reaches the world
     * matrix and the entity vanishes with nothing said).
     */
    [[nodiscard]] inline Common::ResultStr<SelectionTransform>
    WriteSelectionTransform( SelectionTransform current, const std::string& property,
                             const std::vector<float>& value )
    {
        glm::vec3* field = nullptr;
        if ( property == kSelectionTranslation )
            field = &current.Translation;
        else if ( property == kSelectionRotation )
            field = &current.Rotation;
        else if ( property == kSelectionScale )
            field = &current.Scale;
        if ( field == nullptr )
            return Common::MakeFormattedError<SelectionTransform>(
                 "'{}' is not a property of the selection. It offers '{}', '{}' and '{}'; ask 'properties' for "
                 "the same subject to see their current values.",
                 property, kSelectionTranslation, kSelectionRotation, kSelectionScale );
        if ( value.size() != 3 )
            return Common::MakeFormattedError<SelectionTransform>( "'{}' takes three numbers and was given {}.",
                                                                   property, value.size() );
        for ( const float component : value )
            if ( !std::isfinite( component ) )
                return Common::MakeFormattedError<SelectionTransform>(
                     "'{}' was given a component that is not a finite number; it would reach the entity's world "
                     "matrix and the entity would vanish.",
                     property );
        *field = glm::vec3( value[0], value[1], value[2] );
        return Common::MakeSuccess( current );
    }
} // namespace Desert::Editor::Core
