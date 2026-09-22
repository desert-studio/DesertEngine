#include <Common/Utilities/FileSystem.hpp>
#include "VFS.hpp"

#if defined( DESERT_PLATFORM_WINDOWS )
#include <Common/Platform/Windows/WindowsFileSystem.hpp>
#elif defined( DESERT_PLATFORM_MACOS )
#include <Common/Platform/MacOS/MacOSFileSystem.hpp>
#include <mach-o/dyld.h>
#endif

#include <Common/Core/Core.hpp>
#include <Common/Utilities/ContentScanLedger.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include <vector>

#ifdef _MSC_VER
#pragma warning( error : 4834 )
#endif

namespace fs = std::filesystem;

namespace Common::Utils
{
    class WindowsFileSystem;
    bool FileSystem::CreateDirectory( const std::filesystem::path& directory )
    {
        return fs::create_directory( directory );
    }

    bool FileSystem::CreateDirectory( const std::string& directory )
    {
        return CreateDirectory( fs::path( directory ) );
    }

    void FileSystem::CreateFile( const std::string& path )
    {
        CreateFile( fs::path( path ) );
    }

    void FileSystem::CreateFile( const std::filesystem::path& path )
    {
        std::ofstream file( path );

        if ( file.is_open() )
        {
            LOG_INFO( "Created File {}", path.string() );
            file.close();
        }
        else
        {
            // Same soft contract as the read primitives: name the failure, let the caller decide.
            LOG_ERROR( "[FileSystem] Could not create file: {}", path.string() );
        }
    }

    bool FileSystem::Exists( const std::filesystem::path& filepath )
    {
        return fs::exists( filepath ) || VFS::Exists( filepath );
    }

    bool FileSystem::Exists( const std::string& filepath )
    {
        return Exists( fs::path( filepath ) );
    }

    const std::string FileSystem::GetFileName( const std::filesystem::path& filepath )
    {
        return filepath.filename().string();
    }

    const std::string FileSystem::GetFileName( const std::string& filepath )
    {
        return std::filesystem::path( filepath ).filename().string();
    }

    std::filesystem::path FileSystem::OpenFileDialog( const char* filter )
    {
#if defined( DESERT_PLATFORM_WINDOWS )
        return WindowsFileSystem::OpenFileDialog( filter );
#elif defined( DESERT_PLATFORM_MACOS )
        return MacOSFileSystem::OpenFileDialog( filter );
#else
        (void)filter;
        return {};
#endif
    }

    std::filesystem::path FileSystem::OpenFolderDialog( const char* initialFolder )
    {
#if defined( DESERT_PLATFORM_WINDOWS )
        return WindowsFileSystem::OpenFolderDialog( initialFolder );
#elif defined( DESERT_PLATFORM_MACOS )
        return MacOSFileSystem::OpenFolderDialog( initialFolder );
#else
        (void)initialFolder;
        return {};
#endif
    }

    std::filesystem::path FileSystem::SaveFileDialog( const char* filter )
    {
#if defined( DESERT_PLATFORM_WINDOWS )
        return WindowsFileSystem::SaveFileDialog( filter );
#elif defined( DESERT_PLATFORM_MACOS )
        return MacOSFileSystem::SaveFileDialog( filter );
#else
        (void)filter;
        return {};
#endif
    }

    std::filesystem::path FileSystem::GetFileDirectory( const std::filesystem::path& filepath )
    {
        return filepath.parent_path();
    }

