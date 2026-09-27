#pragma once

#include <filesystem>

namespace Common::Settings
{
    // THE ENGINE INSTALLATION'S PER-USER DIRECTORY, `<home>/.desertengine`, spelled in ONE place. It is shared by
    // the editor, the launcher and the tools (editor.json, projects.json, crash reports of hosts with no project
    // open); a shipped game uses GameUserDirectory instead. ProjectContext::ConfigDirectory decides which home
    // and creates it; the crash handler lives below the engine and cannot call that, so both compose through
    // this. PreferenceOwnership fails when the name is spelled anywhere else.
    [[nodiscard]] inline std::filesystem::path EngineUserDirectoryUnder( const std::filesystem::path& home )
    {
        return home / ".desertengine";
    }
} // namespace Common::Settings
