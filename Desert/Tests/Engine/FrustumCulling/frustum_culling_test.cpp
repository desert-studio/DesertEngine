// ОТСЕЧЕНИЕ ОБЯЗАНО УБИРАТЬ НЕВИДИМОЕ И НЕ ТРОГАТЬ НИ ОДНОГО ВИДИМОГО ПИКСЕЛЯ.
//
// Шаг 3 `Docs/World/PROGRAMME.md`. До него в движке был класс `Frustum` с правильным именем, который
// не отсекал ничего: `IsInside` тестировал ТОЧКУ, а `Camera::GetFrustum()` не имел ни одного вызова
// в движке — он возвращал ссылку на изменяемый член, то есть был неconst, а рендер-путь держит
// КОНСТАНТНУЮ камеру. Ноль вызовов был не недосмотром, а следствием сигнатуры.
//
// Здесь утверждаются три вещи, и первая из них — та, ради которой тест вообще написан:
//
//   1. отсечение по ТОЧКЕ удаляет видимую геометрию, а по AABB — нет. Это положительный контроль в
//      виде теста: подмена коробки центром краснит его, а на кадре та же подмена видна только с
//      некоторых углов;
//   2. габариты переносятся в мир ВОСЕМЬЮ углами, а не двумя: у повёрнутого объекта пара
//      противоположных углов даёт коробку, которая его не содержит;
//   3. проходы, рисующие ИЗ ИСТОЧНИКА СВЕТА, не отсекаются фрустумом камеры — иначе «выигрыш»
//      оказывается кадром, в котором пропали тени.

#include <gtest/gtest.h>

#include <Engine/Core/Frustum.hpp>
#include <Engine/Core/Projection.hpp>
#include <Engine/Geometry/MeshBounds.hpp>
#include <Engine/Graphic/VisibilityCulling.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using Common::Math::AABB;
using Desert::Core::Frustum;
using Desert::Graphic::IsVisibleInView;

namespace
{
    namespace fs = std::filesystem;

    // THE ENGINE'S OWN PROJECTION BUILDER, not a copy of it. Frustum::Rebuild derives near and far from
    // the clip-space half-spaces, and those two planes are the only ones that depend on the depth
    // convention — a GL-convention matrix written out here would test a different class than the one the
    // renderer culls with, and would pass while the engine culled the visible range away.
    Frustum LookingDownNegativeZ()
    {
        const glm::mat4 projection =
             Desert::Core::MakePerspective( glm::radians( 60.0f ), 1.0f, 100.0f, 100000.0f );
        const glm::mat4 view =
             glm::lookAt( glm::vec3( 0.0f ), glm::vec3( 0.0f, 0.0f, -1.0f ), glm::vec3( 0.0f, 1.0f, 0.0f ) );
        return Frustum( projection, view );
    }

    AABB UnitBoxAt( const glm::vec3& centre, float halfExtent )
    {
        return AABB{ centre - glm::vec3( halfExtent ), centre + glm::vec3( halfExtent ) };
    }

    fs::path RepoRoot()
    {
        fs::path p = fs::current_path();
        for ( int i = 0; i < 8; ++i )
        {
            if ( fs::exists( p / "Desert" / "Common" ) && fs::exists( p / "Editor" ) )
            {
                return p;
            }
            p = p.parent_path();
        }
        return {};
    }

    std::string ReadFile( const fs::path& file )
    {
        const std::ifstream in( file );
        std::stringstream   ss;
        ss << in.rdbuf();
        return ss.str();
    }

