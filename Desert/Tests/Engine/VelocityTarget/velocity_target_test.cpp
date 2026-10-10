// TAA1 step 4 — the velocity target: its format has one spelling, the per-draw motion record has a GLSL twin of
// the same std430 layout, and the velocity the geometry passes write is current minus previous unjittered NDC
// (CPU twin Graphic::VelocityNdc, GLSL DesertVelocity in Common/ObjectMotion.glslh).

#include <Engine/Core/Projection.hpp>
#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Engine/Core/ShaderCompiler/ShaderGraphBindings.hpp>
#include <Engine/Graphic/InstanceWind.hpp>
#include <Engine/Graphic/View/ObjectMotionRows.hpp>
#include <Engine/Graphic/View/SceneViewState.hpp>
#include <Engine/Graphic/View/Velocity.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/ViewRasterTargets.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/ViewTargetLayouts.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGFault.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanShaderReflection.hpp>
#include <Engine/Core/ShaderCompiler/Includer/ShaderIncluder.hpp>

#include <Common/Core/Constants.hpp>

#include <shaderc/shaderc.hpp>

#include "../../TestSupport/runner.hpp"
#include "../../TestSupport/scratch_dir.hpp"

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cctype>
#include <cstring>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <array>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace VelocityTargetTest
{
    using namespace Desert::Graphic;

    // A matrix as its sixteen IEEE words: equal only when bit for bit equal (-0 and +0 differ, a NaN equals
    // itself).
    std::array<uint32_t, 16> Bits( const glm::mat4& m )
    {
        return std::bit_cast<std::array<uint32_t, 16>>( m );
    }

    std::string ReadFile( const std::filesystem::path& path )
    {
        const std::ifstream in( path, std::ios::binary );
        std::ostringstream  text;
        text << in.rdbuf();
        return text.str();
    }

    // The member list of `struct <name> { ... };` in a GLSL source, as (type, name) in declaration order.
    std::vector<std::pair<std::string, std::string>> GlslStructMembers( const std::string& source,
                                                                        const std::string& name )
    {
        std::vector<std::pair<std::string, std::string>> members;
        const std::regex head( std::format( R"(struct\s+{}\s*\{{([^}}]*)\}})", name ) );
        std::smatch      match;
        if ( !std::regex_search( source, match, head ) )
            return members;
        // Line comments go first: the twin's member comments are prose ("... palette in objectBones; 0
        // otherwise"), and a `word word;` inside one would otherwise read as a member.
        const std::string body = std::regex_replace( match[1].str(), std::regex( "//[^\\n]*" ), "" );
        const std::regex  member( R"((\w+)\s+(\w+)\s*;)" );
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
    const auto root = Desert::TestSupport::RepositoryRoot() / "Editor/Resources/Shaders/Mesh/Surface";
    const auto noWs = []( std::string s )
    {
        s.erase( std::remove_if( s.begin(), s.end(), []( unsigned char c ) { return std::isspace( c ) != 0; } ),
                 s.end() );
        return s;
    };
    for ( const char* vertex : { "Vertex_Static.glslh", "Vertex_Skinned.glslh" } )
    {
        const std::string src = noWs( ReadFile( root / vertex ) );
        ASSERT_FALSE( src.empty() ) << vertex;
        EXPECT_NE( src.find( "objectMotions.Motions[m_PushConstants.PrimitiveIndex]" ), std::string::npos )
             << vertex;
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
    const auto        output  = gbuffer.find( "layout(location=4)outvec2oVelocity;" );
    ASSERT_NE( output, std::string::npos );
    // The NEAREST gate before the output: the pass has more than one RSM gate (GBUF1 also gates the shading word
    // at slot 2), and the one that matters is the one whose #endif must come after oVelocity.
    const auto gate = gbuffer.rfind( "#ifndefDESERT_GBUFFER_RSM", output );
    ASSERT_NE( gate, std::string::npos );
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
        return std::format( "Shader \"SceneReadRangeProbe\"\n{{\n    Domain Surface\n\n    {}"
                            "\n    State {{ Cull Back ZTest LEqual ZWrite On }}\n"
                            "    Vertex\n    {{\n        In(0) vec3 a_Position;\n"
                            "        void main() {{ gl_Position = vec4( a_Position, 1.0 ); }}\n    }}\n"
                            "    Fragment\n    {{\n        Out(0) vec4 o_Color;\n"
                            "        void main() {{ o_Color = vec4( 1.0 ); }}\n    }}\n}}\n",
                            properties );
    }
} // namespace VelocityTargetTest

// Lead decision (TAA1-VEL-d): the scene-read slots are FIXED numbers, so they need a guarantee and not a hope.
// Seam 1, the material layout: a Properties row or a run of texture properties that reaches into the reserved
// range is a named parse error. The control parses the same block one run higher, so the refusal is about the
// range and not about the probe.
// Mutation: delete the IsSceneReadBinding refusal in DShaderParser::Parse -> red.
TEST( VelocityTarget, AMaterialRowOrTextureCannotBeNumberedIntoTheSceneReadRange )
{
    using Desert::Core::kSceneReadBindingFirst;
    using Desert::Core::Preprocess::DShaderParser;

    const auto textures = []( uint32_t first )
    {
        return std::format(
             "Properties Binding(1) TextureBinding({})\n    {{\n        Color Tint (\"Tint\") = (1, 1, 1, 1)\n"
             "        Texture2D u_First (\"First\")\n        Texture2D u_Second (\"Second\")\n    }}\n",
             first );
    };
    // Two textures from one below the range: the second lands on its first slot.
    const auto intoRange = DShaderParser::Parse( ProbeWithProperties( textures( kSceneReadBindingFirst - 1u ) ) );
    ASSERT_FALSE( intoRange.IsSuccess() );
    EXPECT_NE( intoRange.GetError().find( "scene-read" ), std::string::npos ) << intoRange.GetError();
    EXPECT_NE( intoRange.GetError().find( "u_Second" ), std::string::npos ) << intoRange.GetError();

    const auto row = DShaderParser::Parse( ProbeWithProperties(
         std::format( "Properties Binding({})\n    {{\n        Color Tint (\"Tint\") = (1, 1, 1, 1)\n    }}\n",
                      kSceneReadBindingFirst ) ) );
    ASSERT_FALSE( row.IsSuccess() );
    EXPECT_NE( row.GetError().find( "scene-read" ), std::string::npos ) << row.GetError();

    const auto above = DShaderParser::Parse( ProbeWithProperties(
         textures( Desert::Core::kSceneReadBindingFirst + Desert::Core::kSceneReadBindingCount ) ) );
    EXPECT_TRUE( above.IsSuccess() ) << ( above.IsSuccess() ? "" : above.GetError() );
}

// Seam 2, the hand-numbered declarations (a template's own `layout( binding = n )` textures, a pass header's
// lighting slots — none of which goes through the Properties block): over the WHOLE shipped shader tree, the
// reserved numbers carry the two scene-read resources and nothing else, and ObjectMotion.glslh spells exactly the
// C++ numbers SceneFrameBinding's resources are reserved under. Text, not reflection, because a resource that only
// collides inside one cell would need that cell compiled to be seen; the after-compile half is
// ShaderReflection::ReflectStage, which refuses a slot claimed twice by name.
// Mutation: move SpotLightsUB (or any texture) to 25 / spell ObjectMotions at 16 (where SpotLightsUB lives) ->
// red.
TEST( VelocityTarget, OnlyTheSceneReadResourcesSitInTheReservedRange )
{
    const auto       shaders = Desert::TestSupport::RepositoryRoot() / "Editor/Resources/Shaders";
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
            const auto&    m       = *it;
            const uint32_t binding = static_cast<uint32_t>( std::stoul( m[1].matched ? m[1].str() : m[3].str() ) );
            const std::string name = m[2].matched ? m[2].str() : m[4].str();
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
// geometry pass reads it: the ECS emplace names it, the command's Execute forwards it, SceneRenderer copies it
// onto the render data. A hop that drops it keys every object of the scene under one id — each draw then reads
// another object's previous transform, which is a wrong velocity with no error anywhere. Mutation that turns this
// red: remove `Entity` from any command's Execute, or pass anything but `entity` first in an emplace.
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
        for ( auto it = std::sregex_iterator( src.begin(), src.end(), emplace ); it != std::sregex_iterator();
              ++it )
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
    const std::regex entityCopy( R"(\.Entity\s*=\s*entity\b)" );
    EXPECT_EQ(
         std::distance( std::sregex_iterator( scene.begin(), scene.end(), entityCopy ), std::sregex_iterator() ),
         2 )
         << "SubmitGenericMesh / SubmitSlotMaterialMesh must both copy the entity onto GenericMeshRenderData";
}

// Step C: the instanced view pass evaluates the wind at the view's previous frame (Vertex_Instanced reads
// WindB.z); the renderer writes it per view. Mutation: drop the B.z write in PackViewInstanceWind -> red; the
// light-view pack writing a previous time -> red.
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

    const auto meshes =
         ReadFile( root / "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.cpp" );
    const auto capture = meshes.find( "SceneFrameBinding MeshRenderer::CaptureFrameState(" );
    ASSERT_NE( capture, std::string::npos );
    const auto end = meshes.find( "return frame;", capture );
    ASSERT_NE( end, std::string::npos );
    const std::string captureBody = meshes.substr( capture, end - capture );
    EXPECT_NE( captureBody.find( "frame.ObjectMotions = m_ObjectMotions;" ), std::string::npos );
    EXPECT_NE( captureBody.find( "frame.ObjectBones   = m_ObjectBones;" ), std::string::npos );
}

