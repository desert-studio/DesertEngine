// ShaderProtocolGen --root <repo> --out-cpp <GeneratedShaderIncludes.gen.cpp> --out-dir <Generated/Shaders>
//                   --allow <TwinAllowList.txt>
// Writes every generated shader include (the engine's table and the same texts as files) from the C++ layout
// declarations, then refuses hand-written GLSL twins of them. Exit 1 on a refusal: the build stops.
#include "GlslEmit.hpp"
#include "IncludeLists.hpp"
#include "TwinCheck.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace
{
    using namespace Desert::ShaderProtocolGen;

    // Writes @p text only when the file's bytes differ: an unchanged include keeps its time stamp, so nothing that
    // reads it rebuilds.
    bool WriteIfChanged( const std::filesystem::path& file, const std::string_view text )
    {
        {
            std::ifstream in( file, std::ios::binary );
            if ( in )
            {
                const std::string old( ( std::istreambuf_iterator<char>( in ) ),
                                       std::istreambuf_iterator<char>() );
                if ( old == text )
                    return true;
            }
        }
        std::error_code error;
        std::filesystem::create_directories( file.parent_path(), error );
        std::ofstream out( file, std::ios::binary | std::ios::trunc );
        out.write( text.data(), static_cast<std::streamsize>( text.size() ) );
        if ( !out )
        {
            std::fprintf( stderr, "%s: cannot write\n", file.generic_string().c_str() );
            return false;
        }
        return true;
    }

    struct Arguments
    {
        std::filesystem::path Root;
        std::filesystem::path OutCpp;
        std::filesystem::path OutDir;
        std::filesystem::path Allow;
    };

    bool ParseArguments( const int argc, char** argv, Arguments& out )
    {
        for ( int i = 1; i + 1 < argc; i += 2 )
        {
            const std::string_view flag = argv[i];
            if ( flag == "--root" )
                out.Root = argv[i + 1];
            else if ( flag == "--out-cpp" )
                out.OutCpp = argv[i + 1];
            else if ( flag == "--out-dir" )
                out.OutDir = argv[i + 1];
            else if ( flag == "--allow" )
                out.Allow = argv[i + 1];
            else
                return false;
        }
        return !out.Root.empty() && !out.OutCpp.empty() && !out.OutDir.empty() && !out.Allow.empty();
    }
} // namespace

int main( const int argc, char** argv )
{
    Arguments arguments;
    if ( !ParseArguments( argc, argv, arguments ) )
    {
        std::fprintf( stderr,
                      "usage: ShaderProtocolGen --root <repo> --out-cpp <file> --out-dir <dir> --allow <file>\n" );
        return 1;
    }
    const std::filesystem::path shaderRoot = arguments.Root / "Editor/Resources/Shaders";

    std::vector<const ShaderIncludeDecl*> all;
    for ( const auto part : { SceneIncludes(), PostIncludes(), GeometryShadowIncludes(), LightingIncludes(),
                              VfxIncludes(), AtmosphereIncludes(), WaterIncludes() } )
        all.insert( all.end(), part.begin(), part.end() );

    const std::vector<std::string> refusals = CheckIncludes( all, SceneIncludes(), shaderRoot );
    for ( const std::string& refusal : refusals )
        std::fprintf( stderr, "%s\n", refusal.c_str() );
    if ( !refusals.empty() )
        return 1;

    std::vector<GeneratedInclude> generated;
    for ( const ShaderIncludeDecl* include : all )
    {
        std::string    text = EmitGlsl( *include );
        const uint64_t hash = Fnv1a64( text );
        generated.push_back( { include, std::move( text ), hash } );
    }
    const std::vector<GeneratedShape> shapes = CollectShapes( all );

    std::vector<TwinAllow>         allowList;
    const std::vector<std::string> allowErrors = ReadAllowList( arguments.Allow, allowList );
    for ( const std::string& error : allowErrors )
        std::fprintf( stderr, "%s\n", error.c_str() );
    if ( !allowErrors.empty() )
        return 1;
    const std::vector<TwinFinding> twins = FindTwins( shaderRoot, shapes, allowList );

    bool                            written = true;
    std::set<std::filesystem::path> current;
    for ( const GeneratedInclude& include : generated )
    {
        const std::filesystem::path file = ( arguments.OutDir / include.Decl->Path ).lexically_normal();
        current.insert( file );
        written = WriteIfChanged( file, include.Text ) && written;
    }
    // An include whose descriptor is gone leaves no file behind for an include path to find.
    std::error_code                    error;
    std::vector<std::filesystem::path> stale;
    for ( auto it = std::filesystem::recursive_directory_iterator( arguments.OutDir, error );
          !error && it != std::filesystem::recursive_directory_iterator(); it.increment( error ) )
        if ( it->is_regular_file() && it->path().extension() == ".glslh" &&
             !current.contains( it->path().lexically_normal() ) )
            stale.push_back( it->path() );
    for ( const std::filesystem::path& file : stale )
        std::filesystem::remove( file, error );

    written =
         WriteIfChanged( arguments.OutCpp, EmitCppTable( generated, GeneratedVertexInputNames() ) ) && written;
    written = WriteIfChanged( arguments.OutDir / "protocol_manifest.json",
                              EmitManifest( generated, shapes, twins, allowList ) ) &&
              written;

    for ( const TwinFinding& twin : twins )
        std::fprintf( stderr, "%s:%u: %s %s the generated %s (%s)\n",
                      ( shaderRoot / twin.Path ).generic_string().c_str(), twin.Line, twin.Name.c_str(),
                      twin.SameName ? "duplicates" : "repeats the members of", twin.GeneratedPath.c_str(),
                      twin.SourceHeader.c_str() );
    for ( const TwinAllow& allow : allowList )
        if ( !allow.Used )
            std::fprintf( stdout, "%s:%u: warning: %s:%s matches no generated layout; remove the line\n",
                          arguments.Allow.generic_string().c_str(), allow.Line, allow.Path.c_str(),
                          allow.Name.c_str() );
    return written && twins.empty() ? 0 : 1;
}
