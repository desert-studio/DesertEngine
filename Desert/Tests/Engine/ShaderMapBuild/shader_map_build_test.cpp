// SHC1: the engine's programs are built on the job system at a cold start. What this suite holds is the
// RELATION between that and the one-at-a-time build the engine did before: for the same requests, the same
// maps — metadata and every SPIR-V word — at the same index. Each side runs against its own empty derived
// data cache, so neither reads what the other compiled.

#include <gtest/gtest.h>

#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Engine/Core/ShaderCompiler/ShaderCompiler.hpp>
#include <Engine/Core/ShaderCompiler/ShaderMapBuild.hpp>
#include <Engine/Core/ShaderCompiler/ShaderSpirvCache.hpp>

#include <Common/Core/JobSystem.hpp>

#include <TestSupport/derived_data_sandbox.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    using Desert::Core::ShaderMapOutcome;
    using Desert::Core::ShaderMapRequest;

    struct ShaderMapBuildFixture : ::testing::Test
    {
        static void SetUpTestSuite()
        {
            // Includes resolve against "Resources/Shaders/", relative: run from Editor/ as the editor does.
            std::filesystem::path here = std::filesystem::current_path();
            for ( int up = 0; up < 8 && !std::filesystem::exists( here / "Editor" / "Resources" / "Shaders" );
                  ++up )
                here = here.parent_path();
            ASSERT_TRUE( std::filesystem::exists( here / "Editor" / "Resources" / "Shaders" ) )
                 << "could not find Editor/Resources/Shaders above " << std::filesystem::current_path();
            std::filesystem::current_path( here / "Editor" );
        }
    };

    std::string ReadFile( const std::filesystem::path& path )
    {
        const std::ifstream in( path, std::ios::binary );
        std::ostringstream  out;
        out << in.rdbuf();
        return out.str();
    }

    // Real engine programs, several directories' worth so the workers genuinely overlap: a list of two would
    // pass with any ordering bug. Media are left out as BootContent leaves them out — they have no stages.
    std::vector<ShaderMapRequest> DefaultPassRequests()
    {
        std::vector<std::filesystem::path> files;
        for ( const char* dir : { "FXAA", "Bloom", "Unlit", "SMAA", "Composite", "UI", "Text", "JFA" } )
        {
            const auto root = std::filesystem::path( "Resources/Shaders/Programs" ) / dir;
            if ( !std::filesystem::exists( root ) )
                continue;
            for ( const auto& entry : std::filesystem::recursive_directory_iterator( root ) )
                if ( entry.is_regular_file() && entry.path().extension() == ".shader" )
                    files.push_back( entry.path() );
        }
        std::sort( files.begin(), files.end() );

        std::vector<ShaderMapRequest> requests;
        for ( const auto& file : files )
        {
            std::string source = ReadFile( file );
            if ( Desert::Core::Preprocess::DShaderParser::MayDeclareMedium( source ) )
                continue;
            requests.push_back( { std::move( source ), file, {}, {}, file.stem().string() } );
        }

        // No shipped shader may declare a named pass (ShippedShaderPasses holds that), so the second round
        // — BootContent's per-pass build — has nothing to chew on in the engine's own folders. These
        // programs give it several passes each, enough for the workers to overlap on that round too.
        for ( int n = 0; n < 4; ++n )
        {
            const std::string name   = std::format( "MultiPass{}", n );
            std::string       source = std::format( R"(Shader "{0}"
{{
    Vertex   {{ void main() {{ gl_Position = vec4( {1}.0 ); }} }}
    Fragment {{ layout( location = 0 ) out vec4 c; void main() {{ c = vec4( {1}.0 ); }} }}

    Pass "Shadow"
    {{
        Vertex   {{ void main() {{ gl_Position = vec4( {1}.5 ); }} }}
        Fragment {{ layout( location = 0 ) out vec4 c; void main() {{ c = vec4( 0.25 ); }} }}
    }}

    Pass "Outline"
    {{
        Vertex   {{ void main() {{ gl_Position = vec4( 2.0 ); }} }}
        Fragment {{ layout( location = 0 ) out vec4 c; void main() {{ c = vec4( {1}.0, 1.0, 0.0, 1.0 ); }} }}
    }}
}}
)",
                                                    name, n );
            const auto        path =
                 std::filesystem::path( "Resources/Shaders/Programs/Test" ) / std::format( "{}.shader", name );
            requests.push_back( { std::move( source ), path, {}, {}, name } );
        }
        return requests;
    }

    // The second round, as BootContent builds it: every named pass of every default program.
    std::vector<ShaderMapRequest> PassRequests( const std::vector<ShaderMapRequest>& defaults,
                                                const std::vector<ShaderMapOutcome>& built )
    {
        std::vector<ShaderMapRequest> requests;
        for ( size_t i = 0; i < defaults.size(); ++i )
            for ( const auto& pass : built[i].Map.Meta.PassNames )
                requests.push_back( { defaults[i].Source,
                                      defaults[i].Path,
                                      pass,
                                      {},
                                      std::format( "{}/{}", defaults[i].Name, pass ) } );
        return requests;
    }

    std::vector<ShaderMapOutcome> BuildOneAtATime( const std::vector<ShaderMapRequest>& requests )
    {
        std::vector<ShaderMapOutcome> outcomes;
        for ( const auto& request : requests )
        {
            auto built = Desert::Core::BuildShaderMap( request );
            outcomes.push_back( built.IsSuccess() ? ShaderMapOutcome{ built.GetValue(), {} }
                                                  : ShaderMapOutcome{ {}, built.GetError() } );
        }
        return outcomes;
    }

    void ExpectSameAnswers( const std::vector<ShaderMapRequest>& requests,
                            const std::vector<ShaderMapOutcome>& parallel,
                            const std::vector<ShaderMapOutcome>& serial )
    {
        ASSERT_EQ( parallel.size(), requests.size() );
        ASSERT_EQ( serial.size(), requests.size() );
        for ( size_t i = 0; i < requests.size(); ++i )
        {
            SCOPED_TRACE( requests[i].Name );
            EXPECT_EQ( parallel[i].Error, serial[i].Error );
            EXPECT_TRUE( parallel[i].Error.empty() ) << parallel[i].Error;
            EXPECT_FALSE( parallel[i].Map.Stages.empty() )
                 << "a program with no stages compares equal to anything";
            EXPECT_TRUE( parallel[i].Map == serial[i].Map ) << "the map at index " << i << " differs";
        }
    }
} // namespace

