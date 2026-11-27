#include "EngineConfig.hpp"

#include "ProjectContext.hpp"

#include <Common/Core/Logger.hpp>
#include <Common/Json/Document.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <filesystem>
#include <format>
#include <optional>
#include <utility>
#include <Common/Json/ReadWrite.hpp>
#include <Common/Json/DocumentStruct.hpp>

namespace Desert::Project
{
    namespace
    {
        // The project directory the layers were read for; nullopt = read again (before the first read, after a
        // save). Shared with SaveEngineConfig so a save is what the next read returns.
        std::optional<std::string> engineLoadedFor;
        EngineConfigLayers         engineLayers;
        // THE COMMAND-LINE LAYER (SetEngineConfigOverrides), verified when set: each item's key path and its value
        // nested under that path as an object, in the order given. Applied by CurrentEngineConfig over the file
        // layers; SaveEngineConfig never writes it.
        std::vector<std::pair<std::string, Common::Json::Value>> overrides;

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
            // On disk OR in a mounted archive: a packaged game's Config/ ships inside its pak
            // (Editor/Packaging/PackagedContentTrees.hpp), where std::filesystem sees nothing.
            if ( !Common::Utils::FileSystem::Exists( path ) )
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

        // @p value under the dotted @p path as nested objects: "Net.BudgetBytesPerSecond" and 2048 give
        // {"Net": {"BudgetBytesPerSecond": 2048}}. Refuses a path with an empty key ("Net..X", ".X", "X.").
        Common::ResultStr<Common::Json::Value> NestedAt( std::string_view path, Common::Json::Value value )
        {
            std::vector<std::string_view> names;
            for ( std::size_t at = 0;; )
            {
                const std::size_t dot  = path.find( '.', at );
                const auto        name = path.substr( at, dot - at ); // npos - at: to the end
                if ( name.empty() )
                    return Common::MakeError<Common::Json::Value>(
                         std::format( "the key path '{}' has an empty key", path ) );
                names.push_back( name );
                if ( dot == std::string_view::npos )
                    break;
                at = dot + 1;
            }
            for ( auto name = names.rbegin(); name != names.rend(); ++name )
            {
                Common::Json::ObjectBuilder level;
                level.Set( *name, value );
                value = Common::Json::Value( level.Build() );
            }
            return Common::MakeSuccess( std::move( value ) );
        }

        // The file layers of @p directory into @p layers (Project, Effective, PlatformFile, PlatformKeys), each
        // refusal logged; returns the JSON that layers.Effective was read from - the base the command-line layer
        // goes over ({} = the defaults).
        Common::Json::Value ReadFileLayers( const std::string& directory, EngineConfigLayers& layers )
        {
            Common::Json::Value base = Common::Json::Value( Common::Json::Object{} );

            const std::filesystem::path projectFile = ProjectLayerFile( directory );
            auto                        project     = ReadLayer( projectFile );
            if ( !project )
            {
                LOG_ERROR( "[Project] {} does not parse, the project runs the engine defaults: {}",
                           projectFile.string(), project.GetError() );
                return base;
            }
            if ( project.GetValue().has_value() )
            {
                auto read = AsConfig( *project.GetValue(), projectFile );
                if ( !read )
                {
                    LOG_ERROR( "[Project] the project runs the engine defaults: {}", read.GetError() );
                    return base;
                }
                layers.Project = read.GetValue();
                base           = *project.GetValue();
            }
            layers.Effective = layers.Project;

            const std::filesystem::path platformFile = PlatformLayerFile( directory );
            auto                        platform     = ReadLayer( platformFile );
            if ( !platform )
            {
                LOG_ERROR( "[Project] {} does not parse, this platform runs the project layer: {}",
                           platformFile.string(), platform.GetError() );
                return base;
            }
            if ( !platform.GetValue().has_value() )
                return base;

            std::vector<std::string>  keys;
            Common::Json::Value layered = Overlay(
                 Common::Json::Root( base ), Common::Json::Root( *platform.GetValue() ), std::string{}, keys );
            auto effective = AsConfig( layered, platformFile );
            if ( !effective )
            {
                LOG_ERROR( "[Project] this platform runs the project layer: {}", effective.GetError() );
                return base;
            }
            layers.Effective    = effective.GetValue();
            layers.PlatformFile = platformFile.string();
            layers.PlatformKeys = std::move( keys );
            std::string list;
            for ( const std::string& key : layers.PlatformKeys )
                list += std::format( "{}{}", list.empty() ? "" : ", ", key );
            LOG_INFO( "[Project] {} sets {} key(s) on this platform: {}", layers.PlatformFile,
                      layers.PlatformKeys.size(), list );
            return layered;
        }

