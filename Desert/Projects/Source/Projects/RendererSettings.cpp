#include "RendererSettings.hpp"

#include "ProjectContext.hpp"

#include <Common/Core/Logger.hpp>
#include <Common/Json/Json.hpp>

#include <filesystem>
#include <format>

namespace Desert::Project
{
    namespace
    {
        // The cache CurrentRendererSettings() answers from and the project directory it was filled for, shared
        // with SaveRendererSettings so a save is what the next read returns (as GameSettings.cpp).
        std::string      rendererLoadedFor;
        RendererSettings rendererSettings;

        std::filesystem::path RendererSettingsFile( const std::string& projectDirectory )
        {
            return std::filesystem::path( projectDirectory ) / "Config" / "Engine.json";
        }
    } // namespace

    const RendererSettings& CurrentRendererSettings()
    {
        const std::string directory = ProjectContext::HasProject() ? ProjectContext::Directory() : std::string{};
        if ( directory == rendererLoadedFor )
            return rendererSettings;
        rendererLoadedFor = directory;
        rendererSettings  = {};
        if ( directory.empty() )
            return rendererSettings;

        const std::filesystem::path path = RendererSettingsFile( directory );
        std::error_code             exists;
        if ( !std::filesystem::exists( path, exists ) )
            return rendererSettings; // the project keeps the renderer's defaults
        auto parsed = Common::Json::ReadFile<RendererSettings>( path );
        if ( !parsed )
        {
            LOG_ERROR( "[Project] {} does not parse: {}", path.string(), parsed.GetError() );
            return rendererSettings;
        }
        rendererSettings = parsed.GetValue();
        return rendererSettings;
    }

    Common::BoolResultStr SaveRendererSettings( const RendererSettings& toSave )
    {
        if ( !ProjectContext::HasProject() )
            return Common::MakeError<bool>( "the renderer settings were not saved: no project is open" );

        const std::string           directory = ProjectContext::Directory();
        const std::filesystem::path path      = RendererSettingsFile( directory );
        std::error_code             ec;
        std::filesystem::create_directories( path.parent_path(), ec );
        if ( ec )
            return Common::MakeError<bool>(
                 std::format( "{} was not saved: cannot create its folder: {}", path.string(), ec.message() ) );

        const auto written = Common::Json::WriteFileAtomic( path, toSave );
        if ( !written )
            return Common::MakeError<bool>(
                 std::format( "{} was not saved: {}", path.string(), written.GetError() ) );

        rendererLoadedFor = directory;
        rendererSettings  = toSave;
        LOG_INFO( "[Project] saved the renderer settings -> {}", path.string() );
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Project
