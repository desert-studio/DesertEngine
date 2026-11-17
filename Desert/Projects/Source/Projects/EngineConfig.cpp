#include "EngineConfig.hpp"

#include "ProjectContext.hpp"

#include <Common/Core/Logger.hpp>
#include <Common/Json/Document.hpp>

#include <filesystem>
#include <format>
#include <optional>
#include <utility>

namespace Desert::Project
{
    namespace
    {
        // The project directory the layers were read for; nullopt = read again (before the first read, after a
        // save). Shared with SaveEngineConfig so a save is what the next read returns.
        std::optional<std::string> engineLoadedFor;
        EngineConfigLayers         engineLayers;

        std::filesystem::path ProjectLayerFile( const std::string& projectDirectory )
        {
            return std::filesystem::path( projectDirectory ) / "Config" / "Engine.json";
        }

        std::filesystem::path PlatformLayerFile( const std::string& projectDirectory )
        {
            return std::filesystem::path( projectDirectory ) / "Config" / std::string( PlatformConfigName() ) /
                   "Engine.json";
        }

        // The file as a JSON value; nullopt when the file does not exist.
        Common::ResultStr<std::optional<Common::Json::Value>> ReadLayer( const std::filesystem::path& path )
        {
            using Layer = std::optional<Common::Json::Value>;
            std::error_code exists;
            if ( !std::filesystem::exists( path, exists ) )
                return Common::MakeSuccess( Layer() );
            auto text = Common::Json::Detail::ReadFileText( path );
            if ( !text )
                return Common::MakeError<Layer>( text.GetError() );
            auto parsed = Common::Json::Parse( text.GetValue() );
            if ( !parsed )
                return Common::MakeError<Layer>( std::format( "{}: {}", path.string(), parsed.GetError() ) );
            return Common::MakeSuccess( Layer( parsed.ExtractValue() ) );
        }

        // @p over's members replace @p base's, recursing where both are objects (UE's ini layering, key by key).
        // Every key @p over replaces or adds is recorded in @p keys as its dotted path.
        Common::Json::Value Overlay( const Common::Json::Node& base, const Common::Json::Node& over,
                                     const std::string& prefix, std::vector<std::string>& keys )
        {
            if ( base.GetKind() != Common::Json::Kind::Object || over.GetKind() != Common::Json::Kind::Object )
            {
                keys.push_back( prefix );
                return over.Raw();
            }
            Common::Json::ObjectBuilder merged;
            base.ForEachMember( [&]( std::string_view name, const Common::Json::Node& member )
                                { merged.Set( name, member.Raw() ); } );
            over.ForEachMember(
                 [&]( std::string_view name, const Common::Json::Node& member )
                 {
                     const std::string path =
                          prefix.empty() ? std::string( name ) : std::format( "{}.{}", prefix, name );
                     if ( const auto under = base.Find( name ); under.has_value() )
                     {
                         const Common::Json::Value replaced = Overlay( *under, member, path, keys );
                         merged.Set( name, replaced );
                     }
                     else
                     {
                         keys.push_back( path );
                         merged.Set( name, member.Raw() );
                     }
                 } );
            return Common::Json::Value( merged.Build() );
        }

        Common::ResultStr<EngineConfig> AsConfig( const Common::Json::Value&   value,
                                                  const std::filesystem::path& file )
        {
            auto read = Common::Json::Root( value ).As<EngineConfig>();
            if ( !read )
                return Common::MakeError<EngineConfig>( std::format( "{}: {}", file.string(), read.GetError() ) );
            return read;
        }
    } // namespace

    std::string_view PlatformConfigName()
    {
#if defined( DESERT_PLATFORM_MACOS )
        return "Mac";
#elif defined( DESERT_PLATFORM_WINDOWS )
        return "Windows";
#elif defined( DESERT_PLATFORM_LINUX )
        return "Linux";
#else
#error "EngineConfig.cpp: no DESERT_PLATFORM_* define, so the platform layer of Engine.json has no folder to name."
#endif
    }

    const EngineConfigLayers& CurrentEngineConfig()
    {
        const std::string directory = ProjectContext::HasProject() ? ProjectContext::Directory() : std::string{};
        if ( engineLoadedFor == directory )
            return engineLayers;
        engineLoadedFor = directory;
        engineLayers    = {};
        if ( directory.empty() )
            return engineLayers;

        const std::filesystem::path projectFile = ProjectLayerFile( directory );
        auto                        project     = ReadLayer( projectFile );
        if ( !project )
        {
            LOG_ERROR( "[Project] {} does not parse, the project runs the engine defaults: {}",
                       projectFile.string(), project.GetError() );
            return engineLayers;
        }
        Common::Json::Value base = Common::Json::Value( Common::Json::Object{} );
        if ( project.GetValue().has_value() )
        {
            auto read = AsConfig( *project.GetValue(), projectFile );
            if ( !read )
            {
                LOG_ERROR( "[Project] the project runs the engine defaults: {}", read.GetError() );
                return engineLayers;
            }
            engineLayers.Project = read.GetValue();
            base                 = *project.GetValue();
        }
        engineLayers.Effective = engineLayers.Project;

        const std::filesystem::path platformFile = PlatformLayerFile( directory );
        auto                        platform     = ReadLayer( platformFile );
        if ( !platform )
        {
            LOG_ERROR( "[Project] {} does not parse, this platform runs the project layer: {}",
                       platformFile.string(), platform.GetError() );
            return engineLayers;
        }
        if ( !platform.GetValue().has_value() )
            return engineLayers;

        std::vector<std::string>  keys;
        const Common::Json::Value layered = Overlay(
             Common::Json::Root( base ), Common::Json::Root( *platform.GetValue() ), std::string{}, keys );
        auto effective = AsConfig( layered, platformFile );
        if ( !effective )
        {
            LOG_ERROR( "[Project] this platform runs the project layer: {}", effective.GetError() );
            return engineLayers;
        }
        engineLayers.Effective    = effective.GetValue();
        engineLayers.PlatformFile = platformFile.string();
        engineLayers.PlatformKeys = std::move( keys );
        std::string list;
        for ( const std::string& key : engineLayers.PlatformKeys )
            list += std::format( "{}{}", list.empty() ? "" : ", ", key );
        LOG_INFO( "[Project] {} sets {} key(s) on this platform: {}", engineLayers.PlatformFile,
                  engineLayers.PlatformKeys.size(), list );
        return engineLayers;
    }

    Common::BoolResultStr SaveEngineConfig( const EngineConfig& projectLayer )
    {
        if ( !ProjectContext::HasProject() )
            return Common::MakeError<bool>( "the engine settings were not saved: no project is open" );

        const std::filesystem::path path = ProjectLayerFile( ProjectContext::Directory() );
        std::error_code             ec;
        std::filesystem::create_directories( path.parent_path(), ec );
        if ( ec )
            return Common::MakeError<bool>(
                 std::format( "{} was not saved: cannot create its folder: {}", path.string(), ec.message() ) );

        const auto written = Common::Json::WriteFileAtomic( path, projectLayer );
        if ( !written )
            return Common::MakeError<bool>(
                 std::format( "{} was not saved: {}", path.string(), written.GetError() ) );

        engineLoadedFor.reset(); // the next read layers this platform's file over what was just written
        LOG_INFO( "[Project] saved the engine settings -> {}", path.string() );
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Project
