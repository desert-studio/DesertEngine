// The direct-light term both render paths shade with, tested as the RELATION it has to be — not as a
// function that returns plausible numbers. The companion of Desert/Tests/Engine/AmbientIBL, and the
// same shape of defect one term over.
//
// What this suite exists for: `Programs/PBR/StaticMeshPBR.shader` computed the sun's diffuse as
// `kd * albedo` while `Programs/Deferred/DeferredLighting.shader` computed it as `kd * albedo / PI`.
// The Lambertian BRDF is albedo/PI, so the forward path's sun was PI times too bright. 51 of the
// repository's 51 scenes shade their ordinary static opaque geometry through the deferred path, and
// the two that do not (MAT_ProbeShadows, MAT_ProbeUnlitShadows) carry exactly the sun intensity of
// their deferred siblings — so nothing anywhere had been authored to compensate, and the disagreement
// was simply visible: Clouds_Showcase from 0,200,0 just below the horizon, clouds/fog/SSAO/GI/shadows
// off, rendered its ground at a mean of 109/255 through the deferred path and 161/255 through the
// forward one, with the sky byte-identical between them. After the fix: 109.11 vs 109.20.
//
// The same disagreement lived INSIDE one shader — PointLight.glslh and Spotlight.glslh already had the
// /PI — so the forward sun was also PI times brighter than a point light of equal intensity standing
// beside it. That is the relation asserted below, and it is why the deliverable is one shared text
// rather than a test that four copies still match.
//
// Every assertion is about the SHIPPED TEXT (Mesh/DirectLighting.glslh compiled as C++), and each picks
// a property a missing 1/PI cannot have.

#include <gtest/gtest.h>

#include "DirectLightingReference.hpp"

#include <glm/gtc/epsilon.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace Desert::Tests::DirectLightingRef;

namespace
{
    // A plausible surface: mid-grey dielectric, fairly rough, lit from 45 degrees off its normal.
    const glm::vec3 kN      = glm::vec3( 0.0f, 0.0f, 1.0f );
    const glm::vec3 kL      = glm::normalize( glm::vec3( 0.0f, 0.7071f, 0.7071f ) );
    const glm::vec3 kView   = glm::normalize( glm::vec3( 0.3f, 0.0f, 1.0f ) );
    const glm::vec3 kAlbedo = glm::vec3( 0.5f );
    const glm::vec3 kF0     = glm::vec3( 0.04f ); // Fdielectric, as every call site builds it at metalness 0

    constexpr float kRoughness = 0.5f;

    glm::vec3 Direct( const glm::vec3& radiance, const glm::vec3& albedo = kAlbedo, float metalness = 0.0f,
                      const glm::vec3& F0 = kF0, float roughness = kRoughness )
    {
        return EvaluateDirectLight( kL, radiance, kView, kN, F0, metalness, roughness, albedo );
    }

