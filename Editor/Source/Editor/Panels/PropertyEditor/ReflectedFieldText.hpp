#pragma once

#include <Common/Core/ResultStr.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace Desert::Editor
{
    class CommandHistory;

    /// One reflected field of a data block, addressed and valued as TEXT — the control channel's `properties`
    /// row for an entity's component or the world's SceneSettings (UE Remote Control: a property by path).
    struct ReflectedFieldRow
    {
        std::string Path; ///< "<TypeName>.<Field>[.<Sub>]" — what `set` addresses it by
        std::string Type; ///< FieldTypeName of the leaf
        std::string Text; ///< the current value in the form SetReflectedFieldFromText parses
    };

    /// Every leaf field of `object` (a `typeName` block) that has a Value form, structs walked, containers
    /// skipped (they have no Value form; ReadField says so).
    [[nodiscard]] std::vector<ReflectedFieldRow> DescribeReflectedFields( const std::string& typeName,
                                                                          const void*        object );

    /// THE DETAILS PANEL'S WRITE, REACHED BY TEXT: walks `fieldPath` ("Field" or "Struct.Field") inside the
    /// `typeName` block at `object`, parses `text` by the leaf's own type (bool true/false, an enumerator
    /// name or integer, decimal integers and asset ids, a number, "x,y,z" for vectors, a string verbatim),
    /// stores it through Reflection::WriteField (read-only / enum / Refuse rules) and records the same undo
    /// step a Details edit does (byte pair, or a string pair) — so it is undoable and dirties the scene.
    [[nodiscard]] Common::BoolResultStr SetReflectedFieldFromText( void* object, const std::string& typeName,
                                                                   std::string_view fieldPath,
                                                                   std::string_view text,
                                                                   CommandHistory&  history );
} // namespace Desert::Editor
