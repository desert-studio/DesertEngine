// The ambient term both render paths shade with, tested as the RELATION it has to be — not as a
// function that returns plausible numbers.
//
// The defect this suite exists for: `Programs/Deferred/DeferredLighting.shader` computed its ambient as
// `albedo * ao * (vec3(0.08) + indirect)` — a flat constant that read neither environment cube — while
// the forward mesh shaders computed theirs from the baked sky. 51 of the repository's 53 scenes are
// Deferred, so essentially the whole project's ordinary static opaque geometry was lit by a constant.
// The owner saw it as ground and buildings almost black under a bright sky; flipping RenderingPath from
// 1 to 0 on the same scene, same camera, same 90 frames lit them.
//
// Every assertion below is about the SHIPPED TEXT (Mesh/AmbientIBL.glslh compiled as C++), and each
// picks a property that a constant cannot have, so the specific way this was wrong is now unbuildable:
//
//   * the ambient RESPONDS to the environment, strictly and monotonically — a constant does not;
//   * it is BOUNDED by the environment it integrates, so no amount of retuning can make a surface a
//     light source (the bound this project reached for after a radiance came out 5000x too large);
//   * albedo enters the diffuse half and NOT the specular half, and the bounce enters as incident
//     light — the three places albedo could be applied twice or not at all;
//   * the two paths' compositions are ONE call, and the forward path is the deferred one at zero
//     indirect. That is the disagreement the owner saw, asserted directly.

#include <gtest/gtest.h>

#include "AmbientIBLReference.hpp"

#include <glm/gtc/epsilon.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace Desert::Tests::AmbientIBLRef;

namespace
{
    // A plausible G-buffer texel: mid-grey dielectric, half-rough, seen at a moderate angle.
    constexpr float kCosLo     = 0.7f;
    constexpr float kMetalness = 0.0f;

    const glm::vec3 kAlbedo = glm::vec3( 0.5f, 0.5f, 0.5f );
    const glm::vec3 kF0     = glm::vec3( 0.04f ); // Fdielectric, as both shaders build it for metalness 0

    // Split-sum LUT values are (scale, bias) for F0; these are representative mid-roughness numbers.
    const glm::vec2 kBrdf = glm::vec2( 0.85f, 0.05f );

