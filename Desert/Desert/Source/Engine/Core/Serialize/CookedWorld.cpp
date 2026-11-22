#include <Engine/Core/Serialize/CookedWorld.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <Common/Content/ContentChunks.hpp>
#include <Common/Json/Json.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <utility>
#include <vector>

namespace Desert::Core
{
    // A cooked file's bytes through the VFS, so a pak and a loose directory read the same way. Called on
    // JobSystem workers; a pak read opens its own stream (PakFile.cpp), so it is safe there. A file the
    // packager shipped in an on-demand region chunk (WP23) mounts that chunk first — once, thread-safe; with
    // no chunks registered (editor, loose cooked files) the mount does nothing.
    WorldCells::FileReader CookedFileReader( std::string directory, RegionChunks regions )
    {
        return [directory = std::move( directory ), regions = std::move( regions )](
                    std::string_view name ) -> Common::ResultStr<std::vector<unsigned char>>
        {
            const std::string path = directory + std::string( name );
            if ( regions )
                if ( const auto chunk = regions->find( std::string( name ) ); chunk != regions->end() )
                    if ( auto mounted = Common::Content::MountChunkOnDemand( chunk->second ); !mounted )
                        return Common::MakeError<std::vector<unsigned char>>( "cooked world file '" + path +
                                                                              "' (chunk '" + chunk->second +
                                                                              "'): " + mounted.GetError() );
            auto read = Common::Utils::FileSystem::ReadByteFileContent( path );
            if ( !read )
                return Common::MakeError<std::vector<unsigned char>>( "cooked world file '" + path +
                                                                      "': " + read.GetError() );
            return Common::MakeSuccess( read.ExtractValue() );
        };
    }

    std::uint64_t CookHashOf( const std::span<const unsigned char> indexBytes )
    {
        std::uint64_t hash = 14695981039346656037ull;
        for ( const unsigned char byte : indexBytes )
        {
            hash ^= byte;
            hash *= 1099511628211ull;
        }
        return hash;
    }

    Common::ResultStr<std::optional<CookedWorldStart>> ReadCookedWorld( const std::string& scenePath )
    {
        using Result = std::optional<CookedWorldStart>;
        CookedWorldStart world;
        world.Directory             = WorldCells::CookedWorldDirectory( scenePath );
        const std::string indexPath = world.Directory + std::string( WorldCells::kIndexFileName );
        if ( !Common::Utils::FileSystem::Exists( indexPath ) )
            return Common::MakeSuccess( Result() );

        // The index ships in the world's Shared chunk, mounted at start: read before any region is known.
        auto bytes = CookedFileReader( world.Directory, nullptr )( WorldCells::kIndexFileName );
        if ( !bytes )
            return Common::MakeError<Result>( bytes.GetError() );
        auto index = WorldCells::ReadWorldIndex( indexPath, bytes.GetValue() );
        if ( !index )
            return Common::MakeError<Result>( index.GetError() );
        world.Index    = std::make_shared<WorldCells::WorldIndex>( index.ExtractValue() );
        world.CookHash = CookHashOf( bytes.GetValue() );

        auto layout = WorldCells::ChunkLayoutOf( *world.Index, world.Directory );
        if ( !layout )
            return Common::MakeError<Result>( indexPath + ": " + layout.GetError() );
        if ( layout.GetValue().has_value() )
        {
            auto regions = std::make_shared<std::map<std::string, std::string>>();
            for ( const auto& [file, chunk] : layout.GetValue()->FileChunk )
                if ( chunk != layout.GetValue()->Shared )
                    regions->emplace( file, chunk );
            world.RegionChunkOfFile = std::move( regions );
        }
        const WorldCells::FileReader reader = CookedFileReader( world.Directory, world.RegionChunkOfFile );

        auto indexed = WorldCells::PlanFromIndex( *world.Index );
        if ( !indexed )
            return Common::MakeError<Result>( indexPath + ": " + indexed.GetError() );
        world.Indexed = indexed.ExtractValue();

        auto alwaysLoaded = WorldCells::AssembleAlwaysLoaded( *world.Index, reader );
        if ( !alwaysLoaded )
            return Common::MakeError<Result>( alwaysLoaded.GetError() );
        world.AlwaysLoadedJson = Common::Json::Write( alwaysLoaded.GetValue() );
        return Common::MakeSuccess( Result( std::move( world ) ) );
    }
} // namespace Desert::Core