namespace VelocityTargetTest
{
    // The body of `<head>` in a source (CR stripped): from the head to the next top-level `    }` line of the
    // namespace.
    std::string FunctionBody( const std::string& crlfOrLf, const std::string& head )
    {
        std::string source = crlfOrLf;
        source.erase( std::remove( source.begin(), source.end(), '\r' ), source.end() );
        const auto begin = source.find( head );
        if ( begin == std::string::npos )
            return {};
        const auto end = source.find( "\n    }\n", begin );
        return source.substr( begin, end == std::string::npos ? std::string::npos : end - begin );
    }
} // namespace VelocityTargetTest

// Step B: every view-pass draw builder indexes the object's motion row and pushes the submesh transform RELATIVE
// to it (identity), and the skinned view path skins from the row's ObjectBones slices instead of uploading its own
// palette. Mutations: drop a SetPrimitiveIndex (static / generic / glass / skinned) -> red; push obj->Transform or
// g.Transform again (as `.Transform` or SetPushMatrix) -> red; restore UploadSkinnedBones or SetSkinnedBoneOffset
// in BuildSkinnedDraws -> red.
TEST( VelocityTarget, EveryViewPassDrawIndexesItsMotionRowAndPushesARelativeTransform )
{
    using VelocityTargetTest::FunctionBody;
    using VelocityTargetTest::ReadFile;
    const auto        root = Desert::TestSupport::RepositoryRoot();
    const std::string forward =
         ReadFile( root / "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRendererForward.cpp" );
    const std::vector<std::string> builders = {
         "void MeshRenderer::BuildGenericDraws(",
         "void MeshRenderer::DeclareGlassBindings(",
         "void MeshRenderer::BuildStaticDraws(",
         "void MeshRenderer::BuildSkinnedDraws(",
    };
    // Raw strings: as plain literals `\(` and `\s` are unknown escapes MSVC turns into `(` and `s`, so the pattern
    // silently became a different one (an extra capture group took the call's parentheses).
    const std::regex designated( R"(\.Transform\s*=\s*([^,\n]+),)" );
    const std::regex pushed( R"(SetPushMatrix\(\s*([^;]+?)\s*\);)" );
    for ( const auto& head : builders )
    {
        const std::string body = FunctionBody( forward, head );
        ASSERT_FALSE( body.empty() ) << head;
        EXPECT_NE( body.find( "SetPrimitiveIndex(" ), std::string::npos ) << head << " must push the motion row";
        for ( auto it = std::sregex_iterator( body.begin(), body.end(), designated ); it != std::sregex_iterator();
              ++it )
        {
            const std::string value = ( *it )[1].str();
            EXPECT_TRUE( value.rfind( "glm::mat4( 1.0f )", 0 ) == 0 || value == "unusedModelTransform" )
                 << head << " draws with a world transform in the push (`.Transform = " << value
                 << "`): the cell reads World from the row, so the push must be relative to it";
        }
        for ( auto it = std::sregex_iterator( body.begin(), body.end(), pushed ); it != std::sregex_iterator();
              ++it )
            EXPECT_EQ( ( *it )[1].str(), "glm::mat4( 1.0f )" ) << head << " SetPushMatrix with a world transform";
    }
    const std::string skinned = FunctionBody( forward, "void MeshRenderer::BuildSkinnedDraws(" );
    EXPECT_EQ( skinned.find( "UploadSkinnedBones" ), std::string::npos )
         << "the skinned view cell reads ObjectBones (the row's BoneOffset / PrevBoneOffset), not a group palette";
    EXPECT_EQ( skinned.find( "SetSkinnedBoneOffset" ), std::string::npos );
}

// Step A's row numbering, on the CPU (Graphic/View/ObjectMotionRows.hpp) — what MeshRenderer::BuildObjectMotions
// uploads. Each test names the mutation that turns it red.
namespace VelocityTargetTest
{
    glm::mat4 At( const float x )
    {
        return glm::translate( glm::mat4( 1.0f ), glm::vec3( x, 0, 0 ) );
    }
} // namespace VelocityTargetTest

