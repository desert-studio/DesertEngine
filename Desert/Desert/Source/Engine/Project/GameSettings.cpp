#include "GameSettings.hpp"

#include "ProjectContext.hpp"

#include <Common/Core/Logger.hpp>

#include <rflcpp/rfl/DefaultIfMissing.hpp>
#include <rflcpp/rfl/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace Desert::Project
{
    const GameSettings& CurrentGameSettings()
    {
        static std::string  loadedFor;
        static GameSettings settings;
        const std::string   directory = ProjectContext::HasProject() ? ProjectContext::Directory() : std::string{};
        if ( directory == loadedFor )
            return settings;
        loadedFor = directory;
        settings  = {};
        if ( directory.empty() )
            return settings;

        const std::filesystem::path path = std::filesystem::path( directory ) / "Config" / "Game.json";
        std::ifstream               in( path, std::ios::binary );
        if ( !in )
            return settings; // the project declares no game settings
        std::stringstream text;
        text << in.rdbuf();
        auto parsed = rfl::json::read<GameSettings, rfl::DefaultIfMissing>( text.str() );
        if ( !parsed.has_value() )
        {
            LOG_ERROR( "[Project] {} does not parse: {}", path.string(), parsed.error().what() );
            return settings;
        }
        settings = std::move( parsed.value() );
        return settings;
    }
} // namespace Desert::Project
