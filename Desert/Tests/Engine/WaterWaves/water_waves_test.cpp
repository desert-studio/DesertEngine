// The wave core's invariants (WATER-W1): the CPU waves are UE's, zero amplitude moves nothing, and the GLSL the
// water shader will compile is the very text the CPU evaluates.
//
// UE REFERENCE VALUES. UE cannot run here, so the numbers below come from a literal second transcription of the UE
// 5.8 sources (GerstnerWaterWaves.cpp:14-22, 115-139; GerstnerWaveEvaluation.cpp:32-135; FRandomStream) in Python,
// rounding to float32 where UE stores a float and keeping double where UE holds an FVector — independent of
// Shaders/Common/GerstnerWave.glslh, which is what is under test. The tolerances cover UE's double WaveVector
// against the shared header's float one: at |phase| ~ 100 rad a float ulp is ~1e-5 rad, times 80 cm amplitude.

#include "../../TestSupport/runner.hpp"
#include "../../TestSupport/scratch_dir.hpp"

#include <Engine/Core/ShaderCompiler/Includer/ShaderIncluder.hpp>
#include <Engine/Assets/Serialization/WaterWaves.hpp>
#include <Engine/Water/WaterWaves.hpp>

#include <Common/Content/ContentKinds.hpp>

#include <gtest/gtest.h>
#include <shaderc/shaderc.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace
{
    using namespace Desert::Water;

    constexpr float kHeightToleranceCm = 0.01f;

    struct ReferenceWave
    {
        float DirX, DirZ, WaveLength, Amplitude, Steepness;
    };
    struct ReferenceHeight
    {
        glm::vec2 Plane;
        float     Time;
        float     Height;
    };

    void ExpectWaves( const std::vector<GerstnerWave>& waves, const std::vector<ReferenceWave>& reference )
    {
        ASSERT_GE( waves.size(), reference.size() );
        for ( size_t i = 0; i < reference.size(); ++i )
        {
            const ReferenceWave& r = reference[i];
            EXPECT_NEAR( waves[i].Direction.x, r.DirX, 1e-5f ) << "wave " << i;
            EXPECT_NEAR( waves[i].Direction.y, r.DirZ, 1e-5f ) << "wave " << i;
            EXPECT_NEAR( waves[i].WaveLength, r.WaveLength, 1e-2f ) << "wave " << i;
            EXPECT_NEAR( waves[i].Amplitude, r.Amplitude, 1e-4f ) << "wave " << i;
            EXPECT_NEAR( waves[i].Steepness, r.Steepness, 1e-6f ) << "wave " << i;
        }
    }

    void ExpectHeights( const std::vector<GerstnerWave>& waves, const std::vector<ReferenceHeight>& reference )
    {
        for ( const ReferenceHeight& r : reference )
            EXPECT_NEAR( WaveHeightAt( waves, r.Plane, r.Time ).Height, r.Height, kHeightToleranceCm )
                 << "at (" << r.Plane.x << ", " << r.Plane.y << "), t = " << r.Time;
    }

    GerstnerWaveGenerator Gusty()
    {
        GerstnerWaveGenerator g;
        g.NumWaves                  = 8;
        g.Seed                      = 7;
        g.Randomness                = 0.5f;
        g.WindAngleDeg              = 30.0f;
        g.DirectionAngularSpreadDeg = 45.0f;
        return g;
    }
} // namespace

TEST( WaterWaves, TheDefaultGeneratorMakesUEsWaves )
{
    const auto waves = GenerateGerstnerWaves( GerstnerWaveGenerator{} );
    ASSERT_EQ( waves.size(), 16u );
    ExpectWaves( waves, { { 1.0000000f, 0.0000000f, 6000.0000f, 80.00000f, 0.200000f },
                          { -0.6648128f, 0.7470100f, 5336.5273f, 70.79688f, 0.212500f },
                          { -0.2657157f, 0.9640514f, 4715.8594f, 62.18750f, 0.225000f },
                          { -0.9485897f, -0.3165085f, 4137.9961f, 54.17188f, 0.237500f } } );
}