    // The DIRECTIONAL ALBEDO of the surface: of the light arriving from L, how much leaves it at all,
    // integrated over the whole outgoing hemisphere. This is the quantity a missing 1/PI multiplies by
    // PI, and it is the one number that says "energy" rather than "brightness".
    //
    // F0 is passed as zero at the call sites below, which is NOT the same as switching the specular
    // lobe off: Schlick still returns 1 at grazing from a zero base reflectance, so both halves are
    // live. That is deliberate — the assertion is about the whole BRDF, not a half of it isolated by a
    // trick. Quadrature is a plain 128x512 midpoint grid over (theta, phi); the integrand is smooth and
    // the number it has to separate is a factor of 3.14.
    float DirectionalAlbedo( float albedo, float roughness )
    {
        const glm::vec3 radiance( 1.0f );
        const float     cosLi = glm::max( glm::dot( kN, kL ), 0.0f );

        constexpr int   kTheta = 128;
        constexpr int   kPhi   = 512;
        constexpr float kPi    = 3.14159265358979f;

        float integral = 0.0f;
        for ( int i = 0; i < kTheta; ++i )
        {
            const float theta = ( i + 0.5f ) / kTheta * ( kPi * 0.5f );
            for ( int j = 0; j < kPhi; ++j )
            {
                const float     phi = ( j + 0.5f ) / kPhi * ( 2.0f * kPi );
                const glm::vec3 wo( std::sin( theta ) * std::cos( phi ), std::sin( theta ) * std::sin( phi ),
                                    std::cos( theta ) );

                // EvaluateDirectLight already multiplies by radiance and cos(theta_i); divide those out
                // to recover the BRDF value, then apply the outgoing cosine and the solid-angle element.
                const glm::vec3 out = EvaluateDirectLight( kL, radiance, wo, kN, glm::vec3( 0.0f ), 0.0f,
                                                           roughness, glm::vec3( albedo ) );

                integral += ( out.r / cosLi ) * std::cos( theta ) * std::sin( theta ) * ( kPi * 0.5f / kTheta ) *
                            ( 2.0f * kPi / kPhi );
            }
        }
        return integral;
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// THE bound. One assertion that catches a spurious factor of anything, and this one was PI.
// ---------------------------------------------------------------------------------------------------

TEST( DirectLighting, TheSurfaceReflectsExactlyItsAlbedoAndNotPiTimesIt )
{
    // A surface is not a light source. Integrated over the outgoing hemisphere, a Lambertian reflector
    // returns exactly `albedo` of what arrives — no more. That identity is what the 1/PI IS: not a
    // tuning constant but the normalization that makes the hemisphere's PI steradians of projected
    // solid angle come back out at unity.
    //
    // The deleted forward form (`kd * albedo`, no /PI) returns PI * albedo here, so a white surface
    // emitted 3.14 times the light that fell on it. That is the defect stated as physics rather than as
    // a diff, and no amount of retuning a scene can satisfy this assertion without the divide.
    //
    // The tolerance is 4e-3 and it is a MEASUREMENT, not a shrug: swept over roughness 0.1..1.0 and
    // albedo 0..1 the worst departure from perfect conservation is +0.0028 (a rough dielectric at
    // albedo 0, where Schlick's grazing reflectance adds a little) and -0.0009 (a very rough white
    // surface, where the Smith visibility takes a little). Pinned so the model's small imbalance stays
    // a known number rather than an unexamined one.
    float worst = 0.0f;
    for ( float roughness = 0.1f; roughness <= 1.0f; roughness += 0.1f )
        for ( float a = 0.0f; a <= 1.0f; a += 0.1f )
        {
            const float reflected = DirectionalAlbedo( a, roughness );

            EXPECT_NEAR( reflected, a, 4e-3f )
                 << "directional albedo at albedo " << a << ", roughness " << roughness
                 << " — a factor of PI here is the defect this suite exists for";
            EXPECT_LE( reflected, 1.01f ) << "the surface reflected more light than reached it";

            worst = glm::max( worst, std::abs( reflected - a ) );
        }

    EXPECT_LT( worst, 4e-3f ) << "energy imbalance grew past the model's known one: " << worst;
}

TEST( DirectLighting, NoLightArrivesFromBelowTheHorizon )
{
    // Exactly zero, not a small number: a light behind the surface contributes nothing, and a
    // formulation that leaks here puts a rim of sun on the shadowed side of everything.
    const glm::vec3 below = glm::normalize( glm::vec3( 0.0f, 0.7071f, -0.7071f ) );
    const glm::vec3 out =
         EvaluateDirectLight( below, glm::vec3( 4.0f ), kView, kN, kF0, 0.0f, kRoughness, kAlbedo );

    EXPECT_TRUE( glm::all( glm::epsilonEqual( out, glm::vec3( 0.0f ), 1e-6f ) ) );
}

TEST( DirectLighting, TheResponseToTheLightIsExactlyLinear )
{
    // The other half of the bound, and the one that catches a stray multiplier wherever it hides: the
    // response is linear in incident radiance, so its GAIN is a constant of the surface and not of the
    // light. A spurious factor, a square, or a light counted twice all break this while leaving a
    // single spot value looking entirely reasonable.
    const glm::vec3 unit = Direct( glm::vec3( 1.0f ) );
    ASSERT_GT( unit.r, 0.0f );

    for ( float scale = 0.5f; scale <= 64.0f; scale *= 2.0f )
    {
        const glm::vec3 scaled = Direct( glm::vec3( scale ) );
        EXPECT_NEAR( scaled.r, unit.r * scale, unit.r * scale * 1e-5f ) << "radiance scale " << scale;
    }
}

TEST( DirectLighting, TheResponseFallsWithTheIncidenceAngleAndVanishesAtGrazing )
{
    // Monotonicity: the same light delivers less to a surface it strikes obliquely, all the way to zero
    // at grazing. Catches an N·L dropped, doubled, or replaced by its absolute value — none of which a
    // single spot value distinguishes from the truth.
    float previous = 1e30f;
    for ( float cosLi = 1.0f; cosLi > 0.0f; cosLi -= 0.05f )
    {
        const glm::vec3 L( 0.0f, std::sqrt( glm::max( 1.0f - cosLi * cosLi, 0.0f ) ), cosLi );
        // View along the normal, so only the incidence angle moves.
        const glm::vec3 out =
             EvaluateDirectLight( L, glm::vec3( 1.0f ), kN, kN, glm::vec3( 0.0f ), 0.0f, kRoughness, kAlbedo );

        EXPECT_LT( out.r, previous ) << "response did not fall at cosLi " << cosLi;
        previous = out.r;
    }
    EXPECT_LT( previous, Direct( glm::vec3( 1.0f ) ).r );
}

// ---------------------------------------------------------------------------------------------------
// Where albedo enters, and where it must not.
// ---------------------------------------------------------------------------------------------------

TEST( DirectLighting, AlbedoTintsTheDiffuseHalfAndNotTheSpecularHighlight )
{
    // A dielectric's highlight is the LIGHT's colour, not the paint's: at metalness 0 the surface's F0
    // is Fdielectric whatever the albedo, so the response must be AFFINE in albedo with a colourless
    // constant term. Applying albedo to both halves is the easiest way to "fix" a frame that came out
    // dark, and it is wrong in a way that only shows on coloured plastic.
    const glm::vec3 black = Direct( glm::vec3( 1.0f ), glm::vec3( 0.0f ) ); // the highlight alone

    EXPECT_GT( black.r, 0.0f ) << "a black dielectric lost its highlight entirely";
    EXPECT_NEAR( black.r, black.g, 1e-6f ) << "a dielectric's highlight took on a colour";
    EXPECT_NEAR( black.r, black.b, 1e-6f );

    // What albedo adds on top is exactly proportional to albedo, per channel and independently.
    const glm::vec3 red = Direct( glm::vec3( 1.0f ), glm::vec3( 0.8f, 0.1f, 0.1f ) );

    const float perUnitRed   = ( red.r - black.r ) / 0.8f;
    const float perUnitGreen = ( red.g - black.g ) / 0.1f;
    EXPECT_NEAR( perUnitRed, perUnitGreen, 1e-5f ) << "the diffuse half is not linear in albedo";
    EXPECT_GT( perUnitRed, 0.0f );

    // And the channels really are independent: an 8:1 albedo ratio is an 8:1 ratio in what albedo adds.
    EXPECT_NEAR( ( red.r - black.r ) / ( red.g - black.g ), 8.0f, 1e-3f );
}

TEST( DirectLighting, AMetalHasNoDiffuseResponse )
{
    // Conductors have no diffuse lobe: at metalness 1 the albedo must contribute nothing through the
    // diffuse half, and everything the surface shows is the specular reflection of the light.
    const glm::vec3 dark   = Direct( glm::vec3( 2.0f ), glm::vec3( 0.05f ), 1.0f, glm::vec3( 1.0f ) );
    const glm::vec3 bright = Direct( glm::vec3( 2.0f ), glm::vec3( 0.95f ), 1.0f, glm::vec3( 1.0f ) );

    EXPECT_TRUE( glm::all( glm::epsilonEqual( dark, bright, 1e-6f ) ) )
         << "albedo leaked into a conductor's response";
}

// ---------------------------------------------------------------------------------------------------
// The directional wrapper: the sun's payload gives the direction the light TRAVELS.
// ---------------------------------------------------------------------------------------------------

TEST( DirectLighting, TheDirectionalWrapperNegatesAndNormalizesItsTravelDirection )
{
    // Both sun call sites hold the direction the light travels, not the direction toward it. Getting
    // that backwards lights the shadowed side of everything, and the negate now lives in one place.
    // The scale is there because DirectionLightComponent's payload is a normalized transform
    // translation and DeferredUB's is not guaranteed to be — the wrapper must not care.
    const glm::vec3 travel = -kL;

    const glm::vec3 viaWrapper =
         EvaluateDirectionalLight( travel * 7.5f, glm::vec3( 1.0f ), kView, kN, kF0, 0.0f, kRoughness, kAlbedo );
    const glm::vec3 direct = Direct( glm::vec3( 1.0f ) );

    EXPECT_TRUE( glm::all( glm::epsilonEqual( viaWrapper, direct, 1e-5f ) ) );

    // And the sign really is a sign: handing it the direction TOWARD the light must light nothing.
    const glm::vec3 flipped =
         EvaluateDirectionalLight( kL, glm::vec3( 1.0f ), kView, kN, kF0, 0.0f, kRoughness, kAlbedo );
    EXPECT_TRUE( glm::all( glm::epsilonEqual( flipped, glm::vec3( 0.0f ), 1e-6f ) ) );
}

// ---------------------------------------------------------------------------------------------------
// THE relation: the two render paths, and the four light types.
// ---------------------------------------------------------------------------------------------------

namespace
{
    std::filesystem::path ShaderRoot()
    {
        // The test binary lives in build/Bin/Tests/<config>; walk up to the repository root.
        std::filesystem::path root = std::filesystem::current_path();
        for ( int up = 0; up < 8 && !std::filesystem::exists( root / "Editor" / "Resources" / "Shaders" ); ++up )
            root = root.parent_path();
        return root / "Editor" / "Resources" / "Shaders";
    }

    std::string Read( const std::filesystem::path& file )
    {
        std::ifstream      in( file, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }
} // namespace

// The relation the owner's experiment measured, and the only form in which it can be stated: not that
// two numbers agree, but that there is only ONE number. Every direct light in the engine — the sun on
// both render paths, point lights and spot lights — reaches its BRDF through Mesh/DirectLighting.glslh.
//
// A numeric comparison here would be theatre: it would call the same C++ function twice and present the
// agreement as evidence. What can actually regress is a shader growing its own copy of the BRDF again,
// which is a fact about the shader SOURCES, so that is what this reads.
TEST( DirectLighting, EveryDirectLightInTheEngineReachesTheOneSharedBRDF )
{
    const std::filesystem::path root = ShaderRoot();
    ASSERT_TRUE( std::filesystem::exists( root ) )
         << "could not find Editor/Resources/Shaders above " << std::filesystem::current_path();

    // Each consumer and the call it must make. The sun call sites take the travel-direction wrapper;
    // the point and spot headers already hold a normalized L and take the core.
    const std::pair<const char*, const char*> kConsumers[] = {
         { "Programs/Deferred/DeferredLighting.shader", "EvaluateDirectionalLight(" }, // the deferred sun
         { "Programs/PBR/StaticMeshPBR.shader", "EvaluateDirectionalLight(" },         // the forward sun
         { "Programs/PBR/StaticMeshPBR_Instanced.shader", "EvaluateDirectionalLight(" },
         { "Programs/PBR/SkinnedMeshPBR.shader", "EvaluateDirectionalLight(" }, // drawn FORWARD in Deferred
         { "Mesh/PointLight.glslh", "EvaluateDirectLight(" },
         { "Mesh/Spotlight.glslh", "EvaluateDirectLight(" },
    };

    for ( const auto& [relative, call] : kConsumers )
    {
        const std::filesystem::path file = root / relative;
        ASSERT_TRUE( std::filesystem::exists( file ) ) << file.string();

        const std::string source = Read( file );

        EXPECT_NE( source.find( "DirectLighting.glslh" ), std::string::npos )
             << relative << " does not compile the shared direct-light BRDF";
        EXPECT_NE( source.find( call ), std::string::npos )
             << relative << " does not evaluate its light through " << call;
    }
}

TEST( DirectLighting, NoShaderCarriesADiffuseLobeOfItsOwn )
{
    // The construction, stated as the thing that must stay true: `albedo / PI` appears in exactly ONE
    // file of code in the whole shader tree, and no shader spells out the naked `kd * albedo` form the
    // forward path used to ship. Four copies is how the paths drifted; a fifth would do it again.
    //
    // Mesh/AmbientIBL.glslh is exempt from the second half by construction rather than by exception: its
    // `kd * albedo * irradiance` is the ambient, whose irradiance cube is already an exitant Lambertian
    // radiance and must NOT be divided by PI a second time. It is matched by neither pattern.
    const std::filesystem::path root = ShaderRoot();
    ASSERT_TRUE( std::filesystem::exists( root ) );

    int withDivision = 0;
    for ( const auto& entry : std::filesystem::recursive_directory_iterator( root ) )
    {
        if ( !entry.is_regular_file() )
            continue;
        const std::string ext = entry.path().extension().string();
        if ( ext != ".shader" && ext != ".glslh" )
            continue;

        const std::string name = entry.path().filename().string();
        if ( name == "DirectLighting.glslh" )
        {
            withDivision++;
            continue;
        }

        // Strip comments so the explanatory notes that QUOTE the deleted form (this is a codebase that
        // records its defects at the site) are not read as code.
        std::istringstream in( Read( entry.path() ) );
        std::string        line;
        while ( std::getline( in, line ) )
        {
            const std::size_t comment = line.find( "//" );
            if ( comment != std::string::npos )
                line.erase( comment );

            EXPECT_EQ( line.find( "albedo / PI" ), std::string::npos )
                 << name << " carries its own Lambert normalization: " << line;
            EXPECT_EQ( line.find( "diffuseBRDF = kd * albedo;" ), std::string::npos )
                 << name << " carries the un-normalized diffuse the forward path used to ship: " << line;
        }
    }

    EXPECT_EQ( withDivision, 1 ) << "Mesh/DirectLighting.glslh was not found under the shader root";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