TEST_F( ShaderMapBuildFixture, TheJobSystemBuildGivesTheOneAtATimeAnswerAtTheSameIndex )
{
    ASSERT_GT( ::Common::JobSystem::Get().WorkerCount(), 0u ) << "no workers: nothing here runs in parallel";

    const auto defaults = DefaultPassRequests();
    ASSERT_GE( defaults.size(), 8u ) << "too few programs found for the workers to overlap";

    std::vector<ShaderMapOutcome> parallelDefaults;
    std::vector<ShaderMapOutcome> parallelPasses;
    std::vector<ShaderMapRequest> passes;
    {
        const Desert::TestSupport::DerivedDataSandbox cache( "ShaderMapBuildParallel" );
        parallelDefaults = Desert::Core::BuildShaderMaps( defaults );
        passes           = PassRequests( defaults, parallelDefaults );
        parallelPasses   = Desert::Core::BuildShaderMaps( passes );
    }
    std::vector<ShaderMapOutcome> serialDefaults;
    std::vector<ShaderMapOutcome> serialPasses;
    {
        const Desert::TestSupport::DerivedDataSandbox cache( "ShaderMapBuildSerial" );
        serialDefaults = BuildOneAtATime( defaults );
        serialPasses   = BuildOneAtATime( PassRequests( defaults, serialDefaults ) );
    }

    ExpectSameAnswers( defaults, parallelDefaults, serialDefaults );
    EXPECT_FALSE( passes.empty() ) << "no program declared a named pass: the second round went untested";
    ExpectSameAnswers( passes, parallelPasses, serialPasses );
}

// The warm half of the same function: a second build of one request reads the map the first one stored, and
// what it reads is what was compiled — the cold start's workers leave behind exactly what the main thread
// then loads.
TEST_F( ShaderMapBuildFixture, AStoredMapReadsBackAsTheMapThatWasBuilt )
{
    const auto defaults = DefaultPassRequests();
    ASSERT_FALSE( defaults.empty() );

    const Desert::TestSupport::DerivedDataSandbox cache( "ShaderMapBuildWarm" );
    const auto                                    cold = Desert::Core::BuildShaderMaps( defaults );
    const auto                                    warm = BuildOneAtATime( defaults );
    ExpectSameAnswers( defaults, cold, warm );
}

