// TAA1 step 4 — the velocity target: its format has one spelling, the per-draw motion record has a GLSL twin of
// the same std430 layout, and the velocity the geometry passes write is current minus previous unjittered NDC
// (CPU twin Graphic::VelocityNdc, GLSL DesertVelocity in Common/ObjectMotion.glslh).

#include <Engine/Core/Projection.hpp>
#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Engine/Core/ShaderCompiler/ShaderGraphBindings.hpp>
#include <Engine/Graphic/InstanceWind.hpp>
#include <Engine/Graphic/View/SceneViewState.hpp>
#include <Engine/Graphic/View/Velocity.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>

#include "../../TestSupport/scratch_dir.hpp"

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
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
    ASSERT_EQ( members.size(), 6u ); // World, PrevWorld, BoneOffset, PrevBoneOffset, Pad0, Pad1
    EXPECT_EQ( members[0].second, "World" );
    EXPECT_EQ( members[1].second, "PrevWorld" );
    EXPECT_EQ( members[2].second, "BoneOffset" );
    EXPECT_EQ( members[3].second, "PrevBoneOffset" );

    std::size_t offset     = 0;
    std::size_t prevAt     = 0;
    std::size_t boneAt     = 0;
    std::size_t prevBoneAt = 0;
    for ( const auto& [type, name] : members )
    {
        const std::size_t size = Std430Size( type );
        ASSERT_NE( size, 0u ) << "unknown GLSL type " << type << " of " << name;
        if ( name == "PrevWorld" )
            prevAt = offset;
        if ( name == "BoneOffset" )
            boneAt = offset;
        if ( name == "PrevBoneOffset" )
            prevBoneAt = offset;
        offset += size;
    }
    EXPECT_EQ( offset, sizeof( GpuObjectMotion ) );
    EXPECT_EQ( prevAt, offsetof( GpuObjectMotion, PrevWorld ) );
    EXPECT_EQ( boneAt, offsetof( GpuObjectMotion, BoneOffset ) );
    EXPECT_EQ( prevBoneAt, offsetof( GpuObjectMotion, PrevBoneOffset ) );
}

// The view-pass vertex contract: the static and skinned surface templates read their world from the object's
// motion row and hand the fragment stage both unjittered clip positions; the G-buffer pass writes velocity at
// slot 4 except in its RSM permutation; the forward opaque pass writes it at slot 1; the translucent pass never.
// Mutation: drop `row.PrevWorld` from a template / write oVelocity in the RSM permutation / give the translucent
// pass a velocity output -> red.
TEST( VelocityTarget, ViewPassSurfaceStagesWriteTheVelocityOfTheirOwnSurface )
{
    const auto root   = Desert::TestSupport::RepositoryRoot() / "Editor/Resources/Shaders/Mesh/Surface";
    const auto noWs   = []( std::string s )
    {
        s.erase( std::remove_if( s.begin(), s.end(), []( unsigned char c ) { return std::isspace( c ) != 0; } ),
                 s.end() );
        return s;
    };
    for ( const char* vertex : { "Vertex_Static.glslh", "Vertex_Skinned.glslh" } )
    {
        const std::string src = noWs( ReadFile( root / vertex ) );
        ASSERT_FALSE( src.empty() ) << vertex;
        EXPECT_NE( src.find( "objectMotions.Motions[m_PushConstants.PrimitiveIndex]" ), std::string::npos ) << vertex;
        EXPECT_NE( src.find( "row.World*m_PushConstants.Transform" ), std::string::npos ) << vertex;
        EXPECT_NE( src.find( "row.PrevWorld*m_PushConstants.Transform" ), std::string::npos ) << vertex;
        EXPECT_NE( src.find( "v_Surface.Clip=cameraUB.ViewProjection*" ), std::string::npos ) << vertex;
        EXPECT_NE( src.find( "v_Surface.PrevClip=cameraUB.PrevViewProjection*" ), std::string::npos ) << vertex;
    }
    // Skinned view pass: BOTH palettes from the view's one ObjectBones buffer at the row's two offsets (lead
    // decision: the row names them, so every pass drawing the primitive skins it from the same bytes).
    // Mutation: skin the view pass from the push BoneOffset / the material's Bones buffer -> red.
    {
        const std::string skinned = noWs( ReadFile( root / "Vertex_Skinned.glslh" ) );
        EXPECT_NE( skinned.find( "constintb=int(row.BoneOffset);" ), std::string::npos );
        EXPECT_NE( skinned.find( "constintpb=int(row.PrevBoneOffset);" ), std::string::npos );
        EXPECT_NE( skinned.find( "#defineDESERT_SKIN_MATRIX(base,i)objectBones.Palettes[(base)+(i)]" ),
                   std::string::npos );
    }

    const std::string gbuffer = noWs( ReadFile( root / "Pass_GBuffer.glslh" ) );
    const auto        gate    = gbuffer.find( "#ifndefDESERT_GBUFFER_RSM" );
    const auto        output  = gbuffer.find( "layout(location=4)outvec2oVelocity;" );
    ASSERT_NE( gate, std::string::npos );
    ASSERT_NE( output, std::string::npos );
    EXPECT_LT( gate, output );
    EXPECT_EQ( gbuffer.find( "#endif", gate ) > output, true ) << "oVelocity sits outside the RSM gate";

    const std::string forward = noWs( ReadFile( root / "Pass_Forward.glslh" ) );
    EXPECT_NE( forward.find( "layout(location=1)outvec2oVelocity;" ), std::string::npos );
    EXPECT_NE( forward.find( "oVelocity=DesertVelocity(v_Surface.Clip,v_Surface.PrevClip);" ), std::string::npos );
    EXPECT_EQ( ReadFile( root / "Pass_Forward_Translucent.glslh" ).find( "oVelocity" ), std::string::npos );
}

