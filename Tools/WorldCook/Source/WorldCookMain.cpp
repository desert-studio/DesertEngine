#include "WorldCookMain.hpp"

#include <Engine/Core/Serialize/WorldCells.hpp>

#include <Common/Utilities/AssetRegistry.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <ostream>
#include <set>

namespace Desert::WorldCook
{
    namespace
    {
        namespace fs = std::filesystem;

        std::string Usage()
        {
            return "usage: WorldCook <source.desce> --out <directory> (--registry <file.dreg>... | --no-registry) "
                   "[--verify]";
        }

        struct Options
        {
            fs::path              Source;
            fs::path              Out;
            std::vector<fs::path> Registries;
            bool                  NoRegistry = false;
            bool                  Verify     = false;
        };

        Common::ResultStr<Options> Parse( const std::vector<std::string>& args )
        {
            Options options;
            for ( std::size_t at = 0; at < args.size(); ++at )
            {
                const std::string& arg     = args[at];
                const auto         takeOne = [&]( const char* flag ) -> Common::ResultStr<std::string>
                {
                    if ( at + 1 >= args.size() )
                        return Common::MakeError<std::string>( std::string( flag ) + " needs a value" );
                    return Common::MakeSuccess( args[++at] );
                };
                if ( arg == "--out" || arg == "--registry" )
                {
                    auto value = takeOne( arg.c_str() );
                    if ( !value )
                        return Common::MakeError<Options>( value.GetError() );
                    if ( arg == "--out" )
                        options.Out = value.GetValue();
                    else
                        options.Registries.emplace_back( value.GetValue() );
                }
                else if ( arg == "--no-registry" )
                    options.NoRegistry = true;
                else if ( arg == "--verify" )
                    options.Verify = true;
                else if ( !arg.empty() && arg[0] == '-' )
                    return Common::MakeError<Options>( "unknown option '" + arg + "'" );
                else if ( options.Source.empty() )
                    options.Source = arg;
                else
                    return Common::MakeError<Options>( "a second source '" + arg + "'; one world per run" );
            }
            if ( options.Source.empty() || options.Out.empty() )
                return Common::MakeError<Options>( "a source and --out are required" );
            // Without a registry mesh assets are placed by position alone and no unit's assets are known.
            // That is a legitimate cook (a world of primitives needs none), but it is CHOSEN, never defaulted.
            if ( options.Registries.empty() == !options.NoRegistry )
                return Common::MakeError<Options>( "state exactly one of --registry <file>... or --no-registry" );
            return Common::MakeSuccess( std::move( options ) );
        }