    std::filesystem::path FileSystem::ExecutablePath()
    {
#if defined( DESERT_PLATFORM_WINDOWS )
        // GROWS THE BUFFER, BECAUSE THE TRUNCATED ANSWER IS INDISTINGUISHABLE FROM THE RIGHT ONE.
        // `GetModuleFileNameW` with a MAX_PATH buffer does not fail on a longer path: it fills the
        // buffer, returns the buffer size, and sets ERROR_INSUFFICIENT_BUFFER — so the old form
        // returned a path that was a valid-looking prefix of the truth. Everything derived from it
        // (the folder the project is discovered in, the folder the engine root is walked up from)
        // would then point at a directory that exists and is the wrong one, silently. A downloaded
        // build unzipped into a deep folder is exactly where that happens.
        std::vector<wchar_t> buf( MAX_PATH );
        for ( ;; )
        {
            ::SetLastError( ERROR_SUCCESS );
            const DWORD n = ::GetModuleFileNameW( nullptr, buf.data(), static_cast<DWORD>( buf.size() ) );
            if ( n == 0 )
                return {}; // the call itself failed; an empty path is the caller's "I do not know"
            if ( ::GetLastError() != ERROR_INSUFFICIENT_BUFFER && n < buf.size() )
                return fs::path( std::wstring( buf.data(), n ) );
            if ( buf.size() >= 32768 ) // the NT path limit: beyond this there is nothing left to try
                return {};
            buf.resize( buf.size() * 2 );
        }
#elif defined( DESERT_PLATFORM_MACOS )
        uint32_t size = 0;
        _NSGetExecutablePath( nullptr, &size ); // first call: query required buffer size
        std::string buf( size, '\0' );
        if ( _NSGetExecutablePath( buf.data(), &size ) != 0 )
            return {};
        std::error_code ec;
        const fs::path  p     = fs::path( buf.c_str() );
        const fs::path  canon = fs::weakly_canonical( p, ec ); // resolve symlinks / '..'
        return ec ? p : canon;
#else // Linux and other POSIX
        std::error_code ec;
        const fs::path  p = fs::read_symlink( "/proc/self/exe", ec );
        return ec ? fs::path{} : p;
#endif
    }

    std::string FileSystem::GetFileDirectoryString( const std::filesystem::path& filepath )
    {
        return filepath.parent_path().string();
    }

    // WHY A FAILED READ IS NOT ALWAYS A MISSING FILE, and why saying so matters. Both read primitives
    // reach the VFS through an `optional`, which collapses two different events into one empty value:
    // the archive does not hold this key at all, and the archive holds it but the entry would not come
    // back. The second became reachable when PakReader started verifying each entry's content hash —
    // and the log then printed two adjacent lines that contradicted each other, "entry is CORRUPT"
    // followed by "not in a mounted pak". A middle link restating a specific failure as a generic one
    // is worse than no message: it sends whoever reads it looking for the wrong thing.
    //
    // Contains() and Read() are the two sides that must agree, so ask Contains() before concluding
    // absence.
    static std::string MissReason( const std::filesystem::path& filepath )
    {
        if ( VFS::Exists( filepath ) )
            return "present in a mounted archive but unreadable — see the [Pak] error above for which "
                   "entry and why";
        return "not on disk, not in a mounted pak";
    }

    Common::ResultStr<std::string> FileSystem::ReadFileContent( const std::filesystem::path& filepath )
    {
        std::ifstream in( filepath, std::ios::in | std::ios::binary );
        if ( !in )
        {
            // Not on disk: a packaged game serves content from the mounted .dpak (disk first so loose
            // files can still override archive entries while debugging a package).
            if ( auto packed = VFS::ReadFile( filepath ) )
                return Common::MakeSuccess( std::move( *packed ) );

            // Soft by contract (see the header): the caller owns the policy for a missing file, and
            // the error VALUE is what forces the caller to have one. This used to DESERT_VERIFY,
            // i.e. abort in every configuration — which made every "file is empty or missing" branch
            // in the loaders dead code and turned one missing asset into a crash of a packaged game.
            const std::string reason = MissReason( filepath );
            LOG_ERROR( "[FileSystem] Could not read file ({}): {}", reason, filepath.string() );
            return Common::MakeFormattedError<std::string>( "Could not read file ({}): {}", reason,
                                                            filepath.string() );
        }

        std::string fileContent;

        in.seekg( 0, std::ios::end );
        fileContent.resize( in.tellg() );
        in.seekg( 0, std::ios::beg );

        // THE READ IS CHECKED, exactly as the byte primitive below checks its own. Opening a path is
        // not the same as being able to read it: a directory opens and then fails on the first read,
        // and so do a racing delete, a truncated pak and an I/O error on a network volume. This
        // result used to be DISCARDED, which handed the caller a SUCCESS holding resize()'s zero
        // fill — 64 NUL bytes presented as the file's contents, with no error and no log. That is
        // the silent substitution §1.4 forbids, sitting in the primitive whose whole job is to make
        // a failed read impossible to ignore.
        if ( !fileContent.empty() && !in.read( &fileContent[0], fileContent.size() ) )
        {
            LOG_ERROR( "[FileSystem] Could not read {} bytes of file: {}", fileContent.size(), filepath.string() );
            return Common::MakeFormattedError<std::string>( "Could not read {} bytes of file: {}",
                                                            fileContent.size(), filepath.string() );
        }
        in.close();

        // A zero-byte file lands here as a SUCCESS holding "" — distinct from the miss above. It
        // skips the read entirely: there is nothing to extract, and asking for zero characters is a
        // question whose answer would only be noise.
        return Common::MakeSuccess( std::move( fileContent ) );
    }