    glm::vec3 AmbientFor( const glm::vec3& irradiance, const glm::vec3& prefiltered )
    {
        return EvaluateAmbientIBL( irradiance, prefiltered, kBrdf, kF0, kAlbedo, kMetalness, kCosLo );
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// The property the flat constant could not have.
// ---------------------------------------------------------------------------------------------------

TEST( AmbientIBL, RespondsStrictlyAndMonotonicallyToTheEnvironment )
{
    // A bright sky must produce more ambient than a dark one, at every step. This is the whole content
    // of "the deferred path reads the bake": the old `vec3(0.08)` returns the same number for all of
    // these, which is exactly why a scene under an overcast and a scene under a clear noon looked
    // identical on the ground.
    glm::vec3 previous( -1.0f );
    for ( float e = 0.0f; e <= 4.0f; e += 0.25f )
    {
        const glm::vec3 env     = glm::vec3( e );
        const glm::vec3 ambient = AmbientFor( env, env );

        EXPECT_GT( ambient.r, previous.r ) << "ambient did not increase with environment radiance " << e;
        previous = ambient;
    }

    // And the endpoints, so a failure says how much rather than only that it stopped rising.
    EXPECT_NEAR( AmbientFor( glm::vec3( 0.0f ), glm::vec3( 0.0f ) ).r, 0.0f, 1e-6f );
    EXPECT_GT( AmbientFor( glm::vec3( 4.0f ), glm::vec3( 4.0f ) ).r, 1.0f );
}

TEST( AmbientIBL, TheAntiBlackFloorIsNotAnAmbientModel )
{
    // The constant that was deleted was 0.08 per channel and it WAS the deferred path's ambient. What
    // replaces it is a floor two orders of magnitude smaller, whose only job is to keep a fully
    // occluded face with no environment from reading as a pure-black hole. Pinned so that nobody
    // quietly raises it back into a lighting model when a scene looks dark — that is the "knob that
    // hides a defect" the contract forbids.
    EXPECT_LT( glm::max( glm::max( PBR_AMBIENT_FLOOR.r, PBR_AMBIENT_FLOOR.g ), PBR_AMBIENT_FLOOR.b ), 0.02f );
    EXPECT_GT( glm::min( glm::min( PBR_AMBIENT_FLOOR.r, PBR_AMBIENT_FLOOR.g ), PBR_AMBIENT_FLOOR.b ), 0.0f );

    // Under any sky worth calling lit, the floor is noise: at unit environment radiance the real
    // ambient must dominate it by a wide margin.
    const glm::vec3 lit = AmbientFor( glm::vec3( 1.0f ), glm::vec3( 1.0f ) );
    EXPECT_GT( lit.r, PBR_AMBIENT_FLOOR.r * 20.0f );
}

// ---------------------------------------------------------------------------------------------------
// Bounds — one assertion that catches any spurious factor of anything.
// ---------------------------------------------------------------------------------------------------

TEST( AmbientIBL, NeitherHalfReflectsMoreLightThanItsOwnEnvironmentDelivers )
{
    // A surface is not a light source. Stated per HALF, which is where it is actually true and tight:
    //
    //   * the diffuse half is kd * albedo * irradiance with kd <= 1 and albedo <= 1, so it cannot exceed
    //     the irradiance;
    //   * the specular half is (F0 * scale + bias) * prefiltered, and a physical split-sum LUT keeps
    //     F0 * scale + bias <= 1 for F0 <= 1, so it cannot exceed the prefiltered radiance.
    //
    // NOT stated for the sum, and that is a finding rather than a weakened test. Measured here, the sum
    // exceeds a white furnace by up to 4.2 % at metalness 0: the split-sum approximation weights the
    // diffuse lobe by (1 - F) and does not additionally subtract the specular energy that the LUT then
    // adds back, so a little energy is created. That is a property of the MODEL — Karis 2013 without a
    // multi-scatter compensation term — and both render paths now share it exactly, which is the point.
    // Anyone tempted to "fix" the 4 % should change the model in Mesh/AmbientIBL.glslh, where both paths
    // will get it, and not add a scale factor at one call site.
    const glm::vec3 white( 1.0f );
    const float     E = 2.0f;

    for ( float metalness = 0.0f; metalness <= 1.0f; metalness += 0.25f )
        for ( float cosLo = 0.05f; cosLo <= 1.0f; cosLo += 0.05f )
            for ( float scale = 0.0f; scale <= 1.0f; scale += 0.25f )
            {
                const glm::vec2 brdf( scale, 1.0f - scale ); // the physical extreme: scale + bias == 1
                const glm::vec3 F0 = glm::mix( glm::vec3( 0.04f ), white, metalness );

                const glm::vec3 diffuseOnly =
                     EvaluateAmbientIBL( glm::vec3( E ), glm::vec3( 0.0f ), brdf, F0, white, metalness, cosLo );
                const glm::vec3 specularOnly =
                     EvaluateAmbientIBL( glm::vec3( 0.0f ), glm::vec3( E ), brdf, F0, white, metalness, cosLo );

                EXPECT_LE( diffuseOnly.r, E + 1e-4f )
                     << "diffuse: metalness " << metalness << " cosLo " << cosLo << " scale " << scale;
                EXPECT_GE( diffuseOnly.r, 0.0f );
                EXPECT_LE( specularOnly.r, E + 1e-4f )
                     << "specular: metalness " << metalness << " cosLo " << cosLo << " scale " << scale;
                EXPECT_GE( specularOnly.r, 0.0f );
            }

    // The 4.2 % above, measured rather than asserted from memory — and bounded, so the model's energy
    // excess is a pinned number instead of an unexamined one. It is quoted for the representative
    // mid-roughness dielectric LUT sample the rest of this suite uses.
    float worstExcess = 0.0f;
    for ( float metalness = 0.0f; metalness <= 1.0f; metalness += 0.25f )
        for ( float cosLo = 0.05f; cosLo <= 1.0f; cosLo += 0.05f )
        {
            const glm::vec3 F0 = glm::mix( glm::vec3( 0.04f ), white, metalness );
            const glm::vec3 a =
                 EvaluateAmbientIBL( glm::vec3( E ), glm::vec3( E ), kBrdf, F0, white, metalness, cosLo );
            worstExcess = glm::max( worstExcess, a.r / E - 1.0f );
        }

    EXPECT_GT( worstExcess, 0.0f ) << "the split-sum overlap disappeared — the model changed";
    EXPECT_LT( worstExcess, 0.05f ) << "white-furnace excess grew past the split-sum's known overlap: "
                                    << worstExcess * 100.0f << " %";
}

TEST( AmbientIBL, TheResponseToTheEnvironmentIsExactlyLinear )
{
    // The other half of the bound, and the one that catches a spurious factor wherever it hides: the
    // ambient is linear in the environment, so its GAIN is a constant of the surface and not of the sky.
    // A stray multiplier, a stray square, or an environment sampled twice all break this while leaving
    // a single spot value looking entirely reasonable — which is how a radiance came out 5000x too
    // large in this engine and read as "the tonemapper needs tuning".
    const glm::vec3 unit = AmbientFor( glm::vec3( 1.0f ), glm::vec3( 1.0f ) );
    ASSERT_GT( unit.r, 0.0f );

    for ( float scale = 0.5f; scale <= 64.0f; scale *= 2.0f )
    {
        const glm::vec3 scaled = AmbientFor( glm::vec3( scale ), glm::vec3( scale ) );
        EXPECT_NEAR( scaled.r, unit.r * scale, unit.r * scale * 1e-5f ) << "environment scale " << scale;
    }
}

TEST( AmbientIBL, DarknessInIsDarknessOut )
{
    // No environment, no bounce, no floor applied yet: the split-sum term itself must be exactly zero.
    // A non-zero result here is an additive constant hiding inside the ambient, which is the entire
    // defect this task removed.
    const glm::vec3 a = AmbientFor( glm::vec3( 0.0f ), glm::vec3( 0.0f ) );

    EXPECT_TRUE( glm::all( glm::epsilonEqual( a, glm::vec3( 0.0f ), 1e-6f ) ) );
}

// ---------------------------------------------------------------------------------------------------
// Where albedo enters, and where it must not.
// ---------------------------------------------------------------------------------------------------

TEST( AmbientIBL, AlbedoTintsTheDiffuseHalfAndNotTheReflection )
{
    // Two surfaces differing ONLY in albedo, under an environment that is diffuse-only (prefiltered
    // radiance zero) and then specular-only (irradiance zero). The first must scale with albedo; the
    // second must not move at all — a reflection is not tinted by the surface it leaves. Applying
    // albedo to both halves is the single most likely way to "fix" a dark deferred frame, and it is
    // wrong in a way that only shows on coloured metal.
    const glm::vec3 red  = glm::vec3( 0.8f, 0.1f, 0.1f );
    const glm::vec3 grey = glm::vec3( 0.8f, 0.8f, 0.8f );

    const glm::vec3 diffRed =
         EvaluateAmbientIBL( glm::vec3( 1.0f ), glm::vec3( 0.0f ), kBrdf, kF0, red, 0.0f, kCosLo );
    const glm::vec3 diffGrey =
         EvaluateAmbientIBL( glm::vec3( 1.0f ), glm::vec3( 0.0f ), kBrdf, kF0, grey, 0.0f, kCosLo );
    EXPECT_NEAR( diffRed.r, diffGrey.r, 1e-6f ); // same red channel in both albedos
    EXPECT_LT( diffRed.g, diffGrey.g * 0.5f );   // green follows albedo

    const glm::vec3 specRed =
         EvaluateAmbientIBL( glm::vec3( 0.0f ), glm::vec3( 1.0f ), kBrdf, kF0, red, 0.0f, kCosLo );
    const glm::vec3 specGrey =
         EvaluateAmbientIBL( glm::vec3( 0.0f ), glm::vec3( 1.0f ), kBrdf, kF0, grey, 0.0f, kCosLo );
    EXPECT_TRUE( glm::all( glm::epsilonEqual( specRed, specGrey, 1e-6f ) ) );
}

TEST( AmbientIBL, AMetalHasNoDiffuseAmbient )
{
    // Conductors have no diffuse lobe: at metalness 1 the irradiance cube must contribute nothing, and
    // everything the surface shows comes from the prefiltered cube. If the diffuse half leaks here, a
    // metal under a bright sky washes out to its albedo.
    const glm::vec3 a = EvaluateAmbientIBL( glm::vec3( 3.0f ), glm::vec3( 0.0f ), kBrdf, glm::vec3( 1.0f ),
                                            kAlbedo, 1.0f, kCosLo );

    EXPECT_TRUE( glm::all( glm::epsilonEqual( a, glm::vec3( 0.0f ), 1e-6f ) ) );
}

// ---------------------------------------------------------------------------------------------------
// THE relation: the two render paths' ambient.
// ---------------------------------------------------------------------------------------------------

TEST( AmbientIBL, TheCompositionCarriesNoConstantTermOfItsOwn )
{
    // With nothing to reflect and nothing to reflect it with, the ambient is zero. `vec3(0.08)` was
    // precisely a constant term that survived a black environment and a black albedo, so this is the
    // shape of the deleted defect stated as a property. It holds for every AO, which also rules out a
    // constant smuggled in outside the occlusion factor.
    for ( float ao = 0.0f; ao <= 1.0f; ao += 0.125f )
    {
        const glm::vec3 nothing = ComposeAmbient( glm::vec3( 0.0f ), glm::vec3( 0.0f ), ao, glm::vec3( 0.0f ) );

        EXPECT_TRUE( glm::all( glm::epsilonEqual( nothing, glm::vec3( 0.0f ), 1e-6f ) ) ) << "ao " << ao;
    }

    // And with a real sky it is the sky that dominates, not the floor.
    const glm::vec3 ibl     = AmbientFor( glm::vec3( 1.2f ), glm::vec3( 1.2f ) );
    const glm::vec3 ambient = ComposeAmbient( ibl, kAlbedo, 1.0f, glm::vec3( 0.0f ) );
    EXPECT_GT( ambient.r, kAlbedo.r * PBR_AMBIENT_FLOOR.r * 10.0f );
}

// THE relation the owner's experiment measured, and the only form in which it can be stated: not that
// two numbers agree, but that there is only ONE number. Both render paths reach their ambient through
// ComposeAmbient in Mesh/AmbientIBL.glslh, and neither adds a term beside it.
//
// A numeric comparison here would be theatre — it would compute the same C++ function twice and call
// the agreement evidence. What can actually regress is a shader growing its own ambient again, which is
// a fact about the shader SOURCES, so that is what this reads.
TEST( AmbientIBL, BothRenderPathsReachTheirAmbientThroughTheOneSharedComposition )
{
    // The test binary lives in build/Bin/Tests/<config>; walk up to the repository root.
    std::filesystem::path root = std::filesystem::current_path();
    for ( int up = 0; up < 8 && !std::filesystem::exists( root / "Editor" / "Resources" / "Shaders" ); ++up )
        root = root.parent_path();
    ASSERT_TRUE( std::filesystem::exists( root / "Editor" / "Resources" / "Shaders" ) )
         << "could not find Editor/Resources/Shaders above " << std::filesystem::current_path();

    // Every shader that shades an opaque surface's ambient, on both paths. StaticMeshGlass is not here:
    // it composes a refraction, not an ambient, and reads the specular cube for a mirror term instead.
    const char* kShaders[] = {
         "Programs/Deferred/DeferredLighting.shader", // the deferred composite — where 0.08 lived
         "Programs/PBR/StaticMeshPBR.shader",         // the forward path the owner compared against
         "Programs/PBR/StaticMeshPBR_Instanced.shader",
         "Programs/PBR/SkinnedMeshPBR.shader", // drawn FORWARD over the deferred composite
    };

    for ( const char* relative : kShaders )
    {
        const std::filesystem::path file = root / "Editor" / "Resources" / "Shaders" / relative;
        ASSERT_TRUE( std::filesystem::exists( file ) ) << file.string();

        std::ifstream      in( file, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        const std::string source = buffer.str();

        EXPECT_NE( source.find( "#include <Mesh/AmbientIBL.glslh>" ), std::string::npos )
             << relative << " does not compile the shared ambient";
        EXPECT_NE( source.find( "ComposeAmbient(" ), std::string::npos )
             << relative << " does not assemble its ambient through the shared composition";
    }
}

TEST( AmbientIBL, OcclusionAttenuatesTheWholeAmbientAndNotOnlyTheFloor )
{
    // AO multiplies the environment term AND the floor AND the bounce. An implementation that occludes
    // only part of the ambient reads as surfaces that never fully darken in a corner — and it is easy
    // to write by accident when the floor is added outside the AO factor, which is how the forward
    // shaders had it before this became one function.
    const glm::vec3 ibl      = AmbientFor( glm::vec3( 1.0f ), glm::vec3( 1.0f ) );
    const glm::vec3 indirect = glm::vec3( 0.3f );

    const glm::vec3 lit      = ComposeAmbient( ibl, kAlbedo, 1.0f, indirect );
    const glm::vec3 occluded = ComposeAmbient( ibl, kAlbedo, 0.0f, indirect );

    EXPECT_TRUE( glm::all( glm::epsilonEqual( occluded, glm::vec3( 0.0f ), 1e-6f ) ) );
    EXPECT_GT( lit.r, 0.0f );

    // Linear in ao, so half occlusion is half the ambient — no hidden curve.
    const glm::vec3 half = ComposeAmbient( ibl, kAlbedo, 0.5f, indirect );
    EXPECT_NEAR( half.r, lit.r * 0.5f, 1e-6f );
}

TEST( AmbientIBL, TheIndirectBounceArrivesAsIncidentLightAndIsReflectedByAlbedo )
{
    // The one asymmetry in ComposeAmbient, and the reason it is a function: `ibl` has already been
    // multiplied by albedo where albedo belongs, whereas `indirect` is incident light from a gather and
    // still has to be reflected. Swapping those is a whole-frame error that looks like bad tuning.
    const glm::vec3 ibl      = glm::vec3( 0.0f ); // isolate the bounce
    const glm::vec3 indirect = glm::vec3( 0.4f );

    const glm::vec3 dark  = ComposeAmbient( ibl, glm::vec3( 0.1f ), 1.0f, indirect );
    const glm::vec3 light = ComposeAmbient( ibl, glm::vec3( 0.8f ), 1.0f, indirect );

    // Eight times the albedo, so eight times the bounce — plus the floor, which scales with albedo too.
    EXPECT_NEAR( light.r / dark.r, 8.0f, 1e-4f );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