// Mutation: DesertVelocity / VelocityNdc swap the operands or drop the w-divide -> red (the GLSL check reads the
// expression; the numeric checks pin the CPU twin).
namespace VelocityTargetTest
{
    // A minimal surface program whose Properties block is @p properties; nothing else in it can fail to parse.
    std::string ProbeWithProperties( const std::string& properties )
    {
        return "Shader \"SceneReadRangeProbe\"\n{\n    Domain Surface\n\n    " + properties +
               "\n    State { Cull Back ZTest LEqual ZWrite On }\n"
               "    Vertex\n    {\n        In(0) vec3 a_Position;\n"
               "        void main() { gl_Position = vec4( a_Position, 1.0 ); }\n    }\n"
               "    Fragment\n    {\n        Out(0) vec4 o_Color;\n"
               "        void main() { o_Color = vec4( 1.0 ); }\n    }\n}\n";
    }
} // namespace VelocityTargetTest

// Lead decision (TAA1-VEL-d): the scene-read slots are FIXED numbers, so they need a guarantee and not a hope.
// Seam 1, the material layout: a Properties row or a run of texture properties that reaches into the reserved
// range is a named parse error. The control parses the same block one run higher, so the refusal is about the
// range and not about the probe.
// Mutation: delete the IsSceneReadBinding refusal in DShaderParser::Parse -> red.
TEST( VelocityTarget, AMaterialRowOrTextureCannotBeNumberedIntoTheSceneReadRange )
{
    using Desert::Core::Preprocess::DShaderParser;
    using Desert::Core::kSceneReadBindingFirst;

    const auto textures = []( uint32_t first )
    {
        return "Properties Binding(1) TextureBinding(" + std::to_string( first ) +
               ")\n    {\n        Color Tint (\"Tint\") = (1, 1, 1, 1)\n"
               "        Texture2D u_First (\"First\")\n        Texture2D u_Second (\"Second\")\n    }\n";
    };
    // Two textures from one below the range: the second lands on its first slot.
    const auto intoRange = DShaderParser::Parse( ProbeWithProperties( textures( kSceneReadBindingFirst - 1u ) ) );
    ASSERT_FALSE( intoRange.IsSuccess() );
    EXPECT_NE( intoRange.GetError().find( "scene-read" ), std::string::npos ) << intoRange.GetError();
    EXPECT_NE( intoRange.GetError().find( "u_Second" ), std::string::npos ) << intoRange.GetError();

    const auto row = DShaderParser::Parse( ProbeWithProperties(
         "Properties Binding(" + std::to_string( kSceneReadBindingFirst ) +
         ")\n    {\n        Color Tint (\"Tint\") = (1, 1, 1, 1)\n    }\n" ) );
    ASSERT_FALSE( row.IsSuccess() );
    EXPECT_NE( row.GetError().find( "scene-read" ), std::string::npos ) << row.GetError();

    const auto above = DShaderParser::Parse( ProbeWithProperties(
         textures( Desert::Core::kSceneReadBindingFirst + Desert::Core::kSceneReadBindingCount ) ) );
    EXPECT_TRUE( above.IsSuccess() ) << ( above.IsSuccess() ? "" : above.GetError() );
}