// Mutation: drop the identical-world lookup in the rigid loop (every record a new row) -> red.
TEST( VelocityTarget, SubmeshRecordsOfOneObjectShareOneRow )
{
    using namespace Desert::Graphic;
    MotionHistory                   motion;
    const std::vector<MotionRecord> rigid = {
         { .Entity = 3, .World = VelocityTargetTest::At( 10 ) }, // material slot 0
         { .Entity = 3, .World = VelocityTargetTest::At( 10 ) }, // material slot 1 of the same mesh
         { .Entity = 3, .World = VelocityTargetTest::At( 90 ) }, // a second primitive of the entity
         { .Entity = 4, .World = VelocityTargetTest::At( 10 ) }, // another entity at the same world
    };
    ObjectMotionRows out;
    BuildObjectMotionRows( motion, rigid, {}, out );
    ASSERT_EQ( out.RecordRows.size(), 4u );
    EXPECT_EQ( out.Rows.size(), 3u ) << "one row per primitive";
    EXPECT_EQ( out.RecordRows[0], out.RecordRows[1] );
    EXPECT_NE( out.RecordRows[0], out.RecordRows[2] );
    EXPECT_NE( out.RecordRows[0], out.RecordRows[3] ) << "rows are per entity, never shared across entities";
    for ( std::size_t i = 0; i < rigid.size(); ++i )
        EXPECT_EQ( out.Rows[out.RecordRows[i]].World, rigid[i].World ) << i;
}

// Mutation: key the history by the global row index instead of the entity's slot (a new object submitted first
// shifts every other one) or drop PreviousTransform (PrevWorld = World always) -> red.
TEST( VelocityTarget, RowsCarryEachPrimitivesOwnPreviousWorldAcrossFrames )
{
    using namespace Desert::Graphic;
    using VelocityTargetTest::At;
    MotionHistory    motion;
    ObjectMotionRows out;
    {
        const std::vector<MotionRecord> rigid = { { .Entity = 5, .World = At( 0 ) },
                                                  { .Entity = 6, .World = At( 7 ) } };
        BuildObjectMotionRows( motion, rigid, {}, out );
        motion.EndFrame();
    }
    // Next frame: a new entity is submitted FIRST, entity 5 moved, entity 6 is still.
    const std::vector<MotionRecord> rigid = { { .Entity = 9, .World = At( 300 ) },
                                              { .Entity = 5, .World = At( 40 ) },
                                              { .Entity = 6, .World = At( 7 ) } };
    BuildObjectMotionRows( motion, rigid, {}, out );
    ASSERT_EQ( out.Rows.size(), 3u );
    EXPECT_EQ( out.Rows[out.RecordRows[0]].PrevWorld, At( 300 ) ) << "absent last frame: prev = current";
    EXPECT_EQ( out.Rows[out.RecordRows[1]].PrevWorld, At( 0 ) ) << "moved: its own previous world";
    // A still object: bit for bit, so its velocity is exactly the camera's.
    EXPECT_EQ( Bits( out.Rows[out.RecordRows[2]].PrevWorld ), Bits( out.Rows[out.RecordRows[2]].World ) );
}

// Mutation: let a skinned record reuse a rigid row of the same world, write PrevBoneOffset = BoneOffset, or key
// PreviousBones by slot 0 for every record -> red.
TEST( VelocityTarget, EverySkinnedSlotGetsItsOwnRowAndItsOwnPreviousPalette )
{
    using namespace Desert::Graphic;
    using VelocityTargetTest::At;
    const std::vector<glm::mat4>    poseA0 = { At( 1 ), At( 2 ) };
    const std::vector<glm::mat4>    poseB0 = { At( 5 ), At( 6 ), At( 7 ) };
    const std::vector<glm::mat4>    poseA1 = { At( 11 ), At( 12 ) };
    const std::vector<glm::mat4>    poseB1 = { At( 15 ), At( 16 ), At( 17 ) };
    MotionHistory                   motion;
    ObjectMotionRows                out;
    const std::vector<MotionRecord> rigid = { { .Entity = 2, .World = At( 0 ) } };
    BuildObjectMotionRows( motion, rigid,
                           std::vector<MotionRecord>{ { .Entity = 2, .World = At( 0 ), .Bones = poseA0 },
                                                      { .Entity = 2, .World = At( 0 ), .Bones = poseB0 } },
                           out );
    motion.EndFrame();
    const std::vector<MotionRecord> skinned = { { .Entity = 2, .World = At( 0 ), .Bones = poseA1 },
                                                { .Entity = 2, .World = At( 0 ), .Bones = poseB1 } };
    BuildObjectMotionRows( motion, rigid, skinned, out );

    ASSERT_EQ( out.RecordRows.size(), 3u );
    EXPECT_EQ( out.Rows.size(), 3u ) << "the rigid record and both skinned slots are three primitives";
    const GpuObjectMotion& a     = out.Rows[out.RecordRows[1]];
    const GpuObjectMotion& b     = out.Rows[out.RecordRows[2]];
    const auto             slice = [&]( const uint32_t offset, const std::size_t count )
    {
        const auto first = out.Palettes.begin() + static_cast<std::ptrdiff_t>( offset );
        return std::vector<glm::mat4>( first, first + static_cast<std::ptrdiff_t>( count ) );
    };
    EXPECT_EQ( slice( a.BoneOffset, 2 ), poseA1 );
    EXPECT_EQ( slice( a.PrevBoneOffset, 2 ), poseA0 );
    EXPECT_EQ( slice( b.BoneOffset, 3 ), poseB1 );
    EXPECT_EQ( slice( b.PrevBoneOffset, 3 ), poseB0 );
    EXPECT_EQ( out.Palettes.size(), 2u * ( poseA1.size() + poseB1.size() ) ) << "both frames, end to end";
}

// Mutation: build twice per frame with a different RecordRows order (rows not stable within the frame), or fill
// RecordRows in another order than rigid-then-skinned -> red.
TEST( VelocityTarget, RowIndicesAreStableWithinTheFrame )
{
    using namespace Desert::Graphic;
    using VelocityTargetTest::At;
    const std::vector<glm::mat4>    pose  = { At( 1 ) };
    const std::vector<MotionRecord> rigid = {
         { .Entity = 1, .World = At( 0 ) }, { .Entity = 2, .World = At( 3 ) }, { .Entity = 1, .World = At( 0 ) } };
    const std::vector<MotionRecord> skinned = { { .Entity = 8, .World = At( 4 ), .Bones = pose } };
    MotionHistory                   motion;
    ObjectMotionRows                first;
    ObjectMotionRows                again;
    BuildObjectMotionRows( motion, rigid, skinned, first );
    BuildObjectMotionRows( motion, rigid, skinned, again ); // a second build in the same frame
    EXPECT_EQ( first.RecordRows, again.RecordRows );
    ASSERT_EQ( first.RecordRows.size(), 4u );
    EXPECT_EQ( first.RecordRows, ( std::vector<uint32_t>{ 0, 1, 0, 2 } ) );
    for ( std::size_t i = 0; i < first.Rows.size(); ++i )
    {
        const GpuObjectMotion& x = first.Rows[i];
        const GpuObjectMotion& y = again.Rows[i];
        EXPECT_EQ( Bits( x.World ), Bits( y.World ) ) << i;
        EXPECT_EQ( Bits( x.PrevWorld ), Bits( y.PrevWorld ) ) << i;
        EXPECT_EQ( x.BoneOffset, y.BoneOffset ) << i;
        EXPECT_EQ( x.PrevBoneOffset, y.PrevBoneOffset ) << i;
        EXPECT_EQ( x.Pad0, y.Pad0 ) << i;
        EXPECT_EQ( x.Pad1, y.Pad1 ) << i;
    }
}

