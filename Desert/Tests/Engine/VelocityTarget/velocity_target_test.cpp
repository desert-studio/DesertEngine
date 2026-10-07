// TAA1 step 4 — the velocity target: its format has one spelling, the per-draw motion record has a GLSL twin of
// the same std430 layout, and the velocity the geometry passes write is current minus previous unjittered NDC
// (CPU twin Graphic::VelocityNdc, GLSL DesertVelocity in Common/ObjectMotion.glslh).

#include <Engine/Core/Projection.hpp>
#include <Engine/Graphic/View/SceneViewState.hpp>
#include <Engine/Graphic/View/Velocity.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>

#include "../../TestSupport/scratch_dir.hpp"

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace VelocityTargetTest
{
    using namespace Desert::Graphic;

    std::string ReadFile( const std::filesystem::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    // The member list of `struct <name> { ... };` in a GLSL source, as (type, name) in declaration order.
    std::vector<std::pair<std::string, std::string>> GlslStructMembers( const std::string& source,
                                                                        const std::string& name )
    {
        std::vector<std::pair<std::string, std::string>> members;
        const std::regex                                 head( "struct\\s+" + name + "\\s*\\{([^}]*)\\}" );
        std::smatch                                      match;
        if ( !std::regex_search( source, match, head ) )
            return members;
        const std::string body = match[1].str();
        const std::regex  member( "(\\w+)\\s+(\\w+)\\s*;" );
        for ( auto it = std::sregex_iterator( body.begin(), body.end(), member ); it != std::sregex_iterator();
              ++it )
            members.emplace_back( ( *it )[1].str(), ( *it )[2].str() );
        return members;
    }

    // std430 size of the scalar/matrix types the record may hold.
    std::size_t Std430Size( const std::string& type )
    {
        if ( type == "mat4" )
            return 64;
        if ( type == "vec4" )
            return 16;
        if ( type == "uint" || type == "float" || type == "int" )
            return 4;
        return 0; // unknown: the census fails on it by name
    }

    constexpr float kTolerance = 1e-5f;

    glm::mat4 Camera( const glm::vec3& eye )
    {
        return Desert::Core::MakePerspective( glm::radians( 60.0f ), 16.0f / 9.0f, 10.0f, 100000.0f ) *
               glm::lookAt( eye, eye + glm::vec3( 0, 0, -1 ), glm::vec3( 0, 1, 0 ) );
    }
} // namespace VelocityTargetTest

using namespace VelocityTargetTest;

TEST( VelocityTarget, OneSpellingOfTheFormat )
{
    EXPECT_EQ( kVelocityFormat, ViewTargetFormats::kVelocity );
    EXPECT_EQ( ViewTargetFormats::kVelocity, Desert::Core::Formats::ImageFormat::RG16F );
}

// Mutation: reorder World/PrevWorld (or add a member) in Common/ObjectMotion.glslh -> red.
TEST( VelocityTarget, GpuObjectMotionHasAStd430TwinInGlsl )
{
    const std::string source =
         ReadFile( Desert::TestSupport::RepositoryRoot() / "Editor/Resources/Shaders/Common/ObjectMotion.glslh" );
    ASSERT_FALSE( source.empty() );
    const auto members = GlslStructMembers( source, "GpuObjectMotion" );
    ASSERT_EQ( members.size(), 2u );
    EXPECT_EQ( members[0].second, "World" );
    EXPECT_EQ( members[1].second, "PrevWorld" );

    std::size_t offset = 0;
    std::size_t prevAt = 0;
    for ( const auto& [type, name] : members )
    {
        const std::size_t size = Std430Size( type );
        ASSERT_NE( size, 0u ) << "unknown GLSL type " << type << " of " << name;
        if ( name == "PrevWorld" )
            prevAt = offset;
        offset += size;
    }
    EXPECT_EQ( offset, sizeof( GpuObjectMotion ) );
    EXPECT_EQ( prevAt, offsetof( GpuObjectMotion, PrevWorld ) );
}

// Mutation: DesertVelocity / VelocityNdc swap the operands or drop the w-divide -> red (the GLSL check reads the
// expression; the numeric checks pin the CPU twin).
TEST( VelocityTarget, GlslVelocityIsTheCpuTwinExpression )
{
    const std::string source =
         ReadFile( Desert::TestSupport::RepositoryRoot() / "Editor/Resources/Shaders/Common/ObjectMotion.glslh" );
    EXPECT_NE( source.find( "return clip.xy / clip.w - prevClip.xy / prevClip.w;" ), std::string::npos );
}

TEST( VelocityTarget, StillCameraOverAStillObjectIsZero )
{
    const glm::mat4 viewProj = Camera( glm::vec3( 0, 0, 500 ) );
    const glm::mat4 world    = glm::translate( glm::mat4( 1.0f ), glm::vec3( 30, -20, 0 ) );
    const glm::vec4 p( 5, 7, -3, 1 );
    const glm::vec2 v = VelocityNdc( viewProj * world * p, viewProj * world * p );
    EXPECT_EQ( v, glm::vec2( 0.0f ) );
}

TEST( VelocityTarget, IsCurrentMinusPreviousNdc )
{
    const glm::mat4 viewProj = Camera( glm::vec3( 0, 0, 500 ) );
    const glm::mat4 prev     = glm::translate( glm::mat4( 1.0f ), glm::vec3( 0, 0, 0 ) );
    const glm::mat4 cur      = glm::translate( glm::mat4( 1.0f ), glm::vec3( 10, 0, 0 ) ); // 10 cm to +x
    const glm::vec4 p( 0, 0, 0, 1 );
    const glm::vec4 c  = viewProj * cur * p;
    const glm::vec4 pc = viewProj * prev * p;
    const glm::vec2 v  = VelocityNdc( c, pc );
    EXPECT_GT( v.x, 0.0f ); // moved right on screen
    EXPECT_NEAR( v.y, 0.0f, kTolerance );
    EXPECT_NEAR( v.x, c.x / c.w - pc.x / pc.w, kTolerance );
}

// The relation the geometry passes rely on: a draw the view did not draw last frame gets PrevWorld = World from
// MotionHistory, so its velocity is the camera's alone — zero under a still camera even though the object
// "jumped" from wherever it was before it vanished.
// Mutation: MotionHistory keeps the transform of an object not drawn for a frame -> red.
TEST( VelocityTarget, AnObjectNotDrawnLastFrameMovesOnlyWithTheCamera )
{
    MotionHistory   motion;
    const MotionKey key{ 7, 0 };
    const glm::mat4 before = glm::translate( glm::mat4( 1.0f ), glm::vec3( -400, 0, 0 ) );
    const glm::mat4 now    = glm::translate( glm::mat4( 1.0f ), glm::vec3( 50, 0, 0 ) );
    (void)motion.PreviousTransform( key, before );
    motion.EndFrame();
    motion.EndFrame(); // a frame without the object

    GpuObjectMotion record;
    record.World     = now;
    record.PrevWorld = motion.PreviousTransform( key, now );

    const glm::mat4 viewProj = Camera( glm::vec3( 0, 0, 500 ) );
    const glm::vec4 p( 1, 2, 3, 1 );
    const glm::vec2 v = VelocityNdc( viewProj * record.World * p, viewProj * record.PrevWorld * p );
    EXPECT_NEAR( v.x, 0.0f, kTolerance );
    EXPECT_NEAR( v.y, 0.0f, kTolerance );

    // And a camera move alone gives a non-zero velocity for that same still object.
    const glm::mat4 prevViewProj = Camera( glm::vec3( -10, 0, 500 ) );
    const glm::vec2 camera       = VelocityNdc( viewProj * record.World * p, prevViewProj * record.PrevWorld * p );
    EXPECT_GT( glm::length( camera ), 1e-4f );
}
