// DShaderTool — offline linter for Desert Shader Language (.shader) files.
//
//   DShaderTool <file|dir> [<file|dir> ...]
//
// Recursively collects *.shader under every argument, runs the ENGINE's DShaderParser over each and
// reports every parse error with its file. Exit code 0 = all parsed, 1 = at least one failure.
// Non-DSL files are lint FAILURES: the engine dropped the legacy #pragma-program format.
//
// The parser is the same translation unit the engine/editor uses (compiled in via premake) — this
// tool can never drift from what the runtime actually accepts.

#include <ToolMain.hpp>

#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    std::string ReadFile( const fs::path& path )
    {
        std::ifstream in( path, std::ios::in | std::ios::binary );
        if ( !in )
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    void Collect( const fs::path& root, std::vector<fs::path>& out )
    {
        std::error_code ec;
        if ( fs::is_regular_file( root, ec ) )
        {
            if ( root.extension() == ".shader" )
                out.push_back( root );
            return;
        }
        for ( auto it = fs::recursive_directory_iterator( root, ec );
              it != fs::recursive_directory_iterator(); it.increment( ec ) )
        {
            if ( !ec && it->is_regular_file() && it->path().extension() == ".shader" )
                out.push_back( it->path() );
        }
    }
} // namespace

static int RunTool( int argc, char** argv )
{
    if ( argc < 2 )
    {
        std::fprintf( stderr, "Usage: DShaderTool <file|dir> [<file|dir> ...]\n" );
        return 2;
    }

    std::vector<fs::path> files;
    for ( int i = 1; i < argc; ++i )
        Collect( argv[i], files );

    if ( files.empty() )
    {
        std::fprintf( stderr, "DShaderTool: no .shader files found under the given paths\n" );
        return 2;
    }

    int parsed = 0, failed = 0;
    for ( const auto& file : files )
    {
        const std::string source = ReadFile( file );
        if ( source.empty() )
        {
            std::printf( "FAIL  %s: cannot read file\n", file.string().c_str() );
            ++failed;
            continue;
        }

        using Desert::Core::Preprocess::DShaderParser;
        if ( !DShaderParser::IsDShader( source ) )
        {
            // The legacy #pragma-program format was removed from the engine — a non-DSL .shader
            // can never load, so it's a lint FAILURE now, not a skip.
            std::printf( "FAIL  %s: not a DSL shader (legacy #pragma format is no longer supported)\n",
                         file.string().c_str() );
            ++failed;
            continue;
        }

        const auto result = DShaderParser::Parse( source );
        if ( result.IsSuccess() )
        {
            ++parsed;
        }
        else
        {
            std::printf( "FAIL  %s: %s\n", file.string().c_str(), result.GetError().c_str() );
            ++failed;
        }
    }

    std::printf( "DShaderTool: %d parsed, %d failed — %zu file(s) total\n", parsed, failed,
                 files.size() );
    return failed == 0 ? 0 : 1;
}

// The entry point, one line. Anything this tool throws is named on stderr with the tool's own name
// instead of reaching std::terminate, which would print the exception's TYPE and nothing else — see
// Tools/Shared/ToolMain.hpp.
int main( int argc, char** argv )
{
    return Desert::Tools::RunMain( "DShaderTool", argc, argv, &RunTool );
}