// Step F: THE DEPTH-WRITER CENSUS. A pixel's depth and its velocity must come from the same surface, so every
// program that writes the view's depth writes velocity too. Over the whole shipped tree, each `.shader` with
// `ZWrite On` is one of: a `Domain Surface` program with a `Surface` block (its stages are the Mesh/Surface pass
// templates, whose velocity outputs ViewPassSurfaceStagesWriteTheVelocityOfTheirOwnSurface pins), a program that
// writes `oVelocity` itself, or a named light-view exclusion that must NOT write it. TextSDF is listed EXCLUDED
// with its reason (ZWrite Off: world-space text writes no depth, so it owes no velocity) and the census holds the
// reason true. Mutations: drop oVelocity from Terrain.shader or TerrainGBuffer.shader / add a ZWrite On program
// with its own stages and no velocity / give TerrainShadow a velocity output / turn TextSDF's ZWrite On -> red.
TEST( VelocityTarget, EveryDepthWritingViewProgramWritesVelocity )
{
    const std::filesystem::path shaders = Desert::TestSupport::RepositoryRoot() / "Editor/Resources/Shaders";
    // Light-view programs: they write a cascade's depth, never the view's; no previous frame of their own.
    const std::map<std::string, std::string> lightView = {
         { "Programs/Terrain/TerrainShadow.shader", "cascade depth (light view)" } };
    const std::regex velocityOut( R"(Out\(\s*\d+\s*\)\s*vec2\s+oVelocity|out\s+vec2\s+oVelocity)" );
    // The EFFECTIVE depth write of a pass: its State's ZWrite, else what a pipeline built from it gets when the
    // State says nothing (PipelineCache applies a State field only when it is set; the spec default stands).
    const bool unsetDepthWrite = Desert::Graphic::GraphicsPipelineSpecification{}.DepthWriteEnabled;
    using Desert::Core::Formats::ShaderDomain;

    std::size_t depthWriters = 0;
    for ( const auto& entry : std::filesystem::recursive_directory_iterator( shaders ) )
    {
        if ( !entry.is_regular_file() || entry.path().extension() != ".shader" )
            continue;
        const std::string rel  = std::filesystem::relative( entry.path(), shaders ).generic_string();
        const std::string text = ReadFile( entry.path() );
        if ( !Desert::Core::Preprocess::DShaderParser::IsDShader( text ) )
            continue;
        const auto parsed = Desert::Core::Preprocess::DShaderParser::Parse( text );
        if ( !parsed )
        {
            ADD_FAILURE() << rel << " does not parse: " << parsed.GetError();
            continue;
        }
        const auto& program = parsed.GetValue();
        // The census population: the view-geometry domains (whatever their State says, the default included) and
        // every program that states its own depth write. Engine programs whose depth state the C++ sets (grid,
        // debug lines, particles, sky) are outside a text census.
        const bool viewDomain =
             program.Meta.Domain == ShaderDomain::Surface || program.Meta.Domain == ShaderDomain::Terrain;
        bool writesDepth = false;
        for ( const auto& pass : program.Passes )
            if ( ( viewDomain || pass.State.DepthWrite.has_value() ) &&
                 pass.State.DepthWrite.value_or( unsetDepthWrite ) )
                writesDepth = true;
        if ( !writesDepth )
            continue;
        ++depthWriters;
        const bool writesVelocity = std::regex_search( text, velocityOut );
        if ( lightView.contains( rel ) )
        {
            EXPECT_FALSE( writesVelocity ) << rel << " is a light view (" << lightView.at( rel )
                                           << ") and must not write the view's velocity";
            continue;
        }
        const bool surfaceTemplate = !program.Surface.Cells.empty();
        EXPECT_TRUE( surfaceTemplate || writesVelocity )
             << rel << " writes the view's depth (ZWrite On) with its own stages and no oVelocity output";
    }
    EXPECT_GE( depthWriters, 8u ) << "the census did not find the shipped depth writers";

    // The terrain is the depth writer with its own stages: both its view programs write velocity, through the
    // one TerrainVelocity of TerrainSurface.glslh, at the slot of their target.
    const std::filesystem::path terrain = shaders / "Programs/Terrain";
    EXPECT_NE( ReadFile( terrain / "Terrain.shader" ).find( "Out(1) vec2 oVelocity" ), std::string::npos );
    EXPECT_NE( ReadFile( terrain / "TerrainGBuffer.shader" ).find( "Out(4) vec2 oVelocity" ), std::string::npos );
    const std::string surface = ReadFile( terrain / "TerrainSurface.glslh" );
    EXPECT_NE( surface.find( "DesertVelocity( u.ViewProjection * world, u.PrevViewProjection * world )" ),
               std::string::npos );

    // EXCLUDED: TextSDF, with its reason held true.
    const std::string text = ReadFile( shaders / "Programs/Text/TextSDF.shader" );
    EXPECT_TRUE( std::regex_search( text, std::regex( R"(ZWrite\s+Off)" ) ) )
         << "TextSDF is excluded from velocity because it writes no depth; with ZWrite On it owes oVelocity";
    EXPECT_EQ( text.find( "oVelocity" ), std::string::npos );
}

