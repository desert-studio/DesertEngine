#include <Engine/Scripting/Luau/LuauScriptSource.hpp>

#include <Common/Utilities/FileSystem.hpp>
#include <Common/Core/NumberText.hpp>
#include <Engine/Scripting/Luau/LuauRuntime.hpp>
#include <Engine/Scripting/ScriptSchema.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>

// What a script DECLARES, read without the game: render-free, so the dedicated server (Server/CMakeLists.txt) compiles
// it beside the Luau runtime and reads script components' schemas through the same loader as the editor (NET-ARCH §7).
namespace Desert::Scripting
{
    namespace
    {
        /// The comment block a script opens with (`--` lines before the first line of code, `--!` directives and
        /// blank lines skipped), its markers stripped, one line per line.
        std::string LeadingComment( const std::string& source )
        {
            std::string text;
            std::size_t at = 0;
            while ( at < source.size() )
            {
                std::size_t end = source.find( '\n', at );
                if ( end == std::string::npos )
                    end = source.size();
                std::string_view line( source.data() + at, end - at );
                at = end + 1;
                while ( !line.empty() && ( line.front() == ' ' || line.front() == '\t' ) )
                    line.remove_prefix( 1 );
                while ( !line.empty() && ( line.back() == '\r' || line.back() == ' ' ) )
                    line.remove_suffix( 1 );
                if ( line.empty() )
                {
                    if ( !text.empty() )
                        break; // a blank line closes the block
                    continue;
                }
                if ( !line.starts_with( "--" ) )
                    break;
                if ( line.starts_with( "--!" ) )
                    continue;
                line.remove_prefix( 2 );
                if ( !line.empty() && line.front() == ' ' )
                    line.remove_prefix( 1 );
                if ( !text.empty() )
                    text += '\n';
                text += line;
            }
            return text;
        }
    } // namespace

    /// The script's text: a loose file on disk, else the packed archive's copy (shipped game).
    Common::ResultStr<std::string> ReadScriptSource( const std::string& path )
    {
        if ( std::filesystem::exists( path ) )
        {
            std::ifstream file( path, std::ios::binary );
            if ( !file )
                return Common::MakeError<std::string>( std::format( "script not readable: {}", path ) );
            return Common::MakeSuccess(
                 std::string( std::istreambuf_iterator<char>( file ), std::istreambuf_iterator<char>() ) );
        }
        if ( Common::Utils::FileSystem::Exists( path ) )
        {
            if ( auto packed = Common::Utils::FileSystem::ReadFileContent( path ); packed )
                return Common::MakeSuccess( packed.ExtractValue() );
        }
        return Common::MakeError<std::string>( std::format( "script not found: {}", path ) );
    }

    ScriptSchema ReadScriptSchema( const std::string& path )
    {
        ScriptSchema schema;
        const auto   fail = [&]( ScriptStatus status, std::string error )
        {
            schema.Status    = status;
            schema.ErrorLine = ScriptErrorLine( error );
            schema.Error     = std::move( error );
            return schema;
        };
        if ( !std::filesystem::exists( path ) && !Common::Utils::FileSystem::Exists( path ) )
            return fail( ScriptStatus::NotFound, std::format( "script not found: {}", path ) );
        Common::ResultStr<std::string> source = ReadScriptSource( path );
        if ( !source.IsSuccess() )
            return fail( ScriptStatus::Error, source.GetError() );
        schema.Description = LeadingComment( source.GetValue() );

        // A throwaway runtime with no engine modules: the top level runs, the declarations are read back.
        LuauRuntime                       runtime;
        Common::ResultStr<LuauSlot> const slot = runtime.Load( path, source.GetValue(), {} );
        if ( !slot.IsSuccess() )
            return fail( ScriptStatus::Error, slot.GetError() );
        schema.Properties = *runtime.Declarations( slot.GetValue() );

        Common::ResultStr<std::vector<std::string>> buttons =
             runtime.ReadStringList( slot.GetValue(), "CallInEditor" );
        if ( !buttons.IsSuccess() )
            return fail( ScriptStatus::Error, std::format( "{}: {}", path, buttons.GetError() ) );
        for ( const std::string& name : buttons.GetValue() )
        {
            if ( !runtime.Defines( slot.GetValue(), name.c_str() ) )
                return fail(
                     ScriptStatus::Error,
                     std::format( "{}: CallInEditor names '{}', which the script does not define as a function",
                                  path, name ) );
            schema.EditorFunctions.push_back( { name } );
        }
        return schema;
    }

    int ScriptErrorLine( const std::string& error )
    {
        // Luau prefixes a located error with `<chunk>:<line>:`; a chunk is a path, which may itself hold a colon
        // (`C:/...`), so the first colon FOLLOWED BY DIGITS AND A COLON is the one.
        for ( std::size_t colon = error.find( ':' ); colon != std::string::npos;
              colon             = error.find( ':', colon + 1 ) )
        {
            std::size_t end = colon + 1;
            while ( end < error.size() && error[end] >= '0' && error[end] <= '9' )
                ++end;
            if ( end > colon + 1 && end < error.size() && error[end] == ':' )
            {
                // Digits only, so a refusal is a number past int, which names no line: the scan goes on.
                const std::string_view digits = std::string_view( error ).substr( colon + 1, end - colon - 1 );
                if ( const std::optional<int> line = Common::WholeIntegerFromText<int>( digits ) )
                    return *line;
            }
        }
        return 0;
    }
} // namespace Desert::Scripting