        // The command-line layer over @p layered, item by item, into layers.Effective and layers.OverrideKeys.
        // Every item was read strictly over the defaults when it was set, and an item replaces whole what its
        // path names, so the read here is the same read: a failure is logged as the one it would be.
        void ApplyOverrides( Common::Json::Value layered, EngineConfigLayers& layers )
        {
            if ( overrides.empty() )
                return;
            for ( const auto& item : overrides )
            {
                std::vector<std::string>  replaced; // OverrideKeys are the items' own paths, below
                const Common::Json::Value over = Overlay( Common::Json::Root( layered ),
                                                          Common::Json::Root( item.second ), std::string{}, replaced );
                layered = over;
            }
            auto effective = Common::Json::Root( layered ).As<EngineConfig>();
            if ( !effective )
            {
                LOG_ERROR( "[Project] the --config layer does not read, this host runs the file layers: {}",
                           effective.GetError() );
                return;
            }
            layers.Effective = effective.GetValue();
            std::string list;
            for ( const auto& item : overrides )
            {
                layers.OverrideKeys.push_back( item.first );
                list += std::format( "{}{}", list.empty() ? "" : ", ", item.first );
            }
            LOG_INFO( "[Project] --config sets {} key(s): {}", layers.OverrideKeys.size(), list );
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
        // No project: the defaults, with the command-line layer over them (it belongs to the process, not to a
        // project).
        Common::Json::Value layered = directory.empty() ? Common::Json::Value( Common::Json::Object{} )
                                                        : ReadFileLayers( directory, engineLayers );
        ApplyOverrides( std::move( layered ), engineLayers );
        return engineLayers;
    }

    Common::BoolResultStr SetEngineConfigOverrides( std::span<const std::string> items )
    {
        std::vector<std::pair<std::string, Common::Json::Value>> parsed;
        parsed.reserve( items.size() );
        // Each item is read strictly over the defaults and the items before it: the same read the file layers get,
        // so a key the struct does not declare, or a value of the wrong type, is refused here, at the start.
        Common::Json::Value checked = Common::Json::FromStruct( EngineConfig{} );
        for ( const std::string& item : items )
        {
            const std::size_t equals = item.find( '=' );
            if ( equals == std::string::npos )
                return Common::MakeError<bool>(
                     std::format( "'{}': no '=' between the key path and its JSON value", item ) );
            const std::string path = item.substr( 0, equals );
            if ( path.empty() )
                return Common::MakeError<bool>( std::format( "'{}': the key path before '=' is empty", item ) );
            auto value = Common::Json::Parse( std::string_view( item ).substr( equals + 1 ) );
            if ( !value )
                return Common::MakeError<bool>(
                     std::format( "'{}': the value is not JSON (a string is written in quotes): {}", item,
                                  value.GetError() ) );
            auto nested = NestedAt( path, value.ExtractValue() );
            if ( !nested )
                return Common::MakeError<bool>( std::format( "'{}': {}", item, nested.GetError() ) );
            std::vector<std::string>  replaced;
            const Common::Json::Value over =
                 Overlay( Common::Json::Root( checked ), Common::Json::Root( nested.GetValue() ), std::string{},
                          replaced );
            if ( auto read = Common::Json::Root( over ).As<EngineConfig>(); !read )
                return Common::MakeError<bool>( std::format( "'{}': {}", item, read.GetError() ) );
            checked = over;
            parsed.emplace_back( path, nested.ExtractValue() );
        }
        overrides = std::move( parsed );
        engineLoadedFor.reset(); // the next read applies them
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr SaveEngineConfig( const EngineConfig& projectLayer )
    {
        if ( !ProjectContext::HasProject() )
            return Common::MakeError<bool>( "the engine settings were not saved: no project is open" );
        if ( const auto invalid = ValidateNetSettings( projectLayer.Net ); invalid.has_value() )
            return Common::MakeError<bool>( std::format( "the engine settings were not saved: {}", *invalid ) );

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
