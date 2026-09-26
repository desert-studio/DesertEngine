// Г16 — THE RELATION A CORNELL BOX EXISTS TO STATE, AND THE ONE NOBODY WAS ASKING:
//
//     Two surfaces standing symmetrically about a point light receive the same light from it.
//
// The defect this suite is born from is not a wrong line in any shader. `CornellDemo.desce` places
// CB_LeftRed at x = -300 and CB_RightGreen at x = +300, same scale, same primitive, with its only point
// light CB_BackLight at x = 0 — so both inner faces stand 386.1 cm from the source, both see it at
// N·L = 0.751, both take the same 0.460 distance attenuation, and both are viewed from a camera on the
// symmetry plane at N·V = 0.203. Every geometric input is equal to four decimals. The frame was not:
// the green wall came back lit and the red wall came back black.
//
// The cause was in neither the geometry nor the shader but in the MATERIAL ASSET. Editor/Resources/
// Assets/Materials/CB_Red.demat carried `RoughnessFactor 0.0` and `MetallicFactor 1.0` — a chrome
// mirror — where its five CB_* siblings and the builder that authors them (EditorLayer::
// BuildCornellShowcase -> CreatePBRMaterialAsset(..., red, 0.9f)) all say roughness 0.9 and no
// metalness at all. A conductor has no diffuse lobe (`kd = (1 - F) * (1 - metalness)` in
// Mesh/DirectLighting.glslh is identically zero at metalness 1) and a mirror's specular lobe only fires
// where the eye lies in the reflected direction of the source, which for a delta light is nowhere. The
// measured consequence, through the shipped text this suite compiles:
//
//     point-light response, luminance   left wall 0.000041   right wall 0.407380   ratio 1 : 9900
//
// Both sides of that were individually defensible — the shader is right about conductors, the asset is
// a legal material — which is the exact shape this project has now paid for repeatedly. So the
// assertion is the AGREEMENT, and it is stated on the shipped shader maths rather than on a CPU model
// that could be right while the GPU is wrong.
//
// HOW THE CAUSE WAS PINNED, because "the material" was a hypothesis and not a reading. Every frame
// below is `--camera 0,300,1400 --look 0,-0.1,-1 --shot-frames 90` at 715x764, and the noise floor was
// MEASURED rather than quoted: the same command run twice differs by 0 pixels of 546260. The numbers
// are the mean sRGB luminance of two rectangles that are mirror images of one another about the image
// centre — LEFT (30,100)-(130,500) and RIGHT (585,100)-(685,500):
//
//     knocked out        left    right   ratio
//     ---------------------------------------
//     nothing            0.010   0.563    56
//     EnableShadows      0.010   0.565    57   <- not the shadow map
//     EnableSSAO         0.010   0.563    56   <- not screen-space occlusion
//     GlobalIllumination 0.009   0.562    62   <- not the SSGI gather
//     RenderingPath -> 0 0.009   0.562    62   <- REPRODUCES IN THE FORWARD PATH; not a path divergence
//     CB_Sun             0.012   0.503    42   <- the asymmetry SURVIVES with only the symmetric light
//     CB_BackLight       0.007   0.185    26
//
// The `CB_Sun` row is the decisive one: with the only off-axis source deleted, what remains is a point
// light on the walls' mid-plane, and the two walls still differ by a factor of 42. Note also that the
// left wall got BRIGHTER when the sun was removed (0.010 -> 0.012) — a diffuse wall cannot do that, and
// a mirror can, because all it was ever showing was the environment cube's specular reflection, and
// deleting the sun changed the sky that cube is baked from.
//
// And then the instrumented frame, which is the argument no code reading produces: the deferred
// composite's own Metallic G-buffer channel (DeferredDebugMode::Metallic) renders the ENTIRE left wall
// white and every other surface in the box black — left rect mean 0.800, right rect mean 0.000. The
// Roughness channel says the same thing the other way: left 0.125, right 0.783.
//
// THE FIX WAS THE ASSET, AND WHAT IT MOVED IS RECORDED HERE so the next person does not re-shoot it.
// CB_Red.demat was restored to the shape of its sibling CB_Green.demat — same albedo, RoughnessFactor
// 0.9, MetallicFactor dropped (StaticMeshPBR.shader declares its default as 0), MaterialId untouched.
// Three scenes reference that asset and all three were shot before and after:
//
//   CornellDemo             11.949 % of pixels, max 227/255 — the left wall and what it bounces onto
//   MAT_ProbeDeferredNoSlot 11.949 % of pixels, max 227/255 — the same box, same wall, intended
//   DepthPrecisionProbe     18.044 % of pixels, max  11/255 — two quads lit almost entirely by a bright
//                                                             sky, where mirror and diffuse nearly agree
//
// And the scenes that share the lighting path but not the asset, which must not have moved: MAT_Probe,
// MAT_ProbeShadows and Clouds_Protocol all came back 0 differing pixels of 546260.
//
// Forward and deferred were checked BOTH before the fix (0.009 / 0.562 against the deferred 0.010 /
// 0.563 — the asymmetry reproduced in both, so it was never a path divergence) and after it (0.326 /
// 0.562 against 0.328 / 0.563). The residual two thousandths are the forward path's missing GI gather,
// which it passes as vec3(0) by construction.
//
// WHAT IS DELIBERATELY NOT ASSERTED. The scene's OTHER light, CB_Sun, is a directional light whose
// travel direction is normalize(0.6, -1, 0.2) — it is NOT on the symmetry plane, and it lights the
// right wall's inner face at N·L = 0.507 while missing the left wall's entirely. That asymmetry is
// honest physics and the fixture is entitled to it; the suite records the number (below) so that a
// future reader does not re-derive it, and asserts only that the POINT light is the symmetric one, which
// is what makes the claim above well-formed.