    Common::ResultStr<std::string> FileSystem::ReadFileContentPrefix( const std::filesystem::path& filepath,
                                                                      const std::size_t            maxBytes )
    {
        std::ifstream in( filepath, std::ios::in | std::ios::binary );
        if ( !in )
        {
            // Not on disk: fall through to the archive, which has no ranged read — see the header for
            // why that is stated rather than worked around.
            if ( auto packed = VFS::ReadFile( filepath ) )
            {
                if ( packed->size() > maxBytes )
                    packed->resize( maxBytes );
                return Common::MakeSuccess( std::move( *packed ) );
            }

            const std::string reason = MissReason( filepath );
            LOG_ERROR( "[FileSystem] Could not read file ({}): {}", reason, filepath.string() );
            return Common::MakeFormattedError<std::string>( "Could not read file ({}): {}", reason,
                                                            filepath.string() );
        }

        std::string prefix;
        prefix.resize( maxBytes );

        // `read` sets failbit when it stops short of the request, which is the NORMAL outcome for a
        // file smaller than the window — so the outcome is judged by `gcount()`, and failbit alone is
        // not treated as an error. `bad()` still is: that is an actual I/O fault rather than a short
        // file, and the difference is why this is not a copy of ReadFileContent's check.
        if ( maxBytes > 0 )
        {
            in.read( &prefix[0], static_cast<std::streamsize>( maxBytes ) );
            if ( in.bad() )
            {
                LOG_ERROR( "[FileSystem] Could not read the first {} bytes of file: {}", maxBytes,
                           filepath.string() );
                return Common::MakeFormattedError<std::string>( "Could not read the first {} bytes of file: {}",
                                                                maxBytes, filepath.string() );
            }
            prefix.resize( static_cast<std::size_t>( in.gcount() ) );
        }
        in.close();

        return Common::MakeSuccess( std::move( prefix ) );
    }

    Common::ResultStr<std::vector<uint8_t>>
    FileSystem::ReadByteFileContent( const std::filesystem::path& filepath )
    {
        std::ifstream file( filepath, std::ios::in | std::ios::binary );
        if ( !file )
        {
            if ( auto packed = VFS::ReadFile( filepath ) )
                return Common::MakeSuccess( std::vector<uint8_t>( packed->begin(), packed->end() ) );

            // Soft by contract (see the header) — same reasoning as ReadFileContent above.
            const std::string reason = MissReason( filepath );
            LOG_ERROR( "[FileSystem] Could not open file ({}): {}", reason, filepath.string() );
            return Common::MakeFormattedError<std::vector<uint8_t>>( "Could not open file ({}): {}", reason,
                                                                     filepath.string() );
        }

        file.seekg( 0, std::ios::end );
        std::streamsize fileSize = file.tellg();
        file.seekg( 0, std::ios::beg );

        std::vector<uint8_t> binaryData( fileSize / sizeof( uint8_t ) );
        if ( !file.read( reinterpret_cast<char*>( binaryData.data() ), fileSize ) )
        {
            LOG_ERROR( "[FileSystem] Could not read {} bytes of file: {}", fileSize, filepath.string() );
            return Common::MakeFormattedError<std::vector<uint8_t>>( "Could not read {} bytes of file: {}",
                                                                     fileSize, filepath.string() );
        }
        return Common::MakeSuccess( std::move( binaryData ) );
    }

    std::vector<std::filesystem::path> FileSystem::ListFilesRecursive( const std::filesystem::path& root )
    {
        std::vector<fs::path> result;

        // TIMED AND COUNTED, and this is the ONE place it can be. This function is the engine's only
        // content-enumeration primitive, so a counter here sees every directory walk there is — which
        // is what makes "the cooked registry removed the walk" an observation rather than an argument
        // from the source. See Common/Utilities/ContentScanLedger.hpp.
        const auto walkStart = std::chrono::steady_clock::now();

        // Dedup key = absolute, symlink-resolved path — the same canonical spelling the VFS resolves
        // against — so a relative disk spelling and the pak's absolute one collapse into ONE
        // candidate, and the loose file (pushed first) is the spelling that survives. weakly_canonical
        // and not lexically_normal alone, because the disk walk yields the root as SPELLED while
        // VFS::ListFiles yields the mount root as RESOLVED, and under a symlinked prefix (macOS
        // /var -> /private/var) those are two spellings of one file.
        std::unordered_set<std::string> seen;
        std::error_code                 ec;
        const fs::path                  cwd  = fs::current_path( ec );
        auto                            push = [&]( const fs::path& p )
        {
            const fs::path  raw = ( p.is_absolute() ? p : cwd / p ).lexically_normal();
            std::error_code canonEc;
            fs::path        abs = fs::weakly_canonical( raw, canonEc );
            if ( canonEc || abs.empty() )
                abs = raw;
            if ( seen.insert( abs.generic_string() ).second )
                result.push_back( p );
        };

        if ( fs::exists( root, ec ) ) // a missing root is a valid state (clean project, packaged game)
        {
            for ( auto it = fs::recursive_directory_iterator( root, ec ); it != fs::recursive_directory_iterator();
                  it.increment( ec ) )
            {
                if ( ec )
                    break;
                if ( it->is_regular_file( ec ) )
                    push( it->path() );
            }
        }
        for ( const auto& packed : VFS::ListFiles( root ) )
            push( packed );

        ContentScanLedger::NoteWalk(
             result.size(),
             std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - walkStart ).count() );