// ONE ARTIFACT, ONE WRITER (SHC1d). Twins — the same text in several files — share one map key (the map key does
// not name the file), so a job-system build that let every twin build would have several workers writing that
// key's one <file>.tmp. The first twin builds; every twin gets its answer, at its own index.
TEST_F( ShaderMapBuildFixture, TwinProgramsAreBuiltOnceAndAnsweredAtEveryIndex )
{
    const std::string             source = R"(Shader "Twin"
{
    Vertex   { void main() { gl_Position = vec4( 7.0 ); } }
    Fragment { layout( location = 0 ) out vec4 c; void main() { c = vec4( 0.5, 7.0, 0.0, 1.0 ); } }
}
)";
    std::vector<ShaderMapRequest> twins;
    for ( int n = 0; n < 8; ++n )
    {
        const std::string name = std::format( "Twin{}", n );
        twins.push_back(
             { source,
               std::filesystem::path( "Resources/Shaders/Programs/Test" ) / std::format( "{}.shader", name ),
               {},
               {},
               name } );
    }

    const Desert::TestSupport::DerivedDataSandbox cache( "ShaderMapBuildTwins" );
    const auto                                    before   = Desert::Core::ReadShaderCacheCounts();
    const auto                                    outcomes = Desert::Core::BuildShaderMaps( twins );
    const auto                                    after    = Desert::Core::ReadShaderCacheCounts();

    EXPECT_EQ( after.MapMisses - before.MapMisses, 1u ) << "each twin built its own map: one key, several writers";
    EXPECT_EQ( after.StoreFailures - before.StoreFailures, 0u );
    ASSERT_EQ( outcomes.size(), twins.size() );
    for ( size_t i = 0; i < twins.size(); ++i )
    {
        SCOPED_TRACE( twins[i].Name );
        EXPECT_TRUE( outcomes[i].Error.empty() ) << outcomes[i].Error;
        EXPECT_FALSE( outcomes[i].Map.Stages.empty() );
        EXPECT_TRUE( outcomes[i].Map == outcomes[0].Map );
    }
}

// The same below the map, under the profile without debug info (a Release build's): one stage text in eight files
// is one SPIR-V key whatever the programs around it, so compiling it on eight workers at once is ONE compile and
// one store; the other seven wait for that answer instead of writing the same <file>.tmp.
TEST_F( ShaderMapBuildFixture, OneStageTextInEightFilesIsOneCompileWithoutDebugInfo )
{
    const std::string source = "#version 450\nvoid main() { gl_Position = vec4( 11.0 ); }\n";
    constexpr size_t  kFiles = 8;

    const Desert::TestSupport::DerivedDataSandbox cache( "ShaderMapBuildStageTwins" );
    std::vector<std::vector<uint32_t>>            binaries( kFiles );
    std::vector<std::string>                      errors( kFiles );
    const auto                                    before = Desert::Core::ReadShaderCacheCounts();
    ::Common::JobSystem::Get().ParallelFor(
         kFiles,
         [&]( const size_t i )
         {
             auto compiled = Desert::Core::ShaderCompiler::CompileGLSLToSPIRVForProfile(
                  Desert::Core::Formats::ShaderStage::Vertex, source,
                  std::format( "Resources/Shaders/Programs/Test/StageTwin{}.shader", i ), false );
             if ( compiled.IsSuccess() )
                 binaries[i] = compiled.GetValue();
             else
                 errors[i] = compiled.GetError();
         } );
    const auto after = Desert::Core::ReadShaderCacheCounts();

    EXPECT_EQ( after.Compiled - before.Compiled, 1u ) << "one key compiled by several workers at once";
    EXPECT_EQ( after.StoreFailures - before.StoreFailures, 0u );
    for ( size_t i = 0; i < kFiles; ++i )
    {
        EXPECT_TRUE( errors[i].empty() ) << errors[i];
        EXPECT_FALSE( binaries[i].empty() );
        EXPECT_EQ( binaries[i], binaries[0] ) << "file " << i;
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