// Seam 2, the hand-numbered declarations (a template's own `layout( binding = n )` textures, a pass header's
// lighting slots — none of which goes through the Properties block): over the WHOLE shipped shader tree, the
// reserved numbers carry the two scene-read resources and nothing else, and ObjectMotion.glslh spells exactly the
// C++ numbers PBRSceneFrame's resources are reserved under. Text, not reflection, because a resource that only
// collides inside one cell would need that cell compiled to be seen; the after-compile half is
// ShaderReflection::ReflectStage, which refuses a slot claimed twice by name.
// Mutation: move SpotLightsUB (or any texture) to 25 / spell ObjectMotions at 16 (where SpotLightsUB lives) -> red.
TEST( VelocityTarget, OnlyTheSceneReadResourcesSitInTheReservedRange )
{
    const auto shaders = Desert::TestSupport::RepositoryRoot() / "Editor/Resources/Shaders";
    const std::regex declaration(
         R"(binding\s*=\s*(\d+)\s*\)\s*(?:readonly\s+|writeonly\s+)?(?:uniform|buffer)\s+(?:\w+\s+)?(\w+))"
         R"(|\b(?:Uniform|Buffer|ReadBuffer|WriteBuffer)\s*\(\s*(\d+)\s*\)\s*(?:\w+\s+)?(\w+))" );

    std::map<std::string, uint32_t> sceneRead; // resource -> number, from ObjectMotion.glslh
    std::size_t                     files = 0;
    for ( const auto& entry : std::filesystem::recursive_directory_iterator( shaders ) )
    {
        const auto ext = entry.path().extension().string();
        if ( !entry.is_regular_file() || ( ext != ".shader" && ext != ".glslh" && ext != ".glsl" ) )
            continue;
        ++files;
        const std::string text = ReadFile( entry.path() );
        for ( std::sregex_iterator it( text.begin(), text.end(), declaration ), end; it != end; ++it )
        {
            const auto&       m       = *it;
            const uint32_t    binding = static_cast<uint32_t>( std::stoul( m[1].matched ? m[1].str() : m[3].str() ) );
            const std::string name    = m[2].matched ? m[2].str() : m[4].str();
            if ( entry.path().filename() == "ObjectMotion.glslh" )
                sceneRead[name] = binding;
            else
                EXPECT_FALSE( Desert::Core::IsSceneReadBinding( binding ) )
                     << entry.path().string() << " declares '" << name << "' at binding " << binding
                     << ", inside the reserved scene-read range — it would share a slot with the view's motion "
                        "rows in every view-pass cell";
        }
    }
    EXPECT_GT( files, 100u ) << "the walk did not find the shader tree";
    ASSERT_EQ( sceneRead.size(), 2u );
    EXPECT_EQ( sceneRead["ObjectMotions"], Desert::Core::kObjectMotionsBinding );
    EXPECT_EQ( sceneRead["ObjectBones"], Desert::Core::kObjectBonesBinding );
}

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

// The owning entity is the MotionHistory key of a draw's previous transform, and it crosses three hops before the
// geometry pass reads it: the ECS emplace names it, the command's Execute forwards it, SceneRenderer copies it onto
// the render data. A hop that drops it keys every object of the scene under one id — each draw then reads another
// object's previous transform, which is a wrong velocity with no error anywhere. Mutation that turns this red:
// remove `Entity` from any command's Execute, or pass anything but `entity` first in an emplace.
TEST( VelocityTarget, EveryMeshDrawCarriesItsOwningEntityToTheRenderData )
{
    using VelocityTargetTest::ReadFile;
    const auto root     = Desert::TestSupport::RepositoryRoot();
    const auto commands = root / "Desert/Desert/Source/Engine/Graphic/Render/Commands";

    struct Hop
    {
        const char* File;
        const char* Forward; // the Execute call that must carry Entity
    };
    const Hop hops[] = {
         { "DrawMeshCommand.hpp", R"(\.Entity\s*=\s*Entity)" },
         { "DrawSkinnedMeshCommand.hpp", R"(\.Entity\s*=\s*Entity)" },
         { "DrawSlotMaterialMeshCommand.hpp", R"(SubmitSlotMaterialMesh\(\s*Entity\s*,)" },
         { "DrawGenericMeshCommand.hpp", R"(SubmitGenericMesh\(\s*Entity\s*,)" },
    };
    for ( const auto& hop : hops )
    {
        const std::string src = ReadFile( commands / hop.File );
        ASSERT_FALSE( src.empty() ) << hop.File;
        EXPECT_TRUE( std::regex_search( src, std::regex( hop.Forward ) ) )
             << hop.File << ": Execute no longer forwards the owning entity";
    }

    for ( const char* producer : { "Desert/Desert/Source/Engine/ECS/System/MeshECSSystem.hpp",
                                   "Desert/Desert/Source/Engine/ECS/System/TextECSSystem.hpp" } )
    {
        const std::string src = ReadFile( root / producer );
        ASSERT_FALSE( src.empty() ) << producer;
        const std::regex emplace(
             R"(Emplace<Graphic::Render::Draw(Static|Skinned|SlotMaterial|Generic)Mesh(Command)?>\(\s*([^,]*),)" );
        int seen = 0;
        for ( auto it = std::sregex_iterator( src.begin(), src.end(), emplace ); it != std::sregex_iterator(); ++it )
        {
            ++seen;
            EXPECT_EQ( ( *it )[3].str(), "static_cast<uint32_t>( entity )" )
                 << producer << ": a mesh draw is emplaced without its owning entity first";
        }
        EXPECT_GT( seen, 0 ) << producer << ": no mesh draw emplace found — the census lost its subject";
    }

    const std::string scene = ReadFile( root / "Desert/Desert/Source/Engine/Graphic/SceneRenderer.cpp" );
    EXPECT_TRUE( std::regex_search( scene, std::regex( R"(\.Entity\s*=\s*extra\.Entity)" ) ) )
         << "SceneRenderer::SubmitMesh drops RenderSubmissionExtra::Entity";
    EXPECT_EQ( std::distance( std::sregex_iterator( scene.begin(), scene.end(),
                                                    std::regex( R"(\.Entity\s*=\s*entity\b)" ) ),
                              std::sregex_iterator() ),
               2 )
         << "SubmitGenericMesh / SubmitSlotMaterialMesh must both copy the entity onto GenericMeshRenderData";
}

