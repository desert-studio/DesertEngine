#pragma once

// THE REGIONS THE EDITOR HAD LOADED, PER WORLD (WP19-REST) - UE's UWorldPartitionEditorPerProjectUserSettings
// (EditorLoadedRegions, kept per user and per map in Saved/Config, and loaded again when the map opens).
//
// The user's state, not the world's: it lives in <Project>/Saved/Config/WorldPartitionEditor.json, beside the
// editor layout, and never in the scene files (two people editing one world hold different regions). A world is
// keyed by its scene path relative to the project folder, so a project moved as a whole keeps its regions.
//
// Written by the World Partition panel after every region change that succeeded; read by the editor's open
// (SceneFiles::OpenScene → EditorRegions::ReadForOpen).

#include <Engine/Core/Serialize/EditorRegions.hpp>

#include <Common/Core/ResultStr.hpp>

#include <filesystem>

namespace Desert::Editor::EditorLoadedRegions
{
    // What the editor held of the world at @p scene when its regions last changed. A world never changed (or no
    // file yet) holds nothing besides its always-loaded part. Refused, naming the file, when the file exists and
    // cannot be read or parsed.
    [[nodiscard]] Common::ResultStr<::Desert::Core::EditorRegions::HeldRegions>
    Remembered( const std::filesystem::path& scene );

    // Records @p held as what the editor holds of the world at @p scene. Refused, naming the file, when it
    // cannot be read or written; the file on disk is then as it was.
    [[nodiscard]] Common::BoolResultStr Remember( const std::filesystem::path&                      scene,
                                                  const ::Desert::Core::EditorRegions::HeldRegions& held );
} // namespace Desert::Editor::EditorLoadedRegions