    /// The body of `MeshRenderer::<name>`, from its signature to the next member definition.
    std::string BodyOf( const std::string& source, const std::string& name )
    {
        const std::string opener = "\n    void MeshRenderer::" + name + "(";
        const std::size_t begin  = source.find( opener );
        if ( begin == std::string::npos )
        {
            return {};
        }
        const std::size_t end  = source.find( "\n    void MeshRenderer::", begin + opener.size() );
        const std::size_t end2 = source.find( "\n    bool MeshRenderer::", begin + opener.size() );
        return source.substr( begin, std::min( end, end2 ) - begin );
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// 1. The box test itself.

TEST( FrustumCulling, ABoxDeepInsideTheViewIsDrawn )
{
    const Frustum frustum = LookingDownNegativeZ();
    EXPECT_TRUE( frustum.Intersects( UnitBoxAt( glm::vec3( 0.0f, 0.0f, -5000.0f ), 100.0f ) ) );
}

TEST( FrustumCulling, ABoxEntirelyBehindTheCameraIsCulled )
{
    const Frustum frustum = LookingDownNegativeZ();
    EXPECT_FALSE( frustum.Intersects( UnitBoxAt( glm::vec3( 0.0f, 0.0f, 5000.0f ), 100.0f ) ) );
}

TEST( FrustumCulling, ABoxPastTheFarPlaneIsCulled )
{
    const Frustum frustum = LookingDownNegativeZ();
    EXPECT_FALSE( frustum.Intersects( UnitBoxAt( glm::vec3( 0.0f, 0.0f, -200000.0f ), 100.0f ) ) );
}

TEST( FrustumCulling, ABoxFarOutToTheSideIsCulled )
{
    const Frustum frustum = LookingDownNegativeZ();
    EXPECT_FALSE( frustum.Intersects( UnitBoxAt( glm::vec3( 200000.0f, 0.0f, -5000.0f ), 100.0f ) ) );
}

// ---------------------------------------------------------------------------------------------------
// 2. THE POSITIVE CONTROL, and the reason the point test could not have been used.
//
// A wall the camera stands beside: its CENTRE is outside the left plane and behind the near plane, and
// it crosses the whole view. The point test deletes it; the box test keeps it. Replace Intersects with
// IsInside(centre) anywhere in the render path and this test is what goes red — on a frame, the same
// substitution is invisible from most angles and catastrophic from some.

TEST( FrustumCulling, AWallWhoseCentreIsOutsideTheViewStillCoversIt )
{
    const Frustum frustum = LookingDownNegativeZ();

    // A block of the city that starts just left of the frame and runs 280 m out to the left and 400 m
    // away down -Z. Its near-right corner is well inside the view cone; its CENTRE is 160 m to the left
    // of a view that is only 116 m wide at that depth.
    const AABB      wall{ glm::vec3( -30000.0f, -500.0f, -40000.0f ), glm::vec3( -2000.0f, 500.0f, -100.0f ) };
    const glm::vec3 centre = ( wall.Min + wall.Max ) * 0.5f;

    EXPECT_FALSE( frustum.IsInside( centre ) ) << "the premise of this test: the centre is outside";
    EXPECT_TRUE( frustum.Intersects( wall ) ) << "and the wall is on screen anyway";
}

// ---------------------------------------------------------------------------------------------------
// 3. Eight corners, not two.

TEST( FrustumCulling, RotatedBoundsAreTheBoxOfEightCornersAndNotOfTwo )
{
    // A 100 m x 20 m slab, yawed 135 degrees. THE ANGLE IS THE TEST. Under a yaw between 0 and 90
    // degrees the widest point along X happens to BE the Max corner, so the two-corner shortcut gets the
    // right answer by luck and a test written at 45 degrees passes over a broken implementation. Past 90
    // the cosine turns negative and the extreme moves to (Min.x, Max.z) — a corner the shortcut never
    // looks at. The world-scale scene rotates its buildings in whole multiples of 90 degrees, so this
    // quadrant is not exotic.
    const AABB      local{ glm::vec3( -5000.0f, -10.0f, -1000.0f ), glm::vec3( 5000.0f, 10.0f, 1000.0f ) };
    const glm::mat4 yaw135 = glm::rotate( glm::mat4( 1.0f ), glm::radians( 135.0f ), glm::vec3( 0, 1, 0 ) );

    const AABB eightCorners = Desert::Geometry::TransformBounds( yaw135, local );

    const glm::vec3 minOnly = glm::vec3( yaw135 * glm::vec4( local.Min, 1.0f ) );
    const glm::vec3 maxOnly = glm::vec3( yaw135 * glm::vec4( local.Max, 1.0f ) );
    const AABB      twoCorners{ glm::min( minOnly, maxOnly ), glm::max( minOnly, maxOnly ) };

    // (5000 + 1000) / sqrt(2) = 4242.6 either side of the origin.
    EXPECT_NEAR( eightCorners.Max.x, 4242.6f, 1.0f );
    EXPECT_NEAR( eightCorners.Min.x, -4242.6f, 1.0f );

    // The shortcut's box is 2828.4 either side — a third of the object's real width missing, which is
    // exactly how an object near the screen edge vanishes.
    EXPECT_LT( twoCorners.Max.x, eightCorners.Max.x );
    EXPECT_GT( twoCorners.Min.x, eightCorners.Min.x );
}

TEST( FrustumCulling, ARotatedPlankReachingIntoTheViewIsNotCulled )
{
    const Frustum frustum = LookingDownNegativeZ();

    // Centre well off to the left and deep; yawed so one end swings into the view cone.
    const AABB      local{ glm::vec3( -20000.0f, -100.0f, -100.0f ), glm::vec3( 20000.0f, 100.0f, 100.0f ) };
    const glm::mat4 transform = glm::translate( glm::mat4( 1.0f ), glm::vec3( -22000.0f, 0.0f, -20000.0f ) ) *
                                glm::rotate( glm::mat4( 1.0f ), glm::radians( 45.0f ), glm::vec3( 0, 1, 0 ) );

    EXPECT_TRUE( IsVisibleInView( frustum, transform, local ) );
}

// ---------------------------------------------------------------------------------------------------
// 4. The default is to DRAW.

TEST( FrustumCulling, AMeshWithNoExtentIsDrawnRatherThanCulled )
{
    const Frustum frustum = LookingDownNegativeZ();

    const AABB none = Desert::Geometry::LocalBounds( {} );
    ASSERT_TRUE( Desert::Geometry::IsEmpty( none ) );

    // Placed far behind the camera, where a box WOULD be culled — the answer must still be "draw it",
    // because "I do not know where this is" is not "it is not there".
    const glm::mat4 behindTheCamera = glm::translate( glm::mat4( 1.0f ), glm::vec3( 0.0f, 0.0f, 500000.0f ) );
    EXPECT_TRUE( IsVisibleInView( frustum, behindTheCamera, none ) );
}

TEST( FrustumCulling, LocalBoundsAreTheUnionOfTheSubmeshBoxes )
{
    std::vector<Desert::Submesh> submeshes( 2 );
    submeshes[0].BoundingBox = AABB{ glm::vec3( -1.0f ), glm::vec3( 1.0f ) };
    submeshes[1].BoundingBox = AABB{ glm::vec3( 0.0f, 0.0f, 0.0f ), glm::vec3( 7.0f, 2.0f, 3.0f ) };

    const AABB box = Desert::Geometry::LocalBounds( submeshes );
    EXPECT_FLOAT_EQ( box.Min.x, -1.0f );
    EXPECT_FLOAT_EQ( box.Max.x, 7.0f );
    EXPECT_FLOAT_EQ( box.Max.y, 2.0f );
    EXPECT_FLOAT_EQ( box.Max.z, 3.0f );
}

// ---------------------------------------------------------------------------------------------------
// 5. EVERY PASS CULLS WITH THE MATRIX IT DRAWS WITH, OR IT DOES NOT CULL.
//
// A register with one named row per pass, and the count derived from the register rather than pinned:
// a gate pinning a NUMBER can be satisfied by editing the number.
//
// The queues are read by five passes and three of them rasterize from somewhere other than the camera.
// An object behind the camera casts a shadow INTO the frame it is not in, so culling a sun pass by the
// camera's frustum buys draw calls with missing shadows — and the draw-call detector, which counts
// submissions and not pixels, would report that as a win. This is the assertion that makes the pairing
// of pass to frustum a property of the tree instead of a thing somebody remembered.

TEST( FrustumCulling, EveryPassCullsWithTheMatrixItDrawsWith )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const std::string source = ReadFile( root / "Desert" / "Desert" / "Source" / "Engine" / "Graphic" / "Systems" /
                                         "Scene" / "Mesh" / "MeshRenderer.cpp" );
    ASSERT_FALSE( source.empty() );

    struct Row
    {
        const char* Function;
        bool        Culls;
        const char* CullsWith; ///< the expression the frustum is built from; nullptr when it must not cull
        const char* Why;
    };

    const Row rows[] = {
         { "DrawStaticMeshes", true, "camera->GetFrustum()", "opaque PBR pass — rasterizes from the camera" },
         { "RenderGlassManual", true, "camera->GetFrustum()", "transparent pass — rasterizes from the camera" },
         { "DrawGenericMeshes", true, "camera->GetFrustum()",
           "data-driven / shader-graph surfaces — rasterizes from the camera. Sound only while no vertex "
           "stage moves a vertex off the authored box, which the next test asserts" },
         { "RenderOverdrawManual", true, "camera->GetFrustum()",
           "debug view OF the camera pass; it must report the frame that actually runs" },
         { "RegisterShadowPass", true, "m_CascadeVP[c]",
           "rasterizes from the SUN: the camera's frustum here would delete off-screen casters whose "
           "shadows land on screen. Its own cascade matrix is a finite ortho box, so this skips only "
           "what the rasterizer already clips" },
         { "RenderRSMManual", false, nullptr,
           "rasterizes from the SUN into the RSM, and every one of the 114 scenes in the tree is "
           "GlobalIllumination=ScreenSpace, so this pass never runs: a change here could not be seen on "
           "any frame this repository can take, and an unobservable change is not shipped" },
    };

    for ( const auto& row : rows )
    {
        const std::string body = BodyOf( source, row.Function );
        ASSERT_FALSE( body.empty() ) << "MeshRenderer::" << row.Function << " was renamed or removed";

        const bool culls = body.find( "IsVisibleInView(" ) != std::string::npos;
        EXPECT_EQ( culls, row.Culls ) << "MeshRenderer::" << row.Function << " — " << row.Why;

        if ( row.CullsWith != nullptr )
        {
            EXPECT_NE( body.find( row.CullsWith ), std::string::npos )
                 << "MeshRenderer::" << row.Function << " must build its frustum from " << row.CullsWith << " — "
                 << row.Why;
        }

        // And the sun passes must never reach for the camera's frustum, which is the substitution that
        // reads as an optimization and ships as a frame with holes where shadows were.
        if ( row.CullsWith == nullptr || std::string( row.CullsWith ) != "camera->GetFrustum()" )
        {
            EXPECT_EQ( body.find( "camera->GetFrustum()" ), std::string::npos )
                 << "MeshRenderer::" << row.Function << " culls from the camera — " << row.Why;
        }
    }
}

// The cascades are STANDARD-Z while the camera is reversed-Z, and the frustum's near/far planes are the
// only two derived from the depth convention. Swapping the convention swaps which derived plane is
// called "near" — the SET of six half-spaces is the same, and a box test that reads all six is correct
// under both. That is why one Frustum serves the camera and the cascades; assert it rather than trust it.
TEST( FrustumCulling, TheBoxTestHoldsForTheCascadesStandardZOrthographicMatrix )
{
    // A cascade, spelled as ShadowCascades.hpp spells it: ortho box of half-width `radius` about the
    // origin, depth from 10 cm to 4 * radius along the light, standard-Z.
    const float     radius = 10000.0f;
    const glm::mat4 proj   = glm::orthoRH_ZO( -radius, radius, -radius, radius, 10.0f, radius * 4.0f );
    const glm::mat4 view =
         glm::lookAt( glm::vec3( 0.0f, radius * 2.0f, 0.0f ), glm::vec3( 0.0f ), glm::vec3( 0, 0, 1 ) );

    const Frustum cascade( proj, view );

    EXPECT_TRUE( cascade.Intersects( UnitBoxAt( glm::vec3( 0.0f ), 100.0f ) ) )
         << "the slice's own centre must rasterize";
    EXPECT_FALSE( cascade.Intersects( UnitBoxAt( glm::vec3( radius * 5.0f, 0.0f, 0.0f ), 100.0f ) ) )
         << "far outside the ortho box on X — the rasterizer clips it today";
    EXPECT_FALSE( cascade.Intersects( UnitBoxAt( glm::vec3( 0.0f, -radius * 5.0f, 0.0f ), 100.0f ) ) )
         << "past the far plane along the light — the rasterizer clips it today";
}

// ---------------------------------------------------------------------------------------------------
// 6. NOTHING IN THIS TREE MOVES A VERTEX OFF THE BOX THE CULLER TESTS.
//
// Culling a data-driven surface on its authored bounds is only sound while its vertex stage transforms
// `a_Position` and nothing else. A world-position offset — the feature UE ships a per-material "bounds
// scale" knob to compensate for — would put geometry outside a box that says it is elsewhere, and the
// object would vanish from some camera angles and not others. Nothing reports that; it is not a crash,
// a warning or a failed test, it is a hole in the picture.
//
// So it is asserted over the SOURCE TEXT of every Surface-domain shader, because that is the set the
// generic queue can bind. The day a vertex-offset node exists this goes red FIRST, naming the shader,
// and whoever adds it has to decide what the bounds of a displaced mesh are before the culler can be
// trusted with it. The register names the set by a DERIVED rule (Domain Surface), never by a count.

TEST( FrustumCulling, NoSurfaceShaderMovesAVertexOffItsAuthoredBounds )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const fs::path shaderRoot = root / "Editor" / "Resources" / "Shaders";
    ASSERT_TRUE( fs::exists( shaderRoot ) );