// Step C: the instanced view pass evaluates the wind at the view's previous frame (Vertex_Instanced reads WindB.z);
// the renderer writes it per view. Mutation: drop the B.z write in PackViewInstanceWind -> red; the light-view
// pack writing a previous time -> red.
TEST( VelocityTarget, InstancedViewPushCarriesTheWindAtThePreviousFrame )
{
    using namespace Desert::Graphic;
    const InstanceWind wind = MakeInstanceWind( 30.0f, 0.5f, 200.0f, 45.0f, 7.25 );
    ASSERT_TRUE( wind.Sways() );
    const InstanceWindPush view = PackViewInstanceWind( wind, 7.0 );
    EXPECT_FLOAT_EQ( view.B.y, wind.Seconds );
    EXPECT_FLOAT_EQ( view.B.z, WindSecondsAt( wind.Speed, 7.0 ) );
    EXPECT_NE( view.B.z, view.B.y ) << "a different previous time must give a different previous wind clock";
    EXPECT_EQ( PackInstanceWind( wind ).B.z, 0.0f ) << "a light view has no velocity and no previous wind";
    // Wrapped like the current clock: a previous time one whole period later is the same clock.
    const double period = static_cast<double>( kWindPeriodCycles ) / static_cast<double>( wind.Speed );
    EXPECT_NEAR( PackViewInstanceWind( wind, 7.0 + period ).B.z, view.B.z, 1e-4f );
    // A still wind pushes zeros, previous included (a push block keeps bytes between draws).
    EXPECT_EQ( PackViewInstanceWind( InstanceWind{}, 7.0 ).B.z, 0.0f );
}

// Step A: the rows are built once per frame per view BEFORE any pass is declared, and every captured frame state
// carries them. Mutation: drop the BuildObjectMotions call, move it after the first AddFrame*, or drop the
// frame.ObjectMotions / ObjectBones assignment in CaptureFrameState -> red.
TEST( VelocityTarget, MotionRowsAreBuiltBeforeAnyViewPassIsDeclared )
{
    using VelocityTargetTest::ReadFile;
    const auto root  = Desert::TestSupport::RepositoryRoot();
    const auto scene = ReadFile( root / "Desert/Desert/Source/Engine/Graphic/SceneRenderer.cpp" );
    const auto body  = scene.find( "void SceneRenderer::OnUpdate(" );
    ASSERT_NE( body, std::string::npos );
    const auto build = scene.find( "->BuildObjectMotions( m_ViewState.Motion() )", body );
    const auto prev  = scene.find( "->SetPrevWorldTimeSeconds( GetViewFrame()->PrevTimeSeconds )", body );
    const auto first = scene.find( "AddFrame", body );
    ASSERT_NE( build, std::string::npos ) << "OnUpdate must build the view's motion rows";
    ASSERT_NE( prev, std::string::npos ) << "OnUpdate must hand the view's previous time to the mesh renderer";
    ASSERT_NE( first, std::string::npos );
    EXPECT_LT( build, first ) << "the rows must be final before the first pass is declared";
    EXPECT_LT( prev, first );

    const auto meshes = ReadFile( root / "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.cpp" );
    const auto capture = meshes.find( "PBRSceneFrame MeshRenderer::CaptureFrameState(" );
    ASSERT_NE( capture, std::string::npos );
    const auto end = meshes.find( "return frame;", capture );
    ASSERT_NE( end, std::string::npos );
    const std::string captureBody = meshes.substr( capture, end - capture );
    EXPECT_NE( captureBody.find( "frame.ObjectMotions = m_ObjectMotions;" ), std::string::npos );
    EXPECT_NE( captureBody.find( "frame.ObjectBones   = m_ObjectBones;" ), std::string::npos );
}