        return result;
    }

    const std::filesystem::path FileSystem::GetParentPath( const std::filesystem::path& filepath )
    {
        return filepath.parent_path();
    }

    const std::string FileSystem::GetFileNameWithoutExtension( const std::filesystem::path& filepath )
    {
        return GetFileNameWithoutExtension_PATH( filepath ).string();
    }

    const std::filesystem::path
    FileSystem::GetFileNameWithoutExtension_PATH( const std::filesystem::path& filepath )
    {
        return filepath.stem();
    }

    const std::string FileSystem::GetFileExtension( const std::filesystem::path& filepath )
    {
        return filepath.extension().string();
    }

    uint32_t FileSystem::GetFileSize( const std::filesystem::path& filepath )
    {
        std::error_code ec;
        if ( fs::exists( filepath, ec ) )
            return (uint32_t)fs::file_size( filepath, ec );
        if ( auto packed = VFS::FileSize( filepath ) )
            return (uint32_t)*packed;
        return 0;
    }

    Common::BoolResultStr FileSystem::WriteBytesToFileAtomic( const std::filesystem::path& filepath,
                                                              std::span<const std::byte>   content )
    {
        // Contract and the reasoning behind every step are in the header. In one line: the original
        // file must survive a failure at ANY point, so nothing here ever opens the original for write.
        std::filesystem::path temp = filepath;
        temp += ".tmp";

        std::ofstream out( temp, std::ios::binary | std::ios::trunc );
        if ( !out )
        {
            LOG_ERROR( "[FileSystem] Atomic write failed: could not open temporary {} (original untouched)",
                       temp.string() );
            return Common::MakeFormattedError( "could not open the temporary file {} (the original is unchanged)",
                                               temp.string() );
        }

        out.write( reinterpret_cast<const char*>( content.data() ),
                   static_cast<std::streamsize>( content.size() ) );
        // close() explicitly, BEFORE the verdict: it flushes, and a buffered failure (disk full, the
        // volume going away) may only surface here. The destructor would swallow exactly that.
        out.close();
        if ( !out )
        {
            LOG_ERROR( "[FileSystem] Atomic write failed: writing {} bytes to {} (original untouched)",
                       content.size(), temp.string() );
            std::error_code removeEc;
            fs::remove( temp, removeEc );
            return Common::MakeFormattedError( "could not write {} bytes to {} (the original is unchanged)",
                                               content.size(), temp.string() );
        }

        std::error_code renameEc;
        fs::rename( temp, filepath, renameEc ); // POSIX rename(2) / MoveFileExW: replaces atomically
        if ( renameEc )
        {
            LOG_ERROR( "[FileSystem] Atomic write failed: renaming {} over {}: {} (original untouched)",
                       temp.string(), filepath.string(), renameEc.message() );
            std::error_code removeEc;
            fs::remove( temp, removeEc );
            return Common::MakeFormattedError( "could not rename {} over {}: {} (the original is unchanged)",
                                               temp.string(), filepath.string(), renameEc.message() );
        }
        return BOOLSUCCESS;
    }

    Common::BoolResultStr FileSystem::WriteContentToFileAtomic( const std::filesystem::path& filepath,
                                                                const std::string&           content )
    {
        // One body, two spellings — see the header. `as_bytes` over a string_view rather than a copy:
        // the bytes are the caller's for the duration of the call and nothing here retains them.
        return WriteBytesToFileAtomic( filepath, std::as_bytes( std::span( content.data(), content.size() ) ) );
    }

} // namespace Common::Utils