#include <Common/Json/Document.hpp>
#include <gtest/gtest.h>

#include "CornellSymmetryReference.hpp"

#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace Desert::Tests::CornellSymmetryRef;

namespace
{
    // The primitive Cube is 100 units on a side (PrimitiveMeshFactory.cpp: "one world unit is a
    // centimetre, so a Cube is 100 units on a side"), so a component scale of s spans 100*s and reaches
    // 50*s from its centre. The walls are thin slabs; their INNER face is the one the box is lit
    // through, and it is the surface every number below is taken on.
    constexpr float kCubeHalfExtent = 50.0f;

    // The acceptance camera the defect was found from: --camera 0,300,1400 --look 0,-0.1,-1. It matters
    // to the specular half of the BRDF, and it sits ON the symmetry plane, which the first test asserts
    // rather than assumes.
    const glm::vec3 kCameraPosition{ 0.0f, 300.0f, 1400.0f };

    // Fdielectric, as every call site in the engine builds it: mix(vec3(0.04), albedo, metalness).
    const glm::vec3 kDielectricF0{ 0.04f };

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Core/SceneSettings.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadAll( const std::string& path )
    {
        std::ifstream     file( path );
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    // The whole document, owned; everything below reads it through Common::Json::Node views.
    Common::Json::Value ParseObject( const std::string& json, const std::string& what )
    {
        const auto parsed = Common::Json::Parse( json );
        EXPECT_TRUE( parsed.IsSuccess() ) << what;
        if ( !parsed.IsSuccess() )
            return Common::Json::Value( Common::Json::Object{} );
        EXPECT_EQ( Common::Json::Root( parsed.GetValue() ).GetKind(), Common::Json::Kind::Object ) << what;
        return parsed.GetValue();
    }

    float Scalar( const Common::Json::Node& value )
    {
        const auto number = value.AsNumber();
        return number ? static_cast<float>( number.GetValue() ) : std::numeric_limits<float>::quiet_NaN();
    }

    float ScalarMember( const Common::Json::Node& owner, const std::string& key )
    {
        const auto field = owner.Get( key );
        EXPECT_TRUE( field ) << field.GetError();
        return field ? Scalar( field.GetValue() ) : std::numeric_limits<float>::quiet_NaN();
    }

    std::vector<Common::Json::Node> Elements( const Common::Json::Node& array )
    {
        std::vector<Common::Json::Node> out;
        array.ForEachElement( [&]( std::size_t, const Common::Json::Node& element )
                              { out.push_back( element ); } );
        return out;
    }

    glm::vec3 Vec3( const Common::Json::Node& owner, const std::string& key )
    {
        const auto field = owner.Get( key );
        EXPECT_TRUE( field ) << field.GetError();
        if ( !field )
            return {};
        EXPECT_EQ( field.GetValue().GetKind(), Common::Json::Kind::Array ) << key;
        const auto array = Elements( field.GetValue() );
        if ( array.size() < 3 )
            return {};
        return { Scalar( array[0] ), Scalar( array[1] ), Scalar( array[2] ) };
    }

    // One entity of CornellDemo, reduced to what a lighting question needs.
    struct Entity
    {
        Common::Json::Value Record;
        glm::vec3           Translation{ 0.0f };
        glm::vec3           Scale{ 1.0f };
    };

    Entity EntityByTag( const Common::Json::Value& scene, const std::string& tag )
    {
        const auto entities = Common::Json::Root( scene ).Get( "Entities" );
        EXPECT_TRUE( entities ) << entities.GetError();
        if ( !entities )
            return {};

        for ( const Common::Json::Node& record : Elements( entities.GetValue() ) )
        {
            const auto name = record.Find( "Tag" );
            if ( !name.has_value() )
                continue;
            const auto text = name->AsString();
            if ( !text || text.GetValue() != tag )
                continue;

            Entity found;
            found.Record      = record.Raw();
            found.Translation = Vec3( record, "Translation" );
            found.Scale       = Vec3( record, "Scale" );
            return found;
        }
        EXPECT_TRUE( false ) << "CornellDemo has no entity tagged " << tag;
        return {};
    }

    // A .demat as the shading path sees it: the three PBR schema params, defaulted exactly as
    // Programs/PBR/StaticMeshPBR.shader declares them (Albedo (1,1,1,1), Metallic 0, Roughness 0.5) so a
    // file that omits a param is read the way the GPU reads it and not the way a test would like to.
    struct Material
    {
        glm::vec3 Albedo{ 1.0f };
        float     Metallic  = 0.0f;
        float     Roughness = 0.5f;
    };

    Material LoadMaterial( const std::string& root, const std::string& relativePath )
    {
        const std::string         path = root + "Editor/Resources/Assets/" + relativePath;
        const Common::Json::Value file = ParseObject( ReadAll( path ), path );

        Material   material;
        const auto params = Common::Json::Root( file ).Get( "Params" );
        EXPECT_TRUE( params ) << path << ": " << params.GetError();
        if ( !params )
            return material;
        EXPECT_EQ( params.GetValue().GetKind(), Common::Json::Kind::Array ) << path;

        for ( const Common::Json::Node& entry : Elements( params.GetValue() ) )
        {
            const auto name  = entry.Find( "Name" );
            const auto value = entry.Find( "Value" );
            if ( !name.has_value() || !value.has_value() )
                continue;
            const auto key = name->AsString();
            if ( !key )
                continue;
            const auto components = Elements( *value );
            if ( components.empty() )
                continue;

            if ( key.GetValue() == "AlbedoColor" && components.size() >= 3 )
                material.Albedo = { Scalar( components[0] ), Scalar( components[1] ), Scalar( components[2] ) };
            else if ( key.GetValue() == "MetallicFactor" )
                material.Metallic = Scalar( components[0] );
            else if ( key.GetValue() == "RoughnessFactor" )
                material.Roughness = Scalar( components[0] );
        }
        return material;
    }

    std::string MaterialPathOf( const Entity& entity )
    {
        const auto paths = Common::Json::Root( entity.Record ).Get( "StaticMesh" );
        EXPECT_TRUE( paths ) << paths.GetError();
        if ( !paths )
            return {};
        const auto list = paths.GetValue().Get( "MaterialPaths" );
        EXPECT_TRUE( list ) << list.GetError();
        if ( !list )
            return {};
        const auto elements = Elements( list.GetValue() );
        EXPECT_FALSE( elements.empty() );
        if ( elements.empty() )
            return {};
        const auto first = elements[0].AsString();
        EXPECT_TRUE( first ) << first.GetError();
        return first ? first.GetValue() : std::string{};
    }

    // The point light as the shader receives it, straight out of the scene file.
    struct PointLightPayload
    {
        glm::vec3 Position{ 0.0f };
        glm::vec3 Color{ 1.0f };
        float     Intensity = 0.0f;
        float     Radius    = 0.0f;
        float     MinRadius = 0.0f;
        int       Falloff   = 1;
    };

    PointLightPayload LoadPointLight( const Common::Json::Value& scene, const std::string& tag )
    {
        const Entity entity = EntityByTag( scene, tag );
        const auto   block  = Common::Json::Root( entity.Record ).Get( "PointLight" );
        EXPECT_TRUE( block ) << tag << ": " << block.GetError();
        if ( !block )
            return {};
        const Common::Json::Node& data = block.GetValue();

        PointLightPayload light;
        light.Position  = entity.Translation;
        light.Color     = Vec3( data, "Color" );
        light.Intensity = ScalarMember( data, "Intensity" );
        light.Radius    = ScalarMember( data, "Radius" );
        light.MinRadius = ScalarMember( data, "MinRadius" );
        light.Falloff   = static_cast<int>( ScalarMember( data, "Falloff" ) );
        return light;
    }

    // What the deferred composite computes for one point light on one surface, through the SHIPPED
    // text: Mesh/PointLight.glslh's `CalculatePointLight` is `LightFalloffFactor` (PBRFunctions.glslh)
    // followed by `EvaluateDirectLight` (DirectLighting.glslh), and both of those are compiled as C++
    // by the reference header. The three lines below are the wrapper, which declares an SSBO and
    // therefore cannot be.
    glm::vec3 PointLightResponse( const PointLightPayload& light, const glm::vec3& surface,
                                  const glm::vec3& normal, const Material& material )
    {
        const glm::vec3 toLight  = light.Position - surface;
        const float     distance = glm::length( toLight );
        const glm::vec3 L        = glm::normalize( toLight );

        const float     attenuation = LightFalloffFactor( distance, light.MinRadius, light.Radius, light.Falloff );
        const glm::vec3 radiance    = light.Color * light.Intensity * attenuation;

        const glm::vec3 view = glm::normalize( kCameraPosition - surface );
        const glm::vec3 F0   = glm::mix( kDielectricF0, material.Albedo, material.Metallic );

        // The deferred composite clamps roughness off zero before shading (`max(gb.a, 0.04)` in
        // DeferredLighting.shader), and the BRDF's contract says the caller must. Clamping here is what
        // makes this the same evaluation the GPU performs, not a kinder one.
        return EvaluateDirectLight( L, radiance, view, normal, F0, material.Metallic,
                                    glm::max( material.Roughness, 0.04f ), material.Albedo );
    }

    float Luminance( const glm::vec3& c )
    {
        return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
    }

    struct Fixture
    {
        Common::Json::Value  Scene;
        Entity               Left;
        Entity               Right;
        PointLightPayload    Light;
        Material             LeftMaterial;
        Material             RightMaterial;
        glm::vec3            LeftFace{ 0.0f }; // a point on the inner face, at wall centre height
        glm::vec3            RightFace{ 0.0f };
        glm::vec3            LeftNormal{ 0.0f };
        glm::vec3            RightNormal{ 0.0f };
    };

    Fixture LoadFixture()
    {
        const std::string root = RepoRoot();
        EXPECT_FALSE( root.empty() ) << "repository root not found from the test's working directory";

        const std::string scenePath = root + "Editor/Resources/Assets/Scenes/CornellDemo.desce";

        Fixture fixture;
        fixture.Scene = ParseObject( ReadAll( scenePath ), scenePath );
        fixture.Left  = EntityByTag( fixture.Scene, "CB_LeftRed" );
        fixture.Right = EntityByTag( fixture.Scene, "CB_RightGreen" );
        fixture.Light = LoadPointLight( fixture.Scene, "CB_BackLight" );

        fixture.LeftMaterial  = LoadMaterial( root, MaterialPathOf( fixture.Left ) );
        fixture.RightMaterial = LoadMaterial( root, MaterialPathOf( fixture.Right ) );

        // Inner faces: the left wall sits at negative x and faces +X, the right wall mirrors it. Sampled
        // at the wall's own centre in y and z so the two samples are mirror images of one another and
        // nothing but x differs.
        const float leftInnerX  = fixture.Left.Translation.x + kCubeHalfExtent * fixture.Left.Scale.x;
        const float rightInnerX = fixture.Right.Translation.x - kCubeHalfExtent * fixture.Right.Scale.x;

        fixture.LeftFace  = { leftInnerX, fixture.Left.Translation.y, fixture.Left.Translation.z };
        fixture.RightFace = { rightInnerX, fixture.Right.Translation.y, fixture.Right.Translation.z };

        fixture.LeftNormal  = { 1.0f, 0.0f, 0.0f };
        fixture.RightNormal = { -1.0f, 0.0f, 0.0f };
        return fixture;
    }
} // namespace

// The premise. Everything below is a statement about two surfaces symmetric ABOUT THE LIGHT, and it is
// only well-formed while the fixture really is arranged that way — so the arrangement is asserted, not
// assumed. If a future edit moves the light or a wall, this is the test that says so, and the two
// equality tests below stop being claims about a defect and start being claims about a changed scene.
TEST( CornellSymmetry, TheTwoSideWallsStandSymmetricallyAboutThePointLightAndTheCamera )
{
    const Fixture fixture = LoadFixture();

    EXPECT_NEAR( fixture.Light.Position.x, 0.5f * ( fixture.Left.Translation.x + fixture.Right.Translation.x ),
                 0.01f )
         << "CB_BackLight is not on the mid-plane of the two side walls";
    EXPECT_NEAR( kCameraPosition.x, fixture.Light.Position.x, 0.01f )
         << "the acceptance camera is off the symmetry plane, so N.V differs between the walls";

    EXPECT_NEAR( fixture.Left.Translation.y, fixture.Right.Translation.y, 0.01f );
    EXPECT_NEAR( fixture.Left.Translation.z, fixture.Right.Translation.z, 0.01f );
    EXPECT_NEAR( fixture.Left.Scale.x, fixture.Right.Scale.x, 1e-4f );
    EXPECT_NEAR( fixture.Left.Scale.y, fixture.Right.Scale.y, 1e-4f );
    EXPECT_NEAR( fixture.Left.Scale.z, fixture.Right.Scale.z, 1e-4f );
}

// The geometry the BRDF actually consumes, rather than the transforms it is derived from. Measured on
// the shipped fixture: distance 386.1 cm, N.L 0.7510, attenuation 0.4600, N.V 0.2028 — on BOTH walls.
// This is the test that makes "the left wall is dark" a statement about the material and nothing else.
TEST( CornellSymmetry, BothInnerFacesSeeThePointLightIdentically )
{
    const Fixture fixture = LoadFixture();

    const glm::vec3 toLeft  = fixture.Light.Position - fixture.LeftFace;
    const glm::vec3 toRight = fixture.Light.Position - fixture.RightFace;

    const float distanceLeft  = glm::length( toLeft );
    const float distanceRight = glm::length( toRight );
    EXPECT_NEAR( distanceLeft, distanceRight, 1e-3f );

    EXPECT_NEAR( glm::dot( fixture.LeftNormal, glm::normalize( toLeft ) ),
                 glm::dot( fixture.RightNormal, glm::normalize( toRight ) ), 1e-5f );

    EXPECT_NEAR(
         LightFalloffFactor( distanceLeft, fixture.Light.MinRadius, fixture.Light.Radius, fixture.Light.Falloff ),
         LightFalloffFactor( distanceRight, fixture.Light.MinRadius, fixture.Light.Radius, fixture.Light.Falloff ),
         1e-6f );

    EXPECT_NEAR( glm::dot( fixture.LeftNormal, glm::normalize( kCameraPosition - fixture.LeftFace ) ),
                 glm::dot( fixture.RightNormal, glm::normalize( kCameraPosition - fixture.RightFace ) ), 1e-5f );
}

// THE RELATION. Same light, same geometry, same albedo — the two walls must reflect the same radiance.
// Albedo is held common on purpose: a Cornell box's walls differ in COLOUR by design and in nothing
// else, so making them differ in colour only is what isolates the thing that is allowed to vary from
// the things that are not. Evaluated through Mesh/DirectLighting.glslh itself.
//
// RED before the fix, on the shipped assets: left 0.000041 against right 0.407380, a factor of 9900,
// because CB_Red.demat carried metalness 1 (kd = (1 - F)(1 - metalness) = 0, no diffuse lobe at all)
// and roughness 0 (a mirror lobe that a delta light never lands in).
TEST( CornellSymmetry, TheTwoWallsReflectThePointLightEquallyOnceColourIsHeldCommon )
{
    const Fixture fixture = LoadFixture();

    // A neutral mid-grey: not a value either wall carries, so neither is favoured.
    const glm::vec3 commonAlbedo{ 0.5f };

    Material left  = fixture.LeftMaterial;
    Material right = fixture.RightMaterial;
    left.Albedo    = commonAlbedo;
    right.Albedo   = commonAlbedo;

    const float lit =
         Luminance( PointLightResponse( fixture.Light, fixture.RightFace, fixture.RightNormal, right ) );
    const float dark =
         Luminance( PointLightResponse( fixture.Light, fixture.LeftFace, fixture.LeftNormal, left ) );

    ASSERT_GT( lit, 0.0f ) << "the point light lights neither wall — the fixture, not the walls, changed";
    EXPECT_NEAR( dark, lit, 0.02f * lit )
         << "CB_LeftRed and CB_RightGreen respond to the same light, at the same distance and the same "
            "angle, by a factor of "
         << ( dark > 0.0f ? lit / dark : std::numeric_limits<float>::infinity() )
         << ". A Cornell box's side walls may differ in colour and in nothing else; check "
            "MetallicFactor/RoughnessFactor in Editor/Resources/Assets/Materials/.";
}

// The same claim said as data, one level below the physics: whatever the two walls are made of, they are
// made of the SAME thing. This is the assertion a reader reaches for when the one above goes red, and it
// names the two fields that can break it.
TEST( CornellSymmetry, TheTwoSideWallsDifferInColourAndInNothingElse )
{
    const Fixture fixture = LoadFixture();

    EXPECT_NEAR( fixture.LeftMaterial.Metallic, fixture.RightMaterial.Metallic, 1e-5f )
         << "one side wall of the Cornell box is a conductor and the other is not";
    EXPECT_NEAR( fixture.LeftMaterial.Roughness, fixture.RightMaterial.Roughness, 1e-5f )
         << "the two side walls of the Cornell box have different roughness";
}

// A MEASURED REFUSAL, kept as an assertion so that it cannot rot into a stale comment.
//
// CB_OrangeCube renders black in the acceptance frame, and that was reported alongside the wall as a
// second defect. It is not one: the cube sits at z = -120 with a half extent of 70 (scale 1.4 on a
// 100-unit primitive), so its front face is at z = -50 with a normal of +Z, while BOTH of the scene's
// lights are behind it.
//
//   point light  CB_BackLight at (0, 250, -250):  L = normalize(0, 120, -200), N.L = -0.858
//   sun          CB_Sun travelling normalize(0.6, -1, 0.2): L = -that, N.L = -0.169
//
// `EvaluateDirectLight` returns exactly vec3(0) at a non-positive cosine, so the front face receives no
// analytic light at all and shows only the ambient floor times its albedo. The cube's TOP face (normal
// +Y) has N.L = +0.359 against the point light and +0.845 against the sun, which is precisely why the
// two top corners that clear the glass sphere are the only orange in the frame.
//
// Verified rather than argued: a throwaway variant that moves the SAME point light from z = -250 to
// z = +250 and changes nothing else renders the cube's front face bright orange.
//
// So there is nothing to fix here, and this test is the deliverable — it pins the geometry the
// explanation rests on. If a future edit moves the cube or a light, this goes red and the explanation
// above stops being quoted at a scene it no longer describes.
TEST( CornellSymmetry, TheOrangeCubesFrontFaceIsTurnedAwayFromBothLights )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const std::string scenePath = root + "Editor/Resources/Assets/Scenes/CornellDemo.desce";
    const auto        scene     = ParseObject( ReadAll( scenePath ), scenePath );