// Step E: THE VELOCITY TARGET. One transient per view (CreateViewVelocity), appended by AppendGraphColors to the
// G-buffer at slot 4 and to the scene target at slot 1, so both name the SAME graph texture; its fault default is
// kVelocityFaultDefault (Black = zero motion). Mutations: create a second "Velocity" per target / append a
// different ref to one target / drop the SetFaultDefault or change kVelocityFaultDefault -> red.
TEST( VelocityTarget, OneVelocityTransientPerViewIsTheSlotOfBothTargets )
{
    using namespace Desert::Graphic;
    static_assert( kVelocityFaultDefault == RDG::FaultDefault::Black );
    EXPECT_EQ( Desert::Graphic::SceneTargetLayout().ColorFormats.at( Desert::Graphic::kSceneTargetVelocitySlot ),
               ViewTargetFormats::kVelocity );
    EXPECT_EQ( Desert::Graphic::GBufferLayout().ColorFormats.at( Desert::Graphic::kGBufferVelocitySlot ),
               ViewTargetFormats::kVelocity );

    RDG::Builder       graph( "VelocityProbe" );
    const ViewVelocity velocity = CreateViewVelocity( graph, RDG::Extent3D{ 64, 32, 1 }, 1 );
    ASSERT_TRUE( velocity.Resolved.IsValid() );
    EXPECT_FALSE( velocity.Multisample.IsValid() );
    const auto name = graph.GetTextureName( velocity.Resolved );
    ASSERT_TRUE( name.IsSuccess() );
    EXPECT_EQ( name.GetValue(), "Velocity" );

    RDG::TextureDesc colour;
    colour.Size = RDG::Extent3D{ 64, 32, 1 };
    RasterTargets scene;
    scene.Colors = { graph.CreateTexture( colour, "SceneColor" ) };
    RasterTargets gbuffer;
    for ( const char* slot : { "A", "B", "C", "Emissive" } )
        gbuffer.Colors.push_back( graph.CreateTexture( colour, slot ) );
    const GraphColor onScene[]   = { VelocityColor( velocity, 1 ) };
    const GraphColor onGBuffer[] = { VelocityColor( velocity, 1 ) };
    ASSERT_TRUE( AppendGraphColors( scene, onScene, false ) );
    ASSERT_TRUE( AppendGraphColors( gbuffer, onGBuffer, false ) );
    ASSERT_EQ( scene.Colors.size(), Desert::Graphic::SceneTargetLayout().ColorFormats.size() );
    ASSERT_EQ( gbuffer.Colors.size(), Desert::Graphic::GBufferLayout().ColorFormats.size() );
    EXPECT_EQ( scene.Colors[Desert::Graphic::kSceneTargetVelocitySlot], velocity.Resolved );
    EXPECT_EQ( gbuffer.Colors[Desert::Graphic::kGBufferVelocitySlot], velocity.Resolved );

    // At MSAA the scene target draws the multisampled twin; velocity is SampleZero, so the slot has NO in-pass
    // resolve (the hardware average) — "Velocity: Resolve" writes sample 0 into the one transient
    // (RenderGraphCompile.SampleZeroGraphColourGetsAResolveNodeAndNoHardwareResolve).
    const ViewVelocity msaa = CreateViewVelocity( graph, RDG::Extent3D{ 64, 32, 1 }, 4 );
    ASSERT_TRUE( msaa.Multisample.IsValid() );
    RasterTargets multisampled;
    multisampled.Colors       = { graph.CreateTexture( colour, "SceneColor.MS" ) };
    multisampled.Resolves     = { graph.CreateTexture( colour, "SceneColor.Resolved" ) };
    const GraphColor onMsaa[] = { VelocityColor( msaa, 4 ) };
    ASSERT_TRUE( AppendGraphColors( multisampled, onMsaa, true ) );
    EXPECT_EQ( multisampled.Colors[1], msaa.Multisample );
    EXPECT_EQ( onMsaa[0].Resolve, GraphColorResolve::SampleZero );
    ASSERT_EQ( multisampled.Resolves.size(), multisampled.Colors.size() ) << "Resolves is indexed by colour slot";
    EXPECT_FALSE( multisampled.Resolves[1].IsValid() ) << "velocity must not be averaged by the render pass";
    // A single-sample colour on a multisampled target is refused, nothing appended.
    RasterTargets refused = multisampled;
    EXPECT_FALSE( AppendGraphColors( refused, onScene, true ) );
    EXPECT_EQ( refused.Colors.size(), multisampled.Colors.size() );
}

// Step E: LoadOp PER SLOT. A node clearing scene colour to the sky grey clears velocity to ZERO (its own clear
// value), not to the grey; the next node on the target loads velocity even when it clears colour again.
// Mutations: ColorLoads applies the pass's LoadOp to every slot / clears the own-clear slot on every writer ->
// red.
TEST( VelocityTarget, EachColourSlotTakesItsOwnClearOnItsFirstWriter )
{
    using namespace Desert::Graphic;
    RasterTargets targets;
    targets.Colors              = { RDG::TextureRef{ 0 } };
    const GraphColor velocity[] = { GraphColor{ RDG::TextureRef{ 7 }, {}, RDG::ClearValue{} } };
    ASSERT_TRUE( AppendGraphColors( targets, velocity, false ) );

    std::set<uint32_t> started;
    const RDG::LoadOp  grey  = RDG::LoadOp::ClearColor( 0.1f, 0.1f, 0.1f, 1.0f );
    const auto         first = ColorLoads( targets, grey, started );
    ASSERT_EQ( first.size(), 2u );
    EXPECT_EQ( first[0].Action, RDG::LoadAction::Clear );
    EXPECT_EQ( first[0].Value.Color, ( std::array<float, 4>{ 0.1f, 0.1f, 0.1f, 1.0f } ) );
    EXPECT_EQ( first[1].Action, RDG::LoadAction::Clear );
    EXPECT_EQ( first[1].Value.Color, ( std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 0.0f } ) );

    const auto second = ColorLoads( targets, grey, started );
    EXPECT_EQ( second[0].Action, RDG::LoadAction::Clear );
    EXPECT_EQ( second[1].Action, RDG::LoadAction::Load );
    const auto loaded = ColorLoads( targets, RDG::LoadOp::Load(), started );
    EXPECT_EQ( loaded[0].Action, RDG::LoadAction::Load );
    EXPECT_EQ( loaded[1].Action, RDG::LoadAction::Load );
}

// Every pipeline drawing into a view target is built against the target LAYOUT (ViewTargetLayouts.hpp: the
// graph owns the images, the velocity slot is a graph transient no Framebuffer has), never a Framebuffer: a
// pipeline built for the framebuffer's own render pass lacks the velocity slot and is incompatible with the pass
// the graph opens. Mutation: revert any one site to `.Framebuffer = target` -> red.
TEST( VelocityTarget, NoViewTargetPipelineNamesAFramebuffer )
{
    using VelocityTargetTest::ReadFile;
    const auto                  root = Desert::TestSupport::RepositoryRoot();
    const std::filesystem::path E    = root / "Desert/Desert/Source/Engine/Graphic/Systems/Scene";
    // file -> how many pipelines in it draw into a view target (scene target or G-buffer)
    const std::map<std::filesystem::path, int> sites = {
         { E / "Skybox/SkyboxRenderer.cpp", 2 },
         { E / "Mesh/MeshRendererForward.cpp", 5 },
         { E / "Mesh/MeshRendererDebug.cpp", 2 },
         { E / "Mesh/MeshRendererDeferred.cpp", 2 },
         { E / "Deferred/DeferredLightingRenderer.hpp", 1 },
         { E / "Deferred/SSRRenderer.hpp", 1 },
         { E / "Clouds/VolumetricCloudRenderer.cpp", 1 },
         { E / "Fog/HeightFogRenderer.cpp", 1 },
         { E / "Particles/ParticleRenderer.cpp", 1 },
         { E / "Terrain/TerrainRenderer.cpp", 1 }, // CreateTerrainPipeline, called with both layouts
         { "Editor/Source/Editor/RenderSystems/Passes/EditorColliderPass.cpp", 1 },
         { "Editor/Source/Editor/RenderSystems/Passes/EditorGridPass.cpp", 1 },
         { "Editor/Source/Editor/RenderSystems/Passes/EditorCubemapPreviewPass.cpp", 1 },
    };
    const std::regex layout(
         R"(\.TargetLayout\s*=\s*(Desert::Graphic::)?(SceneTargetLayout|GBufferLayout)\(\)|\.TargetLayout\s*=\s*layout;)" );
    // A view target named as a pipeline's Framebuffer (the names those sites held before).
    const std::regex framebuffer(
         R"(\.Framebuffer\s*=\s*(target|targetFb|compositeFramebuffer|gbuffer|framebuffer|scene->GetTargetFramebuffer\(\))\s*;)" );
    for ( const auto& [file, expected] : sites )
    {
        const std::string text = ReadFile( root / file );
        ASSERT_FALSE( text.empty() ) << file.generic_string();
        const auto count =
             std::distance( std::sregex_iterator( text.begin(), text.end(), layout ), std::sregex_iterator() );
        EXPECT_EQ( count, expected ) << file.generic_string()
                                     << ": a view-target pipeline is not built against its target layout";
        EXPECT_FALSE( std::regex_search( text, framebuffer ) )
             << file.generic_string() << ": a view-target pipeline names a Framebuffer";
    }
    const std::string terrain = ReadFile( root / E / "Terrain/TerrainRenderer.cpp" );
    EXPECT_NE( terrain.find( "GBufferLayout(), error" ), std::string::npos ) << "terrain G-buffer pipeline";
    EXPECT_NE( terrain.find( "SceneTargetLayout(), error" ), std::string::npos ) << "terrain scene pipeline";
}

