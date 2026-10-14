// VFX-03k: the fixed particle record is gone. Every emitter runs its compiled stack (SoA Floats / Ints of the
// pool, VFX/Simulate/<key>); the AoS `struct Particle` (Common/ParticleState.glslh), its simulation
// (Programs/Particles/ParticleSimulate.shader), the "Particles" storage binding and the RECORD branches of the
// sprite vertex path and the sort were deleted. Red when any shader under Editor/Resources/Shaders, or the C++ of
// ParticleWorldGpu / ParticleRenderer / ParticleSimGraph / ParticleSortGraph / ParticleSpriteBindings, names the
// record again, or when either deleted file is back. The census also fails when it scans nothing (a moved shader
// root would otherwise make it pass vacuously).
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <vector>

namespace
{
    std::filesystem::path RepoRoot()
    {
        return std::filesystem::path( __FILE__ )
             .parent_path()
             .parent_path()
             .parent_path()
             .parent_path()
             .parent_path();
    }

    std::string ReadText( const std::filesystem::path& path )
    {
        std::ifstream file( path, std::ios::binary );
        return std::string( ( std::istreambuf_iterator<char>( file ) ), std::istreambuf_iterator<char>() );
    }

    // What naming the fixed record looks like, in GLSL / DSL and in the C++ that binds it.
    const std::vector<std::string>& RecordWords()
    {
        static const std::vector<std::string> words = { "ParticleState",
                                                        "u_Particles",
                                                        "PARTICLE_SPRITE_SOURCE_RECORD",
                                                        "kParticleSpriteSourceRecord",
                                                        "ParticleSimPush",
                                                        "ParticleSimulate.shader",
                                                        "kParticleStride",
                                                        "kParticleChannelSpawnStride",
                                                        "\"Particles\"" };
        return words;
    }

    // `struct Particle` (not ParticleDrawSlot / ParticleSpriteSource) and a storage block named Particles.
    const std::regex& RecordPattern()
    {
        static const std::regex pattern(
             R"((struct\s+Particle\s*[\{\r\n])|((buffer|Buffer\s*\(\s*\d+\s*\))\s+Particles\b))" );
        return pattern;
    }

    void ExpectNoRecord( const std::filesystem::path& file, const std::string& text )
    {
        for ( const std::string& word : RecordWords() )
            EXPECT_EQ( text.find( word ), std::string::npos )
                 << file.string() << " names the fixed record: " << word;
        EXPECT_FALSE( std::regex_search( text, RecordPattern() ) )
             << file.string() << " declares the fixed Particle record or its Particles storage block";
    }
} // namespace

TEST( VFXParticleRecordCensus, NoShaderNamesTheFixedParticleRecord )
{
    const std::filesystem::path shaders = RepoRoot() / "Editor/Resources/Shaders";
    ASSERT_TRUE( std::filesystem::is_directory( shaders ) ) << shaders.string();
    EXPECT_FALSE( std::filesystem::exists( shaders / "Common/ParticleState.glslh" ) );
    EXPECT_FALSE( std::filesystem::exists( shaders / "Programs/Particles/ParticleSimulate.shader" ) );

    size_t scanned   = 0;
    bool   sawSprite = false;
    for ( const auto& entry : std::filesystem::recursive_directory_iterator( shaders ) )
    {
        if ( !entry.is_regular_file() )
            continue;
        const std::string ext = entry.path().extension().string();
        if ( ext != ".shader" && ext != ".glslh" && ext != ".glsl" && ext != ".dshader" )
            continue;
        ++scanned;
        sawSprite = sawSprite || entry.path().filename() == "Vertex_ParticleSprite.glslh";
        ExpectNoRecord( entry.path(), ReadText( entry.path() ) );
    }
    EXPECT_GT( scanned, 20u ) << "the census scanned almost nothing under " << shaders.string();
    EXPECT_TRUE( sawSprite ) << "the census did not reach the sprite vertex path";
}

TEST( VFXParticleRecordCensus, NoParticleRendererSourceNamesTheFixedParticleRecord )
{
    const std::filesystem::path dir = RepoRoot() / "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Particles";
    for ( const char* name : { "ParticleWorldGpu.hpp", "ParticleWorldGpu.cpp", "ParticleRenderer.hpp",
                               "ParticleRenderer.cpp", "ParticleSimGraph.hpp", "ParticleSortGraph.hpp",
                               "ParticleSpriteBindings.hpp", "ParticleGpuLayout.hpp" } )
    {
        const std::filesystem::path file = dir / name;
        const std::string           text = ReadText( file );
        ASSERT_FALSE( text.empty() ) << file.string() << " is missing or empty";
        ExpectNoRecord( file, text );
    }
}