    const Entity cube = EntityByTag( scene, "CB_OrangeCube" );
    ASSERT_GT( cube.Scale.z, 0.0f );

    const glm::vec3 frontFace = cube.Translation + glm::vec3( 0.0f, 0.0f, kCubeHalfExtent * cube.Scale.z );
    const glm::vec3 frontNormal{ 0.0f, 0.0f, 1.0f };
    const glm::vec3 topFace = cube.Translation + glm::vec3( 0.0f, kCubeHalfExtent * cube.Scale.y, 0.0f );
    const glm::vec3 topNormal{ 0.0f, 1.0f, 0.0f };

    const PointLightPayload light = LoadPointLight( scene, "CB_BackLight" );
    // The sun's direction is its TRANSLATION, normalized (Engine/Core/Scene.cpp) — not its rotation,
    // which nothing reads. It is the direction the light TRAVELS, so the direction toward it is minus it.
    const glm::vec3 towardSun = -glm::normalize( EntityByTag( scene, "CB_Sun" ).Translation );

    EXPECT_LE( glm::dot( frontNormal, glm::normalize( light.Position - frontFace ) ), 0.0f )
         << "the point light is no longer behind the orange cube";
    EXPECT_LE( glm::dot( frontNormal, towardSun ), 0.0f ) << "the sun is no longer behind the orange cube";