    // Whitespace removed, so the assertion is about the EXPRESSION and not about formatting — a
    // clang-format pass must not be able to turn this red.
    const auto squash = []( std::string text )
    {
        text.erase(
             std::remove_if( text.begin(), text.end(), []( unsigned char c ) { return std::isspace( c ) != 0; } ),
             text.end() );
        return text;
    };

    int checked = 0;
    for ( const auto& entry : fs::recursive_directory_iterator( shaderRoot ) )
    {
        if ( !entry.is_regular_file() || entry.path().extension() != ".shader" )
        {
            continue;
        }
        const std::string text = ReadFile( entry.path() );
        if ( text.find( "Domain Surface" ) == std::string::npos )
        {
            continue; // Skybox / UI / Volume / Terrain are not drawn through the mesh queues
        }

        // The graph shaders declare no vertex body of their own; they include the one shared stage.
        // Follow it, so the assertion lands on the code that runs rather than on the include line.
        std::string vertexSource = text;
        if ( text.find( "#include <Common/GraphVertex.glslh>" ) != std::string::npos )
        {
            vertexSource = ReadFile( shaderRoot / "Common" / "GraphVertex.glslh" );
        }

        const std::size_t at = vertexSource.find( "gl_Position =" );
        ASSERT_NE( at, std::string::npos ) << entry.path().string() << " has no gl_Position";
        const std::size_t semicolon = vertexSource.find( ';', at );
        ASSERT_NE( semicolon, std::string::npos ) << entry.path().string();

        const std::string rhs         = squash( vertexSource.substr( at + 13, semicolon - at - 13 ) );
        const std::string undisplaced = "*vec4(a_Position,1.0)";
        ASSERT_GE( rhs.size(), undisplaced.size() ) << entry.path().string();
        EXPECT_EQ( rhs.substr( rhs.size() - undisplaced.size() ), undisplaced )
             << entry.path().string()
             << ": its vertex stage computes gl_Position from something other than the mesh's own "
                "a_Position. MeshRenderer culls this shader's draws on the mesh's authored bounding "
                "box, and a displaced vertex is outside a box that claims otherwise. Either the "
                "displacement must be bounded and the bounds widened to cover it, or this shader must "
                "be excluded from bounds culling — decide which, then update this register.";
        ++checked;
    }

    // Derived from the register above, not pinned: a number here could be satisfied by editing it.
    EXPECT_GT( checked, 0 ) << "no Surface-domain shader was found — the search is looking in the "
                               "wrong place, and a census that examines nothing passes silently";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