        double MsSince( std::chrono::steady_clock::time_point start )
        {
            return std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - start ).count();
        }
    } // namespace

    int RunWorldCook( const std::vector<std::string>& args, std::ostream& out, std::ostream& err )
    {
        auto parsed = Parse( args );
        if ( !parsed )
        {
            err << "WorldCook: " << parsed.GetError() << "\n" << Usage() << "\n";
            return 2;
        }
        const Options options = parsed.ExtractValue();

        const auto started = std::chrono::steady_clock::now();
        auto       text    = Common::Utils::FileSystem::ReadFileContent( options.Source );
        if ( !text )
        {
            err << "WorldCook: " << text.GetError() << "\n";
            return 3;
        }
        auto scene = Core::ParseLoadableScene( options.Source.string(), text.GetValue() );
        if ( !scene )
        {
            err << "WorldCook: " << scene.GetError() << "\n";
            return 3;
        }
        std::vector<Common::Utils::AssetRegistry> registries;
        for ( const fs::path& path : options.Registries )
        {
            auto registry = Common::Utils::AssetRegistry::LoadFrom( path );
            if ( !registry )
            {
                err << "WorldCook: registry '" << path.string() << "': " << registry.GetError() << "\n";
                return 3;
            }
            registries.push_back( registry.ExtractValue() );
        }
        const double readMs = MsSince( started );

        const auto cookStart = std::chrono::steady_clock::now();
        auto       cooked    = Core::WorldCells::CookWorld( scene.GetValue(), registries );
        if ( !cooked )
        {
            err << "WorldCook: " << cooked.GetError() << "\n";
            return 4;
        }
        const double cookMs = MsSince( cookStart );

        // Written whole or not at all, file by file; then whatever an earlier cook left that this one does not
        // name is removed, so the directory is exactly one cook. Only the cook's own extensions are touched.
        const auto      writeStart = std::chrono::steady_clock::now();
        std::error_code made;
        fs::create_directories( options.Out, made );
        if ( made )
        {
            err << "WorldCook: cannot create '" << options.Out.string() << "': " << made.message() << "\n";
            return 5;
        }
        std::set<std::string> names;
        std::size_t           totalBytes = 0;
        std::size_t           largest    = 0;
        std::string           largestName;
        for ( const auto& file : cooked.GetValue().Files )
        {
            names.insert( file.Name );
            totalBytes += file.Bytes.size();
            if ( file.Name != Core::WorldCells::kIndexFileName && file.Bytes.size() > largest )
            {
                largest     = file.Bytes.size();
                largestName = file.Name;
            }
            const auto written = Common::Utils::FileSystem::WriteBytesToFileAtomic(
                 options.Out / file.Name, std::as_bytes( std::span( file.Bytes ) ) );
            if ( !written )
            {
                err << "WorldCook: " << written.GetError() << "\n";
                return 5;
            }
        }
        std::size_t removed = 0;
        for ( const auto& entry : fs::directory_iterator( options.Out ) )
        {
            const std::string name = entry.path().filename().string();
            const bool        ours = entry.path().extension() == Core::WorldCells::kCellExtension ||
                              name == Core::WorldCells::kIndexFileName;
            if ( ours && names.count( name ) == 0 )
            {
                std::error_code gone;
                removed += fs::remove( entry.path(), gone ) ? 1 : 0;
                if ( gone )
                {
                    err << "WorldCook: cannot remove the stale '" << entry.path().string()
                        << "': " << gone.message() << "\n";
                    return 5;
                }
            }
        }
        const double writeMs = MsSince( writeStart );

        const auto& index      = cooked.GetValue().Index;
        const auto& files      = cooked.GetValue().Files;
        std::size_t alwaysUnit = 0;
        for ( const auto& unit : index.Units )
            alwaysUnit += unit.Level.has_value() ? 0 : 1;
        out << "WorldCook: '" << index.SceneName << "' -> " << options.Out.string() << "\n"
            << "  records      : " << index.Records << "\n"
            << "  units        : " << index.Units.size() << " (" << index.Units.size() - alwaysUnit << " cells, "
            << alwaysUnit << " always-loaded)\n"
            << "  files        : " << files.size() << " (" << files.size() - 1 << " cell files + index), "
            << totalBytes << " bytes\n"
            << "  index        : " << files.back().Bytes.size() << " bytes\n"
            << "  largest cell : " << largestName << ", " << largest << " bytes\n"
            << "  references   : " << index.References.size() << " crossing a unit\n"
            << "  assets       : "
            << ( index.AssetClosureKnown ? "closure per unit from the registry" : "UNKNOWN (--no-registry)" )
            << "\n"
            << "  stale removed: " << removed << "\n"
            << "  time         : read " << readMs << " ms, cook " << cookMs << " ms, write " << writeMs << " ms\n";

        if ( !options.Verify )
            return 0;

        // THE ROUND TRIP, FROM DISK: what a reader gets back out of the directory holds exactly the source's
        // records. Read through the same functions the runtime will use.
        const auto verifyStart = std::chrono::steady_clock::now();
        const auto reader      = [&]( std::string_view name ) -> Common::ResultStr<std::vector<unsigned char>>
        {
            auto bytes = Common::Utils::FileSystem::ReadFileContent( options.Out / std::string( name ) );
            if ( !bytes )
                return Common::MakeError<std::vector<unsigned char>>( bytes.GetError() );
            const std::string& held = bytes.GetValue();
            return Common::MakeSuccess( std::vector<unsigned char>( held.begin(), held.end() ) );
        };
        auto indexBytes = reader( Core::WorldCells::kIndexFileName );
        if ( !indexBytes )
        {
            err << "WorldCook: verify: " << indexBytes.GetError() << "\n";
            return 6;
        }
        auto readIndex =
             Core::WorldCells::ReadWorldIndex( Core::WorldCells::kIndexFileName, indexBytes.GetValue() );
        if ( !readIndex )
        {
            err << "WorldCook: verify: " << readIndex.GetError() << "\n";
            return 6;
        }
        auto back = Core::WorldCells::AssembleWorld( readIndex.GetValue(), reader );
        if ( !back )
        {
            err << "WorldCook: verify: " << back.GetError() << "\n";
            return 6;
        }
        if ( Core::WorldCells::CanonicalRecords( back.GetValue() ) !=
             Core::WorldCells::CanonicalRecords( scene.GetValue() ) )
        {
            err << "WorldCook: verify: the world assembled from the cells does not hold the source's records\n";
            return 7;
        }
        out << "  verify       : " << back.GetValue().Entities.size() << " records back from the cells, equal to "
            << "the source (" << MsSince( verifyStart ) << " ms)\n";
        return 0;
    }
} // namespace Desert::WorldCook