TEST( WaterWaves, ASeededRandomisedGeneratorMakesUEsWaves )
{
    const auto waves = GenerateGerstnerWaves( Gusty() );
    ASSERT_EQ( waves.size(), 8u );
    ExpectWaves( waves, { { 0.8660254f, 0.5000000f, 6000.0000f, 80.00000f, 0.200000f },
                          { 0.6342140f, 0.7731576f, 4773.1128f, 62.98167f, 0.225000f },
                          { 0.9997399f, 0.0228053f, 3338.2810f, 43.07891f, 0.250000f },
                          { 0.2960479f, 0.9551731f, 2362.9170f, 29.54950f, 0.275000f } } );
    EXPECT_EQ( waves, GenerateGerstnerWaves( Gusty() ) ) << "the same seed must give the same waves";
}

TEST( WaterWaves, TheHeightAtAPointIsUEs )
{
    ExpectHeights( GenerateGerstnerWaves( GerstnerWaveGenerator{} ),
                   { { { 0.0f, 0.0f }, 0.0f, 508.12500f },
                     { { 1234.5f, -987.25f }, 3.5f, -55.03937f },
                     { { -4000.0f, 2500.0f }, 12.25f, -198.52424f } } );
    ExpectHeights( GenerateGerstnerWaves( Gusty() ), { { { 0.0f, 0.0f }, 0.0f, 259.74839f },
                                                       { { 1234.5f, -987.25f }, 3.5f, -158.39438f },
                                                       { { -4000.0f, 2500.0f }, 12.25f, -102.80332f } } );
}

TEST( WaterWaves, ZeroAmplitudeDisplacesNothing )
{
    std::vector<GerstnerWave> waves = GenerateGerstnerWaves( Gusty() );
    for ( GerstnerWave& w : waves )
        w.Amplitude = 0.0f;
    for ( const glm::vec2 plane :
          { glm::vec2( 0.0f ), glm::vec2( 1234.5f, -987.25f ), glm::vec2( -4000.0f, 2500.0f ) } )
    {
        const glm::vec3 d = WaveDisplacement( waves, plane, 7.0f );
        EXPECT_EQ( d.x, 0.0f );
        EXPECT_EQ( d.y, 0.0f );
        EXPECT_EQ( d.z, 0.0f );
        const WaveHeightSample s = WaveHeightAt( waves, plane, 7.0f );
        EXPECT_EQ( s.Height, 0.0f );
        EXPECT_EQ( s.Normal, glm::vec3( 0.0f, 1.0f, 0.0f ) );
        EXPECT_EQ( WaveSimpleHeightAt( waves, plane, 7.0f ), 0.0f );
    }
    EXPECT_EQ( MaxWaveHeight( waves ), 0.0f );
}

// Steepness 0: no sideways motion, so the height at a point is A cos(phase) and the normal is the gradient's.
TEST( WaterWaves, ASwellIsACosineAndItsNormalIsTheGradients )
{
    const GerstnerWave     wave{ glm::normalize( glm::vec2( 0.6f, 0.8f ) ), 1000.0f, 50.0f, 0.0f };
    const std::vector      waves{ wave };
    const glm::vec2        plane( 321.0f, -77.0f );
    const float            t     = 2.0f;
    const float            k     = 2.0f * 3.14159265f / wave.WaveLength;
    const float            phase = glm::dot( plane, wave.Direction * k ) - std::sqrt( k * 980.0f ) * t;
    const WaveHeightSample s     = WaveHeightAt( waves, plane, t );
    EXPECT_NEAR( s.Height, wave.Amplitude * std::cos( phase ), 1e-3f );
    EXPECT_NEAR( s.Height, WaveSimpleHeightAt( waves, plane, t ), 1e-4f );
    const glm::vec3 expected =
         glm::normalize( glm::vec3( wave.Amplitude * k * std::sin( phase ) * wave.Direction.x, 1.0f,
                                    wave.Amplitude * k * std::sin( phase ) * wave.Direction.y ) );
    EXPECT_NEAR( glm::length( s.Normal - expected ), 0.0f, 1e-5f );
}

TEST( WaterWaves, DepthAttenuationIsUEs )
{
    EXPECT_EQ( WaveDepthAttenuation( 0.0f, 2000.0f ), 0.0f );
    EXPECT_EQ( WaveDepthAttenuation( -50.0f, 2000.0f ), 0.0f );
    EXPECT_NEAR( WaveDepthAttenuation( 2000.0f, 2000.0f ), 1.0f - std::exp( -2.0f ), 1e-6f );
    EXPECT_NEAR( WaveDepthAttenuation( 1e6f, 2000.0f ), 1.0f, 1e-6f );
}

