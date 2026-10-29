#pragma once

// THE PREFAB'S VERSION GATE — the same contract .desce files have, applied to .deprefab files.
//
// A prefab's payload is the scene's own EntityData, serialized by the same ComponentRegistry, so every
// schema step that moves Core::kSceneVersion moves this format with it. Until Д28 nothing recorded that:
// a .deprefab carried no version at all, so a format change broke every saved prefab SILENTLY and the
// first symptom was a user's failed (or, before Ф1, crashing) load. Scenes solved this with a stamp, a
// refusal and a migrator; prefabs get the identical three, sharing the scene's two generation integers
// rather than inventing a third numbering scheme that would have to be kept in step by hand.
//
// Everything here is PURE — no filesystem, no logging, no globals — for the reason SceneFormat's gate is:
// the caller has already read the text, the loader logs the refusal, and a test can hold every wording
// and every version to account without a GPU or an asset manager.

#include <Engine/Assets/Prefab/PrefabData.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <Common/Core/ResultStr.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace Desert::Assets
{
    // True when the parsed tree is at BOTH current generations — the only thing the loader accepts.
    // An ABSENT integer is version 0 and not "current": every stamp this engine writes states both
    // numbers, so a file missing one was written by something older (every pre-Д28 build, in this case).
    [[nodiscard]] inline bool PrefabIsAtCurrentVersion( const PrefabData& prefab )
    {
        return StatedVersion( prefab.Header, kSceneSchemaTag ) == Core::kSceneVersion &&
               StatedVersion( prefab.Header, kUnitSchemaTag ) == Core::kUnitVersion &&
               StatedVersion( prefab.Header, kWorldPartitionSchemaTag ) == Core::kWorldPartitionVersion &&
               StatedVersion( prefab.Header, kAssetRefSchemaTag ) == Core::kAssetRefVersion &&
               StatedVersion( prefab.Header, kLightSchemaTag ) == Core::kLightVersion;
    }

    // The refusal, as a string: which file, what it is, what this engine needs, and the exact command
    // that fixes it — the four things a reader must have to act without opening any source.
    [[nodiscard]] std::string RefusePrefabVersion( std::string_view source, int foundSceneVersion,
                                                   int foundUnitVersion, int foundWorldPartitionVersion,
                                                   int foundAssetRefVersion, int foundLightVersion );

    // Same, read straight off the parsed tree (absent integers become 0).
    [[nodiscard]] std::string RefusePrefabVersion( std::string_view source, const PrefabData& prefab );

    // Reads the JSON of a .deprefab and hands back the tree ONLY if this engine will load it: readable,
    // and at both current generations. Otherwise the error is the message the loader logs. The loader
    // calls THIS rather than repeating the checks, so there is one statement of what "loadable" means.
    [[nodiscard]] Common::ResultStr<PrefabData> ParseLoadablePrefab( std::string_view   source,
                                                                     const std::string& json );

    // The ONE writer of .deprefab text: stamps both current generations onto the tree and serializes it.
    // PrefabAsset::Serialize goes through here, and so does anything else that ever writes a prefab, so
    // "what the saver writes" and "what the gate accepts" meet in a single function a test can hold
    // together: WritePrefabJson(tree) must always satisfy ParseLoadablePrefab.
    [[nodiscard]] Common::ResultStr<std::string> WritePrefabJson( PrefabData prefab );

    // The same writer for a prefab that already HAS an identity (PrefabAsset::Serialize): @p identity is
    // the header GUID written, whatever @p prefab carries, so re-saving a prefab never mints a new GUID and
    // a foliage type or scene naming the old one keeps resolving. A null identity (no file has held this
    // prefab yet) is minted by the stamp, as above.
    [[nodiscard]] Common::ResultStr<std::string> WritePrefabJson( PrefabData                        prefab,
                                                                  const Common::Content::AssetGuid& identity );

    // The GUID a prefab's header states; null when it states none or not a readable one.
    [[nodiscard]] Common::Content::AssetGuid PrefabStatedGuid( const PrefabData& prefab );

    // HOW A FILE STATES A PREFAB IT INSTANTIATES (EntityData::PrefabPath): relative to the assets root, as
    // a material, a landscape layer and a mesh are stated, so the reference ships with the project and the
    // scene's dependency list can find the file it names (Core::StatedPathGuid resolves a relative path only).
    // A prefab outside the assets root keeps its absolute spelling — it has no project-relative one.
    //
    // THE WRITER AND EVERY READER GO THROUGH THESE TWO. The asset's own filepath is whatever spelling it was
    // created from (the Assets window hands an absolute path), and writing that verbatim made every prefab
    // instance placed in the editor unsaveable: the dependency gather refused the absolute spelling.
    [[nodiscard]] std::string StatePrefabPath( const std::filesystem::path& file );

    // The file a stated PrefabPath names: the content key expanded, an absolute path as is, a bare spelling refused.
    [[nodiscard]] std::filesystem::path ResolvePrefabPath( std::string_view stated );

} // namespace Desert::Assets
