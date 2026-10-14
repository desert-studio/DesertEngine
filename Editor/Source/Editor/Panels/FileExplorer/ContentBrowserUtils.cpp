#define NOMINMAX

#include "ContentBrowserUtils.hpp"

#include <Editor/Core/AssetFileOps.hpp>
#include <Editor/Core/Commands/AssetMoveCommand.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <system_error>

#ifdef DESERT_PLATFORM_WINDOWS
#include <windows.h>
#include <shellapi.h>
#endif

#if defined( DESERT_PLATFORM_MACOS )
#include <crt_externs.h>
#include <spawn.h>
#include <sys/wait.h>
#endif
#include <format>
#include <string>
#include <vector>

namespace Desert::Editor::ContentBrowserUtils
{
    bool MoveOrRename( const std::string& src, const std::filesystem::path& dst, const char* label,
                       std::string& error )
    {
        std::error_code ec;
        const bool      folder = std::filesystem::is_directory( src, ec );
        if ( folder || Assets::ContentRegistry::HasRow( src ) )
        {
            const auto moved =
                 folder ? MoveFolderWithUndo( src, dst, label ) : MoveAssetWithUndo( src, dst, label );
            if ( !moved )
                error = moved.GetError();
            return static_cast<bool>( moved );
        }
        std::string newPath;
        return dst.parent_path() == std::filesystem::path( src ).parent_path()
                    ? AssetFileOps::Rename( src, dst.filename().string(), newPath, error )
                    : AssetFileOps::Move( src, dst.parent_path().string(), newPath, error );
    }

    bool MoveFileTo( const std::string& filePath, const std::string& movePath )
    {
        std::string error;
        const bool  moved = MoveOrRename(
             filePath, std::filesystem::path( movePath ) / std::filesystem::path( filePath ).filename(), "Move",
             error );
        if ( !moved )
            LOG_ERROR( "[Assets] move '{}' -> '{}': {}", filePath, movePath, error );
        return moved;
    }

#if defined( DESERT_PLATFORM_MACOS )
    // `open` run directly rather than through a shell: no quoting to get wrong for a path that holds a
    // quote, and no std::system (which is not thread-safe). Waits, as the shell call did; `open` returns at once.
    namespace
    {
        bool SpawnOpen( std::vector<std::string> args )
        {
            std::vector<char*> argv;
            argv.reserve( args.size() + 1 );
            for ( std::string& a : args )
                argv.push_back( a.data() );
            argv.push_back( nullptr );
            pid_t pid = 0;
            if ( posix_spawnp( &pid, argv[0], nullptr, nullptr, argv.data(), *_NSGetEnviron() ) != 0 )
                return false;
            int status = 0;
            waitpid( pid, &status, 0 );
            return WIFEXITED( status ) && WEXITSTATUS( status ) == 0;
        }
    } // namespace
#endif

    void ShellOpenDefault( const std::string& path )
    {
#if defined( DESERT_PLATFORM_WINDOWS )
        std::error_code ec;
        const auto      abs = std::filesystem::absolute( path, ec ).make_preferred().wstring();
        ShellExecuteW( nullptr, L"open", abs.c_str(), nullptr, nullptr, SW_SHOWNORMAL );
#elif defined( DESERT_PLATFORM_MACOS )
        std::error_code   ec;
        const std::string abs = std::filesystem::absolute( path, ec ).string();
        SpawnOpen( { "open", abs } );
#else
        (void)path;
#endif
    }

    bool ShellOpenInCodeEditor( const std::string& path, int line )
    {
        // VS Code's URL (`vscode://file/<absolute path>:<line>`) opens the file AT THE LINE whether the editor is
        // running or not, with no PATH to depend on — an editor launched from the Finder or the Start menu does
        // not have VS Code's `code` CLI on its PATH. Without VS Code the OS's default editor opens the file.
        std::error_code             ec;
        const std::filesystem::path absolute = std::filesystem::absolute( path, ec );
#if defined( DESERT_PLATFORM_WINDOWS )
        // The path's own UTF-16 (std::filesystem::path holds it natively on Windows): a narrow string would go
        // through the ANSI code page and lose every character outside it.
        const std::wstring wide =
             std::format( L"vscode://file/{}:{}", absolute.generic_wstring(), std::max( line, 1 ) );
        if ( reinterpret_cast<INT_PTR>(
                  ShellExecuteW( nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL ) ) > 32 )
            return true;
#elif defined( DESERT_PLATFORM_MACOS )
        if ( SpawnOpen( { "open", std::format( "vscode://file/{}:{}", absolute.generic_string(), std::max( line, 1 ) ) } ) )
            return true;
#endif
        ShellOpenDefault( path );
        return false;
    }

    void ShellRevealInExplorer( const std::string& path )
    {
#if defined( DESERT_PLATFORM_WINDOWS )
        std::error_code    ec;
        const auto         abs    = std::filesystem::absolute( path, ec ).make_preferred().wstring();
        const std::wstring params = std::format( L"/select,\"{}\"", abs );
        ShellExecuteW( nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL );
#elif defined( DESERT_PLATFORM_MACOS )
        std::error_code   ec;
        const std::string abs = std::filesystem::absolute( path, ec ).string();
        SpawnOpen( { "open", "-R", abs } );
#else
        (void)path;
#endif
    }

    // The error is taken through the std::error_code overload rather than a catch: a failed status() is a
    // value this function has to read, so it cannot be swallowed by accident.
    bool IsHidden( const std::filesystem::path& filePath )
    {
        std::error_code                    ec;
        const std::filesystem::file_status status = std::filesystem::status( filePath, ec );
        if ( ec )
            return false;

        return ( status.permissions() & std::filesystem::perms::owner_read ) == std::filesystem::perms::none ||
               filePath.stem().string() == ".DS_Store";
    }

    std::string ToLowerCopy( std::string s )
    {
        std::transform( s.begin(), s.end(), s.begin(),
                        []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
        return s;
    }
} // namespace Desert::Editor::ContentBrowserUtils
