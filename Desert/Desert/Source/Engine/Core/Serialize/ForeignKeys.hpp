#pragma once

// A KEY THIS BUILD DOES NOT DECLARE IS NOT THIS BUILD'S TO DELETE.
//
// THE DEFECT. Every writer in this tree enumerates ITS OWN REGISTRY and not the file it is rewriting:
// SceneSerializer.cpp builds the Settings block from SceneSettings' 51 reflected fields,
// EntitySerializer.cpp builds each entity from ComponentRegistry's 45 keys, and
// ReflectionSerializer.cpp walks `type.Fields` in both directions. So a key that is in the file and
// not in the registry is never read, never written, and gone the moment anything saves — silently,
// and with no way for either side to notice.
//
// WHY THAT IS NOT AN EDGE CASE HERE. The scene format broke thirteen times in twenty-eight days (one
// generation every 2.15 days, five of them inside nine hours), and this repository routinely has ten
// worktrees alive at different commits, each able to open the editor and save a scene. The version
// gate does not cover it: a field ADDED needs no version bump (a missing key already defaults), and
// thirty fields have been REMOVED with no bump either — so the foreign key travels inside a file at
// the CURRENT generation, which is exactly the file the gate waves through.
//
// AND THE TREE ALREADY HELD TWO CONTRADICTORY ANSWERS. Tools/SceneMigrator PRESERVES what it does not
// know (`kept[key] = value` appears sixteen times in SceneMigration.cpp, and the tree it writes back
// is the tree it parsed); the editor's saver DESTROYS it. PrefabData.hpp even promises "full
// back/forward compatibility", which was true of the migrator and false of the editor. This file
// makes the migrator's answer the one answer, because it is the one that does not lose data.
//
// WHAT THIS IS NOT. It is not a compatibility shim and it does not keep legacy alive (contract §4). A
// key this project DELETED ON PURPOSE is not unknown, it is RETIRED, and retirement happens in
// Tools/SceneMigrator: a migration step names it, drops it from the files, and the version bump makes
// the run compulsory. Preservation is only ever for keys another BUILD owns. Nothing here has a list
// of key names in it, and nothing here should ever grow one.
//
// PURE. No GPU, no scene, no filesystem, no globals — in: two Nodes over parsed trees; out: one object. That is what
// lets Desert/Tests/Engine/SceneForeignKeys assert the whole class over the real corpus.

#include <Common/Json/Document.hpp>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Desert::Core::Serialize
{
    // "Does the writer at THIS level state this key whenever it has one?"
    //
    // It exists for exactly one level and the reason is worth stating, because everywhere else the
    // question does not arise. SerializeReflected writes EVERY field of its type unconditionally, so
    // at a settings or component-data level "absent from the fresh tree" already means "not a field
    // of this build" — nothing else can produce that. EntitySerializer is the exception: it writes a
    // component key only when the entity HAS that component, so an entity whose light the user just
    // DELETED looks exactly like an entity carrying a component this build never heard of. Without
    // this predicate the merge below would faithfully resurrect every component anyone removed.
    using KeyIsOurs = std::function<bool( const std::string& )>;

    // Nothing at this level is ours, so every key the fresh tree omits is foreign and is kept. The
    // default for every level except an entity record.
    [[nodiscard]] KeyIsOurs NothingIsOurs();

    // `fresh` wins for every key it states. `source` contributes only the keys `fresh` does not state
    // AND that `ours` does not claim. Where a key is an object in both, the two are merged the same
    // way one level down (with NothingIsOurs, for the reason above).
    //
    // Arrays are NOT merged element by element — a fresh array replaces the source's. Entities are the
    // one array whose elements have identity, and MergeSceneDocument below is what knows that.
    [[nodiscard]] Common::Json::Object MergeObjects( const Common::Json::Node& fresh, const Common::Json::Node& source,
                                                     const KeyIsOurs& ours );

    // The whole .desce, merged: the top level by the rule above, and `Entities` element by element,
    // matched on the record's `id`. A record in `source` whose id is not in `fresh` is DROPPED — that
    // is an entity the user deleted, and preserving it would make deletion impossible.
    //
    // `entityKeyIsOurs` is asked about the keys of one entity record (its meta members and the
    // component keys this build's registry holds). Everything below that answers to NothingIsOurs.
    [[nodiscard]] Common::Json::Object MergeSceneDocument( const Common::Json::Node& fresh,
                                                           const Common::Json::Node& source,
                                                           const KeyIsOurs&          entityKeyIsOurs );

    // Every key at THIS ONE LEVEL of `source` that `ours` does not claim, counted by NAME and not by
    // path: a key that appears on forty entities is one finding with a count of forty, not forty log
    // lines. The order is the map's, so a report of it is stable between runs.
    //
    // DELIBERATELY NOT RECURSIVE. Each level of a .desce answers to a DIFFERENT registry — the top to
    // SceneSerialized's members, the settings block to SceneSettings' reflected fields, an entity
    // record to the component registry — and a recursion would have to carry a table of which
    // registry answers where, which is a second statement of the format. The caller walks the levels
    // it knows and asks here with the right predicate for each.
    void CountForeignKeysAtLevel( const Common::Json::Node& source, const KeyIsOurs& ours,
                                  std::map<std::string, int>& into );

    // "Foo (x40), Bar" — the one line a load logs. Empty when the map is empty, so the caller can
    // test the string rather than the map.
    [[nodiscard]] std::string DescribeForeignKeys( const std::map<std::string, int>& counted );
} // namespace Desert::Core::Serialize
