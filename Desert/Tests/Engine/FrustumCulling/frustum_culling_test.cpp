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
        const glm::mat4 view       = glm::lookAt( glm::vec3( 0.0f ), glm::vec3( 0.0f, 0.0f, -1.0f ),
                                                  glm::vec3( 0.0f, 1.0f, 0.0f ) );
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
        std::ifstream     in( file );
        std::stringstream ss;
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
        const std::size_t end = source.find( "\n    void MeshRenderer::", begin + opener.size() );
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

    // 400 m long, 20 m thick, starting beside the camera and running away down -Z.
    const AABB wall{ glm::vec3( -3000.0f, -500.0f, -40000.0f ), glm::vec3( -2000.0f, 500.0f, 0.0f ) };
    const glm::vec3 centre = ( wall.Min + wall.Max ) * 0.5f;

    EXPECT_FALSE( frustum.IsInside( centre ) ) << "the premise of this test: the centre is outside";
    EXPECT_TRUE( frustum.Intersects( wall ) ) << "and the wall is on screen anyway";
}

// ---------------------------------------------------------------------------------------------------
// 3. Eight corners, not two.

TEST( FrustumCulling, RotatedBoundsAreTheBoxOfEightCornersAndNotOfTwo )
{
    // A long thin plank, yawed 45 degrees. The two-corner shortcut maps Min and Max onto a pair of
    // points that no longer bracket the object: on a 45-degree yaw the plank's extent along X is
    // ~length/sqrt(2), while Min.x and Max.x map to points a fraction of that apart.
    const AABB      local{ glm::vec3( -5000.0f, -10.0f, -10.0f ), glm::vec3( 5000.0f, 10.0f, 10.0f ) };
    const glm::mat4 yaw45 = glm::rotate( glm::mat4( 1.0f ), glm::radians( 45.0f ), glm::vec3( 0, 1, 0 ) );

    const AABB eightCorners = Desert::Geometry::TransformBounds( yaw45, local );

    const glm::vec3 minOnly = glm::vec3( yaw45 * glm::vec4( local.Min, 1.0f ) );
    const glm::vec3 maxOnly = glm::vec3( yaw45 * glm::vec4( local.Max, 1.0f ) );
    const AABB twoCorners{ glm::min( minOnly, maxOnly ), glm::max( minOnly, maxOnly ) };

    // The real extent along X is half the plank's diagonal projected onto X, ~3536 either side.
    EXPECT_GT( eightCorners.Max.x, 3500.0f );
    EXPECT_LT( eightCorners.Min.x, -3500.0f );

    // The shortcut's box is strictly smaller on X, which is how an object near the screen edge vanishes.
    EXPECT_LT( twoCorners.Max.x, eightCorners.Max.x );
    EXPECT_GT( twoCorners.Min.x, eightCorners.Min.x );
}

TEST( FrustumCulling, ARotatedPlankReachingIntoTheViewIsNotCulled )
{
    const Frustum frustum = LookingDownNegativeZ();

    // Centre well off to the left and deep; yawed so one end swings into the view cone.
    const AABB      local{ glm::vec3( -20000.0f, -100.0f, -100.0f ), glm::vec3( 20000.0f, 100.0f, 100.0f ) };
    const glm::mat4 transform =
         glm::translate( glm::mat4( 1.0f ), glm::vec3( -22000.0f, 0.0f, -20000.0f ) ) *
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
    const glm::mat4 behindTheCamera =
         glm::translate( glm::mat4( 1.0f ), glm::vec3( 0.0f, 0.0f, 500000.0f ) );
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
// 5. WHICH PASSES MAY BE CULLED BY THE CAMERA — a register with one named row per pass, never a count.
//
// The queues are read by five passes and three of them rasterize from somewhere else. An object behind
// the camera casts a shadow INTO the frame it is not in, so culling a sun pass by the camera's frustum
// buys draw calls with missing shadows — and the draw-call detector would report that as a win.

TEST( FrustumCulling, TheCameraPassesCullAndTheLightPassesDoNot )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const std::string source =
         ReadFile( root / "Desert" / "Desert" / "Source" / "Engine" / "Graphic" / "Systems" / "Scene" /
                   "Mesh" / "MeshRenderer.cpp" );
    ASSERT_FALSE( source.empty() );

    struct Row
    {
        const char* Function;
        bool        CulledByTheCamera;
        const char* Why;
    };

    const Row rows[] = {
        { "DrawStaticMeshes", true, "the opaque PBR pass — rasterizes from the camera" },
        { "RenderGlassManual", true, "transparent pass — rasterizes from the camera" },
        { "RenderOverdrawManual", true, "debug view of the camera pass; must show the frame that runs" },
        { "RegisterShadowPass", false, "rasterizes from the SUN: off-screen casters shadow on-screen ground" },
        { "RenderRSMManual", false, "rasterizes from the SUN: off-screen surfaces are the bounce light" },
    };

    for ( const auto& row : rows )
    {
        const std::string body = BodyOf( source, row.Function );
        ASSERT_FALSE( body.empty() ) << "MeshRenderer::" << row.Function << " was renamed or removed";
        const bool culls = body.find( "IsVisibleInView(" ) != std::string::npos;
        EXPECT_EQ( culls, row.CulledByTheCamera )
             << "MeshRenderer::" << row.Function << " — " << row.Why;
    }
}