// The view's velocity is created ONCE per view graph, inside CreateViewVelocity, before the first node, and is a
// colour of the scene target and the G-buffer only: never of a light view (RSM, cascades). Mutation: a second
// CreateViewVelocity, the call moved after AddFrameClearMainFramebuffer, or AddGraphColor( m_RSMBuffer ... ) ->
// red.
TEST( VelocityTarget, VelocityIsCreatedOncePerViewAndNeverOnALightView )
{
    using VelocityTargetTest::ReadFile;
    const auto root  = Desert::TestSupport::RepositoryRoot();
    const auto count = []( const std::string& text, const std::string& what )
    {
        std::size_t n = 0;
        for ( std::size_t at = text.find( what ); at != std::string::npos; at = text.find( what, at + 1 ) )
            ++n;
        return n;
    };
    const std::string targets = ReadFile( root / "Desert/Desert/Source/Engine/Graphic/ViewRasterTargets.hpp" );
    EXPECT_EQ( count( targets, "CreateTexture( desc, \"Velocity\" )" ), 1u );
    const std::string scene = ReadFile( root / "Desert/Desert/Source/Engine/Graphic/SceneRenderer.cpp" );
    EXPECT_EQ( count( scene, "CreateViewVelocity(" ), 1u );
    const auto created = scene.find( "CreateViewVelocity(" );
    const auto first   = scene.find( "AddFrameClearMainFramebuffer( graph" );
    ASSERT_NE( created, std::string::npos );
    ASSERT_NE( first, std::string::npos );
    EXPECT_LT( created, first ) << "the velocity must exist before the first node of the frame";
    EXPECT_EQ( count( scene, "AddGraphColor( m_TargetFramebuffer" ), 1u );
    EXPECT_EQ( count( scene, "AddGraphColor( m_GBuffer" ), 1u );
    EXPECT_EQ( count( scene, "AddGraphColor(" ), 2u ) << "velocity on any other target (RSM, cascades, overdraw)";
    EXPECT_EQ( count( scene, "AddGraphColor( m_RSMBuffer" ), 0u );
}

namespace VelocityTargetTest
{
    // The fragment stage of a shipped program, by its `Shader "<name>"`: the default pass, else the first pass
    // with one.
    struct FragmentOf
    {
        std::filesystem::path File;
        std::string           Source;
    };

    std::optional<FragmentOf> FindFragment( const std::filesystem::path& shaders, const std::string& name )
    {
        using Desert::Core::Formats::ShaderStage;
        for ( const auto& entry : std::filesystem::recursive_directory_iterator( shaders ) )
        {
            if ( !entry.is_regular_file() || entry.path().extension() != ".shader" )
                continue;
            const std::string text = ReadFile( entry.path() );
            if ( !Desert::Core::Preprocess::DShaderParser::IsDShader( text ) )
                continue;
            const auto parsed = Desert::Core::Preprocess::DShaderParser::Parse( text );
            if ( !parsed || parsed.GetValue().Name != name )
                continue;
            const auto& program = parsed.GetValue();
            if ( const auto it = program.Stages.find( ShaderStage::Fragment ); it != program.Stages.end() )
                return FragmentOf{ entry.path(), it->second };
            for ( const auto& pass : program.Passes )
                if ( const auto it = pass.Stages.find( ShaderStage::Fragment ); it != pass.Stages.end() )
                    return FragmentOf{ entry.path(), it->second };
        }
        return std::nullopt;
    }

    std::vector<uint32_t> CompileFragment( const FragmentOf& fragment )
    {
        const shaderc::Compiler compiler;
        shaderc::CompileOptions options;
        options.SetIncluder( std::make_unique<Desert::Core::ShaderIncluder>( fragment.File ) );
        // Same target as Core::ShaderCompiler::CompileGLSLToSPIRV.
        options.SetTargetEnvironment( shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1 );
        const auto result = compiler.CompileGlslToSpv( fragment.Source, shaderc_fragment_shader,
                                                       fragment.File.string().c_str(), options );
        EXPECT_EQ( result.GetCompilationStatus(), shaderc_compilation_status_success )
             << fragment.File.string() << ": " << result.GetErrorMessage();
        if ( result.GetCompilationStatus() != shaderc_compilation_status_success )
            return {};
        return { result.begin(), result.end() };
    }
} // namespace VelocityTargetTest

