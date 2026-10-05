#include "GameSettings.hpp"

#include "ProjectContext.hpp"

#include <Common/Core/Logger.hpp>
#include <Common/Json/Json.hpp>

#include <filesystem>
#include <format>

namespace Desert::Project
{
    namespace
    {
        // The cache CurrentGameSettings() answers from, and the project directory it was filled for. One pair,
        // shared by the reader and SaveGameSettings, so a save is what the next read returns.
        std::string  loadedFor;
        GameSettings settings;

        std::filesystem::path GameSettingsFile( const std::string& projectDirectory )
        {
            return std::filesystem::path( projectDirectory ) / "Config" / "Game.json";
        }
    } // namespace

    const GameSettings& CurrentGameSettings()
    {
        const std::string directory = ProjectContext::HasProject() ? ProjectContext::Directory() : std::string{};
        if ( directory == loadedFor )
            return settings;
        loadedFor = directory;
        settings  = {};
        if ( directory.empty() )
            return settings;

        const std::filesystem::path path = GameSettingsFile( directory );
        std::error_code             exists;
        if ( !std::filesystem::exists( path, exists ) )
            return settings; // the project declares no game settings
        auto parsed = Common::Json::ReadFile<GameSettings>( path );
        if ( !parsed )
        {
            LOG_ERROR( "[Project] {} does not parse: {}", path.string(), parsed.GetError() );
            return settings;
        }
        settings = std::move( parsed.GetValue() );
        return settings;
    }

    Common::BoolResultStr SaveGameSettings( const GameSettings& toSave )
    {
        if ( !ProjectContext::HasProject() )
            return Common::MakeError<bool>( "the game settings were not saved: no project is open" );

        const std::string           directory = ProjectContext::Directory();
        const std::filesystem::path path      = GameSettingsFile( directory );
        std::error_code             ec;
        std::filesystem::create_directories( path.parent_path(), ec );
        if ( ec )
            return Common::MakeError<bool>(
                 std::format( "{} was not saved: cannot create its folder: {}", path.string(), ec.message() ) );

        const auto written = Common::Json::WriteFileAtomic( path, toSave );
        if ( !written )
            return Common::MakeError<bool>(
                 std::format( "{} was not saved: {}", path.string(), written.GetError() ) );

        loadedFor = directory;
        settings  = toSave;
        LOG_INFO( "[Project] saved the game settings -> {}", path.string() );
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Project
