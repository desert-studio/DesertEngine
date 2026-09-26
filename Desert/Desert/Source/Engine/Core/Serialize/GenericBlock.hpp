#pragma once

// ONE MISSING FIELD USED TO DELETE THE WHOLE COMPONENT, AND NOTHING SAID SO.
//
// THE DEFECT, MEASURED. Eight components in ComponentRegistry.cpp are mapped through a reflect-cpp
// mirror struct — Script, StaticMesh, SkinnedMesh, InstancedStaticMesh, Material, UIAnim, Text and
// Animation. Reading one used to be `rfl::json::read<T>( ... )` with no processor, and reflect-cpp
// treats a MISSING field as an error even when the struct declares a default for it:
//
//     read<AnimationComponentSer>(R"({"CurrentClip":"Run","Playing":true,"Loop":true,"PlaybackSpeed":2})")
//       -> error: Found 2 errors: 1) Field named 'EnableRootMotion' not found.
//                                 2) Field named 'GraphJson' not found.
//
// The call sites turned that into `if ( !parsed.has_value() ) return;` — eight of them, none with a
// log line. So a block that was short of ONE field did not lose that field: the entity lost the whole
// component, silently. An animated character stopped being animated, a text element stopped having
// text, and the log had nothing in it.
//
// WHY THAT IS NOT HYPOTHETICAL. ForeignKeys.hpp states the rule this project works by — "a field
// ADDED needs no version bump, a missing key already defaults" — and builds the whole preservation
// argument on it. For these eight components the rule was FALSE: adding one field to a mirror struct
// silently voided every scene already written, and this repository routinely has ten worktrees alive
// at different commits, each able to open the editor and save. Two files stating opposite things
// about the same load is the shape §4 of the contract calls a relation defect; the comment was right
// and the code was wrong, so the code moved.
//
// THE FIX, AND WHERE IT LIVES NOW (JS1c). A block is read by Common::Json::Node::AsBlock<T>: an absent field
// takes the struct's own default, which is what every caller already believed, and an UNKNOWN extra field
// is tolerated, which is what lets an older build open a newer build's scene. A payload that is genuinely
// malformed — a string where a float belongs — still refuses the WHOLE block (the wrong-type rule for a
// Ser-struct, Common/Json/Document.hpp), and it refuses OUT LOUD: an Issue carrying the block's full path
// ("Entities[id=4127].Text") and reflect-cpp's reason, which the caller reports on the load's error line.
// An empty successful answer is a silent wrong answer (contract §1.4), and dropping a component with no
// line in the log was exactly that. Writing is Common::Json::FromStruct, the tree the text would parse to.
//
// PURE, AND A HEADER FOR THAT REASON. ComponentRegistry.cpp links the AssetManager and through it the
// renderer, so nothing defined inside it can be exercised by a suite. This function can, and
// Desert/Tests/Engine/GenericBlockRead does.

#include <Common/Json/Document.hpp>

#include <cstddef>
#include <optional>

namespace Desert::Core::Serialize
{
    // A component block read whole into its mirror struct, or nothing — never a part of one. On a refusal
    // `issues` gains the entry that says why, at the block's path; the caller reports it and leaves the
    // entity without the component.
    template <class T>
    std::optional<T> ReadBlock( const Common::Json::Node& block, Common::Json::Issues& issues )
    {
        T                 out;
        const std::size_t before = issues.size();
        block.ReadValue( out, issues );
        if ( issues.size() != before )
            return std::nullopt;
        return out;
    }
} // namespace Desert::Core::Serialize