// The passes that draw INTO the view's scene target but must NOT write its velocity: sky, fog, clouds, the
// deferred and SSR composites, debug lines, the overdraw view, particles and the editor overlays. They have no
// motion of a surface of their own (a fullscreen composite has no surface), so the velocity the depth writers left
// must survive them. What keeps it is the colour-write MASK the pipeline gets from the FRAGMENT-OUTPUT REFLECTION
// (ShaderReflection::ReflectFragmentOutputLocations -> BuildColorBlendAttachments, the path VulkanPipeline takes):
// a slot the fragment stage has no output at gets mask 0. So each named program is compiled from its shipped
// source and reflected, and the velocity slot of SceneTargetLayout must come out unwritten with mask 0; and the
// C++ site that builds its pipeline must load it by that name against SceneTargetLayout. Mutation: give
// Skybox.shader (or any named program) an `Out(1) vec2 oVelocity` -> red; rename a site's shader -> red. ONE
// extent source (lead decision): the scene target, the G-buffer and the velocity transient all take the view
// extent. ViewTargetExtentMismatch names a target at another extent; SceneRenderer::OnUpdate refuses that frame,
// creates the velocity from m_ViewExtent, and Resize resizes both targets at the one extent. Mutations: make the
// mismatch check compare only widths / create the velocity from the target spec again / drop the G-buffer check
// or its Resize -> red.
TEST( VelocityTarget, SceneTargetGBufferAndVelocityShareTheViewExtent )
{
    using Desert::Graphic::ViewTargetExtentMismatch;
    EXPECT_TRUE( ViewTargetExtentMismatch( 1920, 1080, "G-buffer", 1920, 1080 ).empty() );
    const std::string height = ViewTargetExtentMismatch( 1920, 1080, "G-buffer", 1920, 1079 );
    EXPECT_NE( height.find( "G-buffer is 1920x1079" ), std::string::npos ) << height;
    EXPECT_NE( height.find( "1920x1080" ), std::string::npos ) << height;
    EXPECT_FALSE( ViewTargetExtentMismatch( 1920, 1080, "scene target", 1280, 1080 ).empty() );

    const std::string scene = VelocityTargetTest::ReadFile(
         Desert::TestSupport::RepositoryRoot() / "Desert/Desert/Source/Engine/Graphic/SceneRenderer.cpp" );
    const auto velocity = scene.find( "CreateViewVelocity(" );
    ASSERT_NE( velocity, std::string::npos );
    const std::string call = scene.substr( velocity, 160 );
    // TAA1-B step 6: the velocity belongs to the RENDER extent set (with the scene target and G-buffer it is written beside).
    EXPECT_NE( call.find( "frame.Split.Render.Width, frame.Split.Render.Height" ), std::string::npos )
         << "the velocity is not created at the frame's render extent: " << call;
    const auto refusal =
         scene.find( "ViewTargetExtentMismatch(", scene.rfind( "if ( m_TargetFramebuffer )", velocity ) );
    ASSERT_LT( refusal, velocity ) << "the extent check must run before the velocity is created";
    EXPECT_NE( scene.find( "\"G-buffer\",", refusal ), std::string::npos ) << "the G-buffer extent is not checked";
    EXPECT_NE( scene.find( "\"scene target\"", refusal ), std::string::npos );
    const auto resize = scene.find( "void SceneRenderer::Resize(" );
    ASSERT_NE( resize, std::string::npos );
    const auto targetResize  = scene.find( "m_TargetFramebuffer->Resize( width, height );", resize );
    const auto gbufferResize = scene.find( "m_GBuffer->Resize( width, height );", resize );
    ASSERT_NE( targetResize, std::string::npos );
    ASSERT_NE( gbufferResize, std::string::npos );
    EXPECT_LT( gbufferResize - targetResize, 200u ) << "Resize must resize the G-buffer with the scene target";
}

TEST( VelocityTarget, PassesThatMustNotWriteVelocityLeaveItsSlotMasked )
{
    using namespace Desert::Graphic::API::Vulkan;
    const auto root    = Desert::TestSupport::RepositoryRoot();
    const auto shaders = root / "Editor/Resources/Shaders";
    ASSERT_TRUE( std::filesystem::exists( shaders ) ) << shaders.string();
    // The includer resolves `#include <...>` against ShaderDir(), derived from the engine directory.
    Common::Constants::Path::SetEngineDir( root / "Editor" );

    const std::filesystem::path E  = root / "Desert/Desert/Source/Engine/Graphic/Systems/Scene";
    const std::filesystem::path Ed = root / "Editor/Source/Editor/RenderSystems/Passes";
    // shader name -> the site that builds its scene-target pipeline; the comment is why it owes no velocity.
    const std::vector<std::pair<std::string, std::filesystem::path>> mustNotWrite = {
         { "Skybox", E / "Skybox/SkyboxRenderer.cpp" },                       // sky
         { "ProceduralSky", E / "Skybox/SkyboxRenderer.cpp" },                // sky
         { "HeightFogApply", E / "Fog/HeightFogRenderer.cpp" },               // fullscreen fog composite
         { "CloudComposite", E / "Clouds/VolumetricCloudRenderer.cpp" },      // fullscreen cloud composite
         { "DeferredLighting", E / "Deferred/DeferredLightingRenderer.hpp" }, // deferred composite
         { "SSRComposite", E / "Deferred/SSRRenderer.hpp" },                  // reflection composite
         { "DebugLine", E / "Mesh/MeshRendererDebug.cpp" },                   // debug lines
         { "OverdrawResolve", E / "Mesh/MeshRendererDebug.cpp" },             // debug view
         { "ParticleSpriteDefault", E / "Particles/ParticleRenderer.cpp" },   // translucent particles
         { "Grid", Ed / "EditorGridPass.cpp" },                               // editor overlay
         { "DebugLine", Ed / "EditorColliderPass.cpp" },                      // editor overlay
         { "CubemapSphere", Ed / "EditorCubemapPreviewPass.cpp" },            // editor overlay
    };

    const auto     layout   = Desert::Graphic::SceneTargetLayout();
    const auto     slots    = static_cast<uint32_t>( layout.ColorFormats.size() );
    const uint32_t velocity = Desert::Graphic::kSceneTargetVelocitySlot;
    ASSERT_LT( velocity, slots );
    for ( const auto& [name, site] : mustNotWrite )
    {
        const std::string siteText = ReadFile( root / site );
        ASSERT_FALSE( siteText.empty() ) << site.generic_string();
        EXPECT_NE( siteText.find( std::format( "\"{}\"", name ) ), std::string::npos )
             << site.generic_string() << " no longer loads " << name;
        EXPECT_NE( siteText.find( "SceneTargetLayout()" ), std::string::npos )
             << site.generic_string() << " no longer builds " << name << " against the scene target layout";

        const auto fragment = FindFragment( shaders, name );
        if ( !fragment.has_value() )
        {
            ADD_FAILURE() << "no shipped program is called " << name;
            continue;
        }
        const auto spirv = CompileFragment( *fragment );
        ASSERT_FALSE( spirv.empty() ) << name;
        const auto written = ShaderReflection::ReflectFragmentOutputLocations( spirv );
        EXPECT_NE( std::find( written.begin(), written.end(), 0u ), written.end() )
             << name << " writes no scene colour: the census reflected the wrong stage";
        EXPECT_EQ( std::find( written.begin(), written.end(), velocity ), written.end() )
             << name << " writes the velocity slot; it must leave the depth writers' velocity in place";

        for ( const bool blend : { false, true } )
        {
            const auto attachments = ShaderReflection::BuildColorBlendAttachments(
                 written, std::vector<bool>( slots, blend ), VK_BLEND_FACTOR_SRC_ALPHA,
                 VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA );
            ASSERT_EQ( attachments.size(), slots ) << name;
            EXPECT_EQ( attachments[velocity].colorWriteMask, 0u )
                 << name << ": the velocity slot is not masked (blend " << blend << ")";
            EXPECT_EQ( attachments[velocity].blendEnable, VK_FALSE ) << name;
        }
    }
}

namespace
{
    // The depth-writer census parses every shipped program, and a surface program's includes resolve against
    // ShaderDir(), which derives from the engine directory (TestSupport/runner.hpp).
    const Desert::TestSupport::SuiteHost kHostSteps{ { .EngineDir = true } };
} // namespace

