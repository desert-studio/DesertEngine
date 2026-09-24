#pragma once

#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

// THE ONE WRITER OF TEXT ASSETS (AF6, decision D3). Scenes, materials and prefabs stay text, but until this
// every one of them was a single line of JSON, so any edit was a one-line diff of the whole file and a git
// merge of two edits was always a conflict. Every text asset kind is written through CanonicalJsonText, and
// the layout it produces is a function of the DOCUMENT alone: the same document always yields the same bytes.
//
// THE LAYOUT, and why each rule is there:
//   - one object member per line, indented four spaces per level, so an edit to one field is a diff of one
//     line and two edits to different fields of one entity merge cleanly;
//   - members keep the order of the input. The input is what rfl::json wrote, i.e. the declaration order of
//     the reflected struct (the schema order), and a merged document keeps the file's own order for keys the
//     build does not declare. Sorting alphabetically would scatter a component's fields away from its schema;
//   - an array of scalars of at most kCanonicalInlineArray elements sits on one line (a vec3, a colour, a
//     quaternion reads as one value); a longer one is broken into lines of that many values, so a change to
//     one sample of a long table is still a diff of one line and the file does not grow a line per number;
//   - an array that holds objects or arrays puts each element on its own lines;
//   - every scalar is spelled by the same yyjson writer rfl::json uses, so a number is the SHORTEST text that
//     reads back to the identical double (what std::to_chars produces) and a string is escaped exactly as
//     before. Reformatting therefore never changes a value: parsing the old single-line file and parsing the
//     canonical file yield the same document, which the CanonicalText suite proves over the whole corpus.
//   - the text ends with one newline.
//
// Considered: UE's TPrettyJsonPrintPolicy (Runtime/Json/Public/Policies/PrettyJsonPrintPolicy.h). Not ported:
// it indents with tabs, puts every array element on its own line (a vec3 becomes five lines) and prints
// through FJsonValue trees this engine does not have; the rules above are the part of it that matters.
namespace Common::Content
{
    constexpr std::size_t kCanonicalInlineArray = 8;

    // Re-lays-out any JSON text canonically. Fails, naming the byte offset, if the input is not JSON.
    ResultStr<std::string> CanonicalJsonText( std::string_view json );

    // The same layout for text a JSON WRITER just produced (rfl::json, yyjson). Such text is JSON by
    // construction, so a failure names the writer as the culprit - but it is still RETURNED, never aborted
    // on: the caller is a save, and a save that cannot produce its text must refuse with a reason and
    // leave the file on disk as it was, not take the editor down with the user's unsaved work.
    ResultStr<std::string> CanonicalJsonTextOfWriterOutput( std::string_view json );

    // The two steps of a text-asset save: the canonical layout of the writer's output, then the atomic
    // replace (FileSystem::WriteContentToFileAtomic - a temporary file renamed over the target). Either
    // failing refuses the save with the reason, and the file on disk stays what it was.
    ResultStr<bool> WriteCanonicalJsonFileAtomic( const std::filesystem::path& file, std::string_view json );

    // True if the text is already byte-identical to its canonical layout.
    bool IsCanonicalJsonText( std::string_view json );
} // namespace Common::Content