// The header the CPU evaluates compiles as GLSL, every function called, warnings as errors.
TEST( WaterWaves, TheSharedHeaderCompilesAsGlsl )
{
    const auto host =
         Desert::TestSupport::RepositoryRoot() / "Editor/Resources/Shaders/Programs/WaterWavesHost.comp";
    const std::string       source = "#version 450\n"
                                     "#include \"../Common/GerstnerWave.glslh\"\n"
                                     "layout( local_size_x = 64 ) in;\n"
                                     "layout( std430, set = 0, binding = 0 ) buffer Out { vec4 O[]; };\n"
                                     "void main()\n"
                                     "{\n"
                                     "    vec2 p = vec2( float( gl_GlobalInvocationID.x ), 3.0 );\n"
                                     "    vec2 d = vec2( 1.0, 0.0 );\n"
                                     "    vec3 o = GerstnerWaveOffset( p, d, 500.0, 20.0, 0.3, 1.5 );\n"
                                     "    vec4 h = GerstnerWaveHeightAt( p, d, 500.0, 20.0, 0.3, 1.5 );\n"
                                     "    vec3 n = GerstnerFinalizeNormal( h.xyz );\n"
                                     "    float s = GerstnerWaveSimpleHeight( p, d, 500.0, 20.0, 1.5 );\n"
                                     "    float a = GerstnerDepthAttenuation( 300.0, 2000.0 );\n"
                                     "    O[gl_GlobalInvocationID.x] = vec4( o + n, h.w + s * a );\n"
                                     "}\n";
    const shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetIncluder( std::make_unique<Desert::Core::ShaderIncluder>( host ) );
    options.SetTargetEnvironment( shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1 );
    options.SetWarningsAsErrors();
    const auto result =
         compiler.CompileGlslToSpv( source, shaderc_compute_shader, host.string().c_str(), options );
    EXPECT_EQ( result.GetCompilationStatus(), shaderc_compilation_status_success ) << result.GetErrorMessage();
}

// THE ASSET: a `.dwaves` stores the generator and its seed; what it reads back generates the same waves.
TEST( WaterWaves, AWaveSetRoundTripsAndGeneratesTheSameWaves )
{
    namespace S = Desert::Assets::Serialization;
    S::WaterWavesData data;
    data.Generator.Seed       = 1234;
    data.Generator.Randomness = 0.5f;
    data.Generator.NumWaves   = 8;

    const auto parsed = S::ParseWaterWaves( S::WriteWaterWaves( data ) );
    ASSERT_TRUE( parsed ) << parsed.GetError();
    EXPECT_EQ( parsed.GetValue().Generator, data.Generator );
    EXPECT_EQ( Desert::Water::GenerateGerstnerWaves( parsed.GetValue().Generator ),
               Desert::Water::GenerateGerstnerWaves( data.Generator ) );
}

TEST( WaterWaves, AWaveSetTheGeneratorCannotHonourIsRefused )
{
    namespace S        = Desert::Assets::Serialization;
    const auto refused = []( auto edit )
    {
        S::WaterWavesData data;
        edit( data.Generator );
        return !S::ValidateWaterWavesData( data );
    };
    EXPECT_FALSE( refused( []( Desert::Water::GerstnerWaveGenerator& ) {} ) );
    EXPECT_TRUE( refused( []( auto& g ) { g.NumWaves = 0; } ) );
    EXPECT_TRUE( refused( []( auto& g ) { g.NumWaves = S::kWaterWavesMaxNumWaves + 1; } ) );
    EXPECT_TRUE( refused( []( auto& g ) { g.MinWavelength = g.MaxWavelength + 1.0f; } ) );
    EXPECT_TRUE( refused( []( auto& g ) { g.MinAmplitude = -1.0f; } ) );
    EXPECT_TRUE( refused( []( auto& g ) { g.SmallWaveSteepness = 1.5f; } ) );
    EXPECT_TRUE( refused( []( auto& g ) { g.Randomness = -0.1f; } ) );
    EXPECT_TRUE( refused( []( auto& g ) { g.WindAngleDeg = std::nanf( "" ); } ) );
}

// One ContentKinds row names `.dwaves`, under the kind the header states.
TEST( WaterWaves, OneContentKindsRowHasTheExtension )
{
    const auto kinds = Common::Content::ContentKinds();
    EXPECT_EQ(
         std::ranges::count_if( kinds, []( const auto& k )
                                { return k.Extension == Desert::Assets::Serialization::kWaterWavesExtension; } ),
         1 );
    EXPECT_EQ( Common::Content::KindSpec( Common::Content::ContentKind::WaterWaves ).Name, "WaterWaves" );
}