    EXPECT_GT( glm::dot( topNormal, glm::normalize( light.Position - topFace ) ), 0.0f );
    EXPECT_GT( glm::dot( topNormal, towardSun ), 0.0f );

    // And the cube's material is not a second instance of the wall's defect: it is an ordinary
    // dielectric, so "black" here is about where the light is and not about what the surface is.
    const Material orange = LoadMaterial( root, MaterialPathOf( cube ) );
    EXPECT_NEAR( orange.Metallic, 0.0f, 1e-5f );
}

// The scene's OTHER light is deliberately not symmetric, and that is worth pinning too — otherwise the
// next reader measures the two walls, finds them unequal even after the fix, and reopens a closed
// question. CB_Sun travels normalize(0.6, -1, 0.2): it reaches the right wall's inner face at
// N.L = +0.507 and misses the left wall's entirely at -0.507. Measured contribution to the frame:
// deleting CB_Sun takes the right wall from 0.563 to 0.503 mean sRGB luminance and leaves the left wall
// where it is.
TEST( CornellSymmetry, TheSunIsOffAxisOnPurposeAndOnlyReachesOneWall )
{
    const Fixture fixture = LoadFixture();

    const std::string root  = RepoRoot();
    const std::string path  = root + "Editor/Resources/Assets/Scenes/CornellDemo.desce";
    const auto        scene = ParseObject( ReadAll( path ), path );

    const glm::vec3 towardSun = -glm::normalize( EntityByTag( scene, "CB_Sun" ).Translation );

    const float onLeft  = glm::dot( fixture.LeftNormal, towardSun );
    const float onRight = glm::dot( fixture.RightNormal, towardSun );

    EXPECT_LT( onLeft, 0.0f );
    EXPECT_GT( onRight, 0.0f );
    EXPECT_NEAR( onLeft, -onRight, 1e-5f ) << "the two walls face opposite ways; the cosines must too";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
