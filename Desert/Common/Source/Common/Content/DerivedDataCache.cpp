#include "DerivedDataCache.hpp"

#include <Common/Core/Constants.hpp>
#include <Common/Settings/MachineSettings.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/PakFile.hpp>
#include <Common/Utilities/VFS.hpp>

#include <cstring>
#include <format>
#include <vector>

namespace Common::DDC
{
    uint64_t MakeKey( const Deriver& deriver, const uint64_t payloadHash, const void* settings,
                      const size_t settingsSize )
    {
        // One contiguous image hashed once: the field boundaries are fixed-width except the two
        // variable parts, and the bucket is length-prefixed so "ab"+"c" and "a"+"bc" cannot collide.
        std::vector<unsigned char> image;
        const auto                 append = [&image]( const void* data, const size_t size )
        {
            const auto* bytes = static_cast<const unsigned char*>( data );
            image.insert( image.end(), bytes, bytes + size );
        };
        const uint64_t bucketSize = deriver.Bucket.size();
        append( &deriver.Version.Hi, sizeof( deriver.Version.Hi ) );
        append( &deriver.Version.Lo, sizeof( deriver.Version.Lo ) );
        append( &bucketSize, sizeof( bucketSize ) );
        append( deriver.Bucket.data(), deriver.Bucket.size() );
        append( &payloadHash, sizeof( payloadHash ) );
        if ( settings != nullptr && settingsSize > 0 )
            append( settings, settingsSize );
        return Utils::PakContentHash( image.data(), image.size() );
    }

    std::filesystem::path ResolveRoot( const std::string_view setting, const std::filesystem::path& projectDir )
    {
        if ( setting.empty() )
            return ( projectDir / "DerivedDataCache" ).lexically_normal();
        const std::filesystem::path configured( setting );
        return ( configured.is_absolute() ? configured : projectDir / configured ).lexically_normal();
    }

    std::filesystem::path Root()
    {
        return ResolveRoot( Settings::MachineSettings::Get().DerivedDataCachePath,
                            Constants::Path::CurrentProjectRoot().ProjectDir );
    }

    std::filesystem::path RelativePath( const Deriver& deriver, const uint64_t key )
    {
        const std::string hex = std::format( "{:016x}", key );
        return std::filesystem::path( "Buckets" ) / deriver.Bucket / hex.substr( 0, 2 ) / hex.substr( 2, 2 ) /
               ( hex.substr( 4 ) + std::string( deriver.Extension ) );
    }

    std::filesystem::path PathFor( const Deriver& deriver, const uint64_t key )
    {
        return Root() / RelativePath( deriver, key );
    }

    std::filesystem::path BucketDir( const std::string_view bucket )
    {
        return Root() / "Buckets" / bucket;
    }

    std::optional<std::string> Get( const Deriver& deriver, const uint64_t key )
    {
        const std::filesystem::path path = PathFor( deriver, key );
        std::error_code             ec;
        if ( std::filesystem::is_regular_file( path, ec ) )
        {
            auto read = Utils::FileSystem::ReadFileContent( path );
            if ( read.IsSuccess() )
                return read.ExtractValue();
        }
        return Utils::VFS::ReadFile( PackagedPath( path ) );
    }

    bool Put( const Deriver& deriver, const uint64_t key, const std::string_view bytes )
    {
        const std::filesystem::path path = PathFor( deriver, key );
        std::error_code             ec;
        std::filesystem::create_directories( path.parent_path(), ec );
        return Utils::FileSystem::WriteContentToFileAtomic( path, std::string( bytes ) ).IsSuccess();
    }

    std::filesystem::path PackagedPath( const std::filesystem::path& loosePath )
    {
        const std::string relative = loosePath.lexically_normal().lexically_relative( Root() ).generic_string();
        if ( relative.empty() || relative == "." || relative.rfind( "..", 0 ) == 0 )
            return loosePath;
        return Constants::Path::COOKED_PATH / relative;
    }

    std::string_view CookPlatformName()
    {
#if defined( _WIN32 )
        return "Windows";
#elif defined( __APPLE__ )
        return "MacOS";
#else
        return "Linux";
#endif
    }

    std::filesystem::path PlatformCookedDir()
    {
        return ( Constants::Path::CurrentProjectRoot().ProjectDir / "Saved" / "Cooked" / CookPlatformName() )
             .lexically_normal();
    }
} // namespace Common::DDC