// THE VELOCITY VIEW MODE ("Debug: Velocity", VelocityView.shader) shows camera motion over static geometry, and is
// black exactly when nothing moved. The chain, CPU side: SceneViewState gives the next frame the committed frame's
// unjittered ViewProjection as PrevViewProjection; a static mesh's velocity is VelocityNdc( ViewProjection * p,
// PrevViewProjection * p ) (Vertex_Static.glslh -> ObjectMotion.glslh DesertVelocity); the view samples
// FrameTransients::Velocity at the render extent and shows it with the shader's own gain (16 px per frame = full
// brightness). VelocityViewColour below is the CPU twin of that shader, pinned line by line.
// Mutation: PrevViewProjection = the current matrix on an un-reset frame, the shader's gain or encoding changed
// (e.g. no 0.5 * size, a different divisor), or the view reading another texture than the frame's velocity -> red.
namespace VelocityTargetTest
{
    glm::vec3 VelocityViewColour( const glm::vec2 ndc, const glm::vec2 size )
    {
        const glm::vec2 pixels   = ndc * 0.5f * size;
        const float     speed    = glm::length( pixels );
        const float     strength = std::clamp( speed / 16.0f, 0.0f, 1.0f );
        const float     hue      = std::atan2( pixels.y, pixels.x ) * ( 0.5f / 3.14159265f ) + 0.5f;
        if ( !( speed > 0.0f ) )
            return glm::vec3( 0.0f );
        glm::vec3   rgb;
        const float offsets[3] = { 0.0f, 2.0f / 3.0f, 1.0f / 3.0f };
        for ( int c = 0; c < 3; ++c )
        {
            const float h = hue + offsets[c];
            rgb[c]        = std::clamp( std::abs( ( h - std::floor( h ) ) * 6.0f - 3.0f ) - 1.0f, 0.0f, 1.0f );
        }
        return rgb * strength;
    }

    ViewInputs StillViewInputs( const glm::vec3 eye, const double time )
    {
        ViewInputs in;
        in.View       = glm::lookAt( eye, eye + glm::vec3( 0.0f, 0.0f, -1.0f ), glm::vec3( 0.0f, 1.0f, 0.0f ) );
        in.Projection = Desert::Core::MakePerspective( glm::radians( 60.0f ), 16.0f / 9.0f, 10.0f, 5'000'000.0f );
        in.CameraPosition           = eye;
        in.NearPlane                = 10.0f;
        in.FarPlane                 = 5'000'000.0f;
        in.CameraIdentity           = 7;
        in.SceneIdentity            = 1;
        in.Output                   = ViewExtent{ 1920, 1080 };
        in.RenderScalePercent       = 100; // no temporal method here: below 100 % would need TAAU (SelectTemporalMethod)
        in.AntiAliasing.Method      = ::Common::Scalability::AntiAliasingMethod::None;
        in.AntiAliasing.PostProcess = ::Common::Scalability::AntiAliasingMethod::None;
        in.TimeSeconds              = time;
        return in;
    }
} // namespace VelocityTargetTest

TEST( VelocityTarget, TheVelocityViewShowsAMovingCameraOverStaticGeometryAndIsBlackWhenStill )
{
    SceneViewState  state;
    const glm::vec3 eye( 0.0f, 150.0f, 0.0f );
    const glm::vec4 staticPoint( 0.0f, 150.0f, -1000.0f, 1.0f ); // 10 m in front, world = identity, never moves

    const auto first = state.BeginFrame( StillViewInputs( eye, 1.0 ), nullptr );
    ASSERT_TRUE( first.IsSuccess() ) << first.GetError();
    state.EndFrame( RDG::ExecuteReport{} );

    // The camera moved 5 cm to +x (one editor frame of a slow pan, or one `set Camera.Position`).
    const auto moved = state.BeginFrame( StillViewInputs( eye + glm::vec3( 5.0f, 0.0f, 0.0f ), 1.016 ), nullptr );
    ASSERT_TRUE( moved.IsSuccess() ) << moved.GetError();
    const ViewFrame& m = moved.GetValue();
    ASSERT_EQ( m.HistoryReset, HistoryResetReason::None );
    const glm::vec2 size( static_cast<float>( m.Split.Render.Width ),
                          static_cast<float>( m.Split.Render.Height ) );
    ASSERT_GT( size.x, 0.0f );
    const glm::vec2 v  = VelocityNdc( m.ViewProjection * staticPoint, m.PrevViewProjection * staticPoint );
    const float     px = glm::length( v * 0.5f * size );
    EXPECT_GT( px, 1.0f ) << "5 cm at 10 m must move a static point by more than a render pixel";
    const glm::vec3 shown = VelocityViewColour( v, size );
    const float     peak  = std::max( { shown.r, shown.g, shown.b } );
    EXPECT_GE( peak * 255.0f, 8.0f ) << "the moving frame must show as colour in an 8-bit shot, peak " << peak;
    state.EndFrame( RDG::ExecuteReport{} );

    // The next frame at the same position: nothing moved, the view is exactly black (what a shot taken after the
    // camera stopped shows).
    const auto still = state.BeginFrame( StillViewInputs( eye + glm::vec3( 5.0f, 0.0f, 0.0f ), 1.032 ), nullptr );
    ASSERT_TRUE( still.IsSuccess() ) << still.GetError();
    const ViewFrame& s  = still.GetValue();
    const glm::vec2  vs = VelocityNdc( s.ViewProjection * staticPoint, s.PrevViewProjection * staticPoint );
    EXPECT_EQ( VelocityViewColour( vs, size ), glm::vec3( 0.0f ) );
    state.EndFrame( RDG::ExecuteReport{} );

    // The twin is the shader, and the view samples the frame's own velocity transient.
    const auto        root   = Desert::TestSupport::RepositoryRoot();
    const std::string shader = ReadFile( root / "Editor/Resources/Shaders/Programs/Debug/VelocityView.shader" );
    for ( const char* line :
          { "const vec2  ndc      = texelFetch(u_Velocity, texel, 0).xy;",
            "const vec2  pixels   = ndc * 0.5 * vec2(size);", "const float speed    = length(pixels);",
            "const float strength = clamp(speed / 16.0, 0.0, 1.0);",
            "const float hue      = atan(pixels.y, pixels.x) * (0.5 / 3.14159265) + 0.5;",
            "const vec3 k = abs(fract(vec3(hue) + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0;",
            "oColor               = vec4(speed > 0.0 ? HueToRgb(hue) * strength : vec3(0.0), 1.0);" } )
        EXPECT_NE( shader.find( line ), std::string::npos ) << "VelocityView.shader no longer carries: " << line;
    const std::string node =
         ReadFile( root / "Desert/Desert/Source/Engine/Graphic/SceneRendererFrameDeferred.cpp" );
    EXPECT_NE( node.find( "const RDG::TextureRef velocity = textures.Transients.Velocity;" ), std::string::npos );
    EXPECT_NE( node.find( "view->DeclareBindings( pass, velocity );" ), std::string::npos );
}
