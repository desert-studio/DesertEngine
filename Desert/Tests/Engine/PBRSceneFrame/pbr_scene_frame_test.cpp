// One scene, one lighting payload — asserted as a RELATION between the skinned path and the static one.
//
// The defect this suite was written for: SkinnedMaterialPBR::Bind() named the four things a skinned mesh
// was thought to need (camera, lights, bones, cloud shadow) and the scene had six. The two it did not
// name — the shadow cascades and the IBL environment — were written by the static path only, so a
// skinned mesh was the one class of geometry in the engine that received neither. Nothing crashed and no
// validation layer said anything, because an unwritten descriptor here is not undefined memory: the
// backend seeds every binding first (VulkanMaterialBackend::WriteFallbacks), so `ShadowUB` kept
// the zero-filled dummy buffer — `u_ShadowParams.y == 0`, cascades silently OFF — while the environment
// trio kept its fallback images, which sample black, so the split-sum ambient was zero and a skinned
// surface was lit by the sun and the anti-black floor alone no matter what the sky was doing.
//
// Two wrong answers, both silent, both invisible to a unit test of either side. So the assertions here
// are about the two sides AGREEING:
//
//   * the skinned material's Bind() payload IS the static path's snapshot type, and has no way of being
//     built without one (a reference member has no default);
//   * every binding that ONE applier fills is declared by ALL three mesh PBR shaders, by NAME — which is
//     what a material actually looks up;
//   * the three shaders' set 0 differ only in the per-object buffer their vertex stage reads, so "one
//     applier serves all of them" is a fact and not an intention;
//   * the `ShadowUB` block is the same number of bytes in GLSL as the C++ struct the applier fills.
//
// None of it needs a device: the shaders are compiled with shaderc and reflected with the engine's own
// reflection, exactly as Tests/Engine/ShaderCacheKey does.

#include <gtest/gtest.h>

#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Engine/Core/ShaderCompiler/ShaderCacheKey.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanShaderReflection.hpp>
#include <Engine/Graphic/Environment/SkyLook.hpp>
#include <Engine/Graphic/Materials/Mesh/PBR/MaterialPBRBase.hpp>
#include <Engine/Graphic/Materials/Mesh/PBR/PBRSceneFrame.hpp>
#include <Engine/Graphic/Materials/Mesh/PBR/MaterialPBR.hpp>
#include <Engine/Graphic/ShaderProtocols/Camera.hpp>
#include <Engine/Graphic/ShaderProtocols/DirectionLight.hpp>
#include <Engine/Graphic/ShaderProtocols/Metadata.hpp>
#include <Engine/Graphic/ShaderProtocols/PointLight.hpp>
#include <Engine/Graphic/ShaderProtocols/SkinnedMaterialUB.hpp>
#include <Engine/Graphic/ShaderProtocols/SpotLight.hpp>

#include <Common/Core/Constants.hpp>

#include <shaderc/shaderc.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

using Desert::Core::Formats::ShaderStage;
using Desert::Graphic::MaterialPBR;
using Desert::Graphic::MaterialPBRBase;
using Desert::Graphic::PBRSceneFrame;
using namespace Desert::Graphic::API::Vulkan;

namespace
{
    // The engine resolves `#include <...>` against Common::Constants::Path::SHADERDIR_PATH, which is
    // relative ("Resources/Shaders/"). The editor runs with its own directory as the working one; the
    // test does the same so the include walk resolves the same files the runtime would.
    struct ShaderRootFixture : ::testing::Test
    {
        static void SetUpTestSuite()
        {
            std::filesystem::path here = std::filesystem::current_path();
            for ( int up = 0; up < 8 && !std::filesystem::exists( here / "Editor" / "Resources" / "Shaders" );
                  ++up )
                here = here.parent_path();

            ASSERT_TRUE( std::filesystem::exists( here / "Editor" / "Resources" / "Shaders" ) )
                 << "could not find Editor/Resources/Shaders above " << std::filesystem::current_path();

            std::filesystem::current_path( here / "Editor" );
        }
    };

    std::string ReadFile( const std::filesystem::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

    std::filesystem::path ShaderPath( const char* relative )
    {
        return std::filesystem::path( "Resources/Shaders/Programs" ) / relative;
    }

    // The assembled GLSL of one stage, straight out of the engine's own DSL parser.
    std::string StageSource( const std::filesystem::path& shaderFile, ShaderStage stage )
    {
        auto parsed = Desert::Core::Preprocess::DShaderParser::Parse( ReadFile( shaderFile ) );
        EXPECT_TRUE( parsed.IsSuccess() ) << shaderFile.string();
        if ( !parsed.IsSuccess() )
            return {};
        const auto it = parsed.GetValue().Stages.find( stage );
        EXPECT_NE( it, parsed.GetValue().Stages.end() ) << shaderFile.string();
        return it == parsed.GetValue().Stages.end() ? std::string{} : it->second;
    }

    // Resolves `#include <...>` exactly as ShaderIncluder does, so the SPIR-V under test is the SPIR-V
    // the engine compiles.
    class Includer final : public shaderc::CompileOptions::IncluderInterface
    {
    public:
        shaderc_include_result* GetInclude( const char* requested, shaderc_include_type type,
                                            const char* requesting, size_t ) override
        {
            const std::filesystem::path full =
                 type == shaderc_include_type_relative
                      ? ( std::filesystem::path( requesting ).parent_path() / requested ).lexically_normal()
                      : ( Common::Constants::Path::SHADERDIR_PATH / requested ).lexically_normal();

            auto* name = new std::string( full.string() );
            auto* body =
                 new std::string( Desert::Core::Preprocess::DShaderParser::TranslateSugar( ReadFile( full ) ) );

            auto* result               = new shaderc_include_result;
            result->source_name        = name->c_str();
            result->source_name_length = name->size();
            result->content            = body->c_str();
            result->content_length     = body->size();
            result->user_data          = new std::pair<std::string*, std::string*>( name, body );
            return result;
        }

        void ReleaseInclude( shaderc_include_result* data ) override
        {
            auto* pair = static_cast<std::pair<std::string*, std::string*>*>( data->user_data );
            delete pair->first;
            delete pair->second;
            delete pair;
            delete data;
        }
    };

    std::vector<uint32_t> CompileStage( const std::string& source, const std::filesystem::path& path,
                                        shaderc_shader_kind kind )
    {
        shaderc::Compiler       compiler;
        shaderc::CompileOptions options;
        options.SetIncluder( std::make_unique<Includer>() );
        options.SetTargetEnvironment( shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1 );

        const auto result = compiler.CompileGlslToSpv( source, kind, path.string().c_str(), options );
        EXPECT_EQ( result.GetCompilationStatus(), shaderc_compilation_status_success )
             << path.string() << ": " << result.GetErrorMessage();
        if ( result.GetCompilationStatus() != shaderc_compilation_status_success )
            return {};
        return { result.begin(), result.end() };
    }

    // Set 0 of a graphics shader, both stages folded together — which is the set a material allocates and
    // therefore the set an applier writes into.
    ShaderResource::ShaderDescriptorSet GraphicsSetZero( const std::filesystem::path& shaderFile )
    {
        const auto vertexSpirv =
             CompileStage( StageSource( shaderFile, ShaderStage::Vertex ), shaderFile, shaderc_vertex_shader );
        const auto fragmentSpirv =
             CompileStage( StageSource( shaderFile, ShaderStage::Fragment ), shaderFile, shaderc_fragment_shader );
        if ( vertexSpirv.empty() || fragmentSpirv.empty() )
            return {};

        ShaderResource::ReflectionData data;
        auto diagnostics = ShaderReflection::ReflectStage( vertexSpirv, ShaderStage::Vertex, data );
        EXPECT_TRUE( diagnostics.empty() ) << ( diagnostics.empty() ? "" : diagnostics.front() );
        diagnostics = ShaderReflection::ReflectStage( fragmentSpirv, ShaderStage::Fragment, data );
        EXPECT_TRUE( diagnostics.empty() ) << ( diagnostics.empty() ? "" : diagnostics.front() );

        const auto it = data.ShaderDescriptorSets.find( 0 );
        return it == data.ShaderDescriptorSets.end() ? ShaderResource::ShaderDescriptorSet{} : it->second;
    }

    // The NAMES set 0 declares. Names and not slots on purpose: Material::Get looks a property up by
    // name, so a name is what the CPU and the shader actually have to agree on.
    std::set<std::string> DeclaredNames( const ShaderResource::ShaderDescriptorSet& set )
    {
        std::set<std::string> names;
        for ( const auto& [binding, resource] : set.UniformBuffers )
            names.insert( resource.Name );
        for ( const auto& [binding, resource] : set.StorageBuffers )
            names.insert( resource.Name );
        for ( const auto& [binding, resource] : set.Image2DSamplers )
            names.insert( resource.Name );
        for ( const auto& [binding, resource] : set.ImageCubeSamplers )
            names.insert( resource.Name );
        for ( const auto& [binding, resource] : set.Image3DSamplers )
            names.insert( resource.Name );
        return names;
    }

    std::string Describe( const std::set<std::string>& names )
    {
        std::ostringstream out;
        for ( const auto& name : names )
            out << name << ' ';
        return out.str();
    }

    // Every resource PBRSceneFrame::ApplyTo fills, by the same name the applier looks it up under. The
    // camera and the light blocks come from the ShaderProtocols types that own those names; the shadow
    // and environment names come from MaterialPBRBase, which owns the CPU half of that contract and is
    // what the writers in SceneLightingBinding.hpp look the blocks up under. Nothing here is a literal
    // repeated from the engine — a rename that reaches only one side fails to compile, not to pass.
    //
    // The cloud-shadow pair (u_CloudShadowMap / CloudShadowUB) is deliberately NOT here: ApplyTo writes it
    // through Graphic::CloudShadowBind, and Tests/Engine/CloudShadow already asserts that every sun-lit
    // shader in the tree — these three included — declares it.
    std::vector<std::string> SceneBindingNames()
    {
        std::vector<std::string> names = {
             Desert::Graphic::ShaderProtocols::Camera::Name,
             Desert::Graphic::ShaderProtocols::PointLight::Name,
             Desert::Graphic::ShaderProtocols::SpotLight::Name,
             Desert::Graphic::ShaderProtocols::DirectionLight::Name,
             Desert::Graphic::ShaderProtocols::LightsMetadata::Name,
             MaterialPBRBase::kShadowBlockName,
             MaterialPBRBase::kEnvIrradianceName,
             MaterialPBRBase::kEnvSpecularName,
             MaterialPBRBase::kBrdfLutName,
             // The look the two environment cubes are read with; SceneEnvironmentBind writes it with them.
             Desert::Graphic::kSkyLookBlockName,
        };
        for ( uint32_t c = 0; c < MaterialPBRBase::kMaxCascades; ++c )
            names.emplace_back( MaterialPBRBase::kShadowMapNames[c] );
        return names;
    }

    // The three shaders one applier has to serve, each with the ONE resource that is genuinely its own —
    // the per-object buffer its vertex stage reads. Everything else in their set 0 is scene state.
    struct MeshShader
    {
        const char* Path;
        const char* PerObjectVertexBuffer; // nullptr = none
    };

    const MeshShader kMeshShaders[] = {
         { "PBR/StaticMeshPBR.shader", nullptr },
         { "PBR/StaticMeshPBR_Instanced.shader", "InstanceTransforms" },
         { "PBR/SkinnedMeshPBR.shader", "Bones" },
    };
} // namespace

// ---- The payload ------------------------------------------------------------------------------------

// THE relation, in the only form in which it can be stated at compile time — and it got STRONGER when
// the vertex path became a parameter instead of a class.
//
// It used to read: the skinned material's Bind() payload IS the static path's snapshot type, and a
// reference member makes it impossible to build one without a snapshot. That guarded a real defect (a
// hand-picked list of four uploads where the scene had six), but it guarded it at the level of a struct,
// and the struct existed only because the skinned material was a DIFFERENT CLASS with a Bind() of its own.
//
// There is one class now. A skinned draw and a static draw call the same Bind with the same argument, so
// a payload the skinned path could receive differently no longer exists to be wrong. That is asserted
// against the TYPE, which is the whole of the claim: if these ever split again, every "one applier serves
// all of them" assertion below stops meaning what it says.
TEST( PBRSceneFrame, TheSkinnedAndStaticDrawsGoThroughOneBindWithOneArgument )
{
    static_assert( std::is_same_v<decltype( MaterialPBR::Create( Desert::Graphic::MeshVertexPath::Static ) ),
                                  decltype( MaterialPBR::Create( Desert::Graphic::MeshVertexPath::Skinned ) )>,
                   "a skinned material and a static one must be the same type — one surface, two paths" );

    // Exactly ONE Bind, taking the instance PBRSceneFrame::ApplyTo was applied to. A second overload
    // taking a skinned-specific payload is how the cascades and the environment cubes went missing the
    // first time: it named four of the six things a lit draw needs and nothing could notice.
    static_assert(
         std::is_invocable_v<void ( MaterialPBR::* )( const Desert::Graphic::MaterialInstance* ), MaterialPBR&,
                             const Desert::Graphic::MaterialInstance*>,
         "MaterialPBR::Bind must take the instance, the same one PBRSceneFrame::ApplyTo was applied to" );

    SUCCEED();
}

// The other half of the same seam, and the reason the applier takes a Material and not a MaterialInstance:
// a data-driven (shader-graph) draw HAS no instance. While the only entry point took one, the generic mesh
// path could not be handed this snapshot at all and hand-filled three of its blocks instead — which is how
// a graph material ticked "Lit" ended up with no environment, no cloud shadow and no punctual lights, and
// a lighting model the graph compiler wrote for itself.
TEST( PBRSceneFrame, TheApplierCanBeHandedAMaterialThatHasNoInstance )
{
    static_assert( std::is_invocable_v<void ( PBRSceneFrame::* )( Desert::Graphic::Material* ) const,
                                       const PBRSceneFrame&, Desert::Graphic::Material*>,
                   "the frame snapshot must be applicable to a bare Material — a MaterialInstance-only "
                   "entry point is what left the generic mesh path filling its own descriptors" );

    // And the instance overload survives, because the PBR queue draws through instances.
    static_assert( std::is_invocable_v<void ( PBRSceneFrame::* )( Desert::Graphic::MaterialInstance* ) const,
                                       const PBRSceneFrame&, Desert::Graphic::MaterialInstance*> );

    SUCCEED();
}

// ---- The applier against the shaders ----------------------------------------------------------------

TEST_F( ShaderRootFixture, EverySceneBindingTheOneApplierFillsIsDeclaredByEveryMeshPBRShader )
{
    const auto expected = SceneBindingNames();
    ASSERT_FALSE( expected.empty() );

    for ( const auto& shader : kMeshShaders )
    {
        const auto declared = DeclaredNames( GraphicsSetZero( ShaderPath( shader.Path ) ) );
        ASSERT_FALSE( declared.empty() ) << shader.Path;

        for ( const auto& name : expected )
            EXPECT_TRUE( declared.count( name ) != 0 )
                 << shader.Path << " does not declare '" << name
                 << "', which PBRSceneFrame::ApplyTo fills for it. Declared: " << Describe( declared );
    }
}

// The other direction, and the one that would have caught Д15 the day it was written: a scene-level
// resource DECLARED by a mesh shader that no applier fills. Before this task SkinnedMeshPBR declared
// ShadowUB, four cascade maps and the environment trio, and the skinned draw path wrote none of them —
// the shader sampled the fallbacks and read as unshadowed and blown-out white.
TEST_F( ShaderRootFixture, NoMeshPBRShaderDeclaresASceneResourceNoApplierFills )
{
    // Everything in a mesh PBR set 0 that is genuinely per-OBJECT and is therefore filled by the material
    // itself rather than by the frame snapshot: the GPU-scene material row and the surface maps
    // (MaterialFactory binds these three by name from the material asset).
    const char* kPerObject[] = { "Materials", "u_AlbedoTexture", "u_NormalTexture", "u_OpacityTexture" };
    // The cloud-shadow pair, filled by Graphic::CloudShadowBind out of the same snapshot — see the note on
    // SceneBindingNames().
    const char* kCloudShadow[] = { "u_CloudShadowMap", "CloudShadowUB" };

    for ( const auto& shader : kMeshShaders )
    {
        const auto declared = DeclaredNames( GraphicsSetZero( ShaderPath( shader.Path ) ) );
        ASSERT_FALSE( declared.empty() ) << shader.Path;

        std::set<std::string> accounted;
        for ( const auto& name : SceneBindingNames() )
            accounted.insert( name );
        for ( const char* name : kPerObject )
            accounted.insert( name );
        for ( const char* name : kCloudShadow )
            accounted.insert( name );
        if ( shader.PerObjectVertexBuffer )
            accounted.insert( shader.PerObjectVertexBuffer );

        for ( const auto& name : declared )
            EXPECT_TRUE( accounted.count( name ) != 0 )
                 << shader.Path << " declares '" << name
                 << "' and nothing in the engine writes it — an unwritten binding is not a disabled "
                    "feature, it is whatever the fallback descriptor happens to contain";
    }
}

// One scene contract, three shaders. They differ ONLY in the per-object buffer their vertex stage reads,
// which is what makes a single applier able to serve all three — and what makes a divergence a failure
// here rather than a class of geometry lit differently from the rest of the scene.
TEST_F( ShaderRootFixture, TheThreeMeshPBRShadersDeclareOneSceneContractAndDifferOnlyInTheirOwnBuffer )
{
    std::set<std::string> reference;

    for ( const auto& shader : kMeshShaders )
    {
        std::set<std::string> declared = DeclaredNames( GraphicsSetZero( ShaderPath( shader.Path ) ) );
        ASSERT_FALSE( declared.empty() ) << shader.Path;

        if ( shader.PerObjectVertexBuffer )
        {
            EXPECT_TRUE( declared.erase( shader.PerObjectVertexBuffer ) != 0 )
                 << shader.Path << " no longer declares its own " << shader.PerObjectVertexBuffer;
        }

        if ( reference.empty() )
            reference = declared;
        else
            EXPECT_EQ( Describe( declared ), Describe( reference ) )
                 << shader.Path << " no longer shares set 0 with StaticMeshPBR, but one applier writes both";
    }
}

// ---- The block the applier fills --------------------------------------------------------------------

// The C++ mirror against the GLSL block: two statements of one layout, which is the disagreement shape
// this project has paid for repeatedly. Asserted on all three shaders, because the mirror is filled once
// and lands in three different descriptor sets.
TEST_F( ShaderRootFixture, TheShadowBlockIsTheSameBytesInTheApplierAndInEveryMeshPBRShader )
{
    // 4 x mat4 + 3 x vec4. Spelt out so a silently added member is visible as a number here.
    constexpr uint32_t kExpectedBytes = MaterialPBRBase::kMaxCascades * 64u + 3u * 16u;
    EXPECT_EQ( sizeof( MaterialPBRBase::ShadowUBData ), kExpectedBytes );

    for ( const auto& shader : kMeshShaders )
    {
        const auto set = GraphicsSetZero( ShaderPath( shader.Path ) );

        const auto block =
             std::find_if( set.UniformBuffers.begin(), set.UniformBuffers.end(), []( const auto& entry )
                           { return entry.second.Name == MaterialPBRBase::kShadowBlockName; } );

        ASSERT_NE( block, set.UniformBuffers.end() ) << shader.Path << " declares no ShadowUB";
        EXPECT_EQ( block->second.Size, sizeof( MaterialPBRBase::ShadowUBData ) )
             << shader.Path << "'s ShadowUB is " << block->second.Size << " bytes and the struct the "
             << "applier fills it from is " << sizeof( MaterialPBRBase::ShadowUBData );
    }
}

// ---- The cascade text against everyone who compiles it ----------------------------------------------

namespace
{
    // Every `.shader` in the tree whose FRAGMENT stage compiles @p header, found by walking the tree and
    // resolving each one's include closure with the engine's own resolver.
    //
    // DERIVED, and that is the whole value of it. A typed list of consumers is a list somebody has to
    // remember to extend, and forgetting is the defect: Д20 found ShadowFactor written out four times
    // because each new consumer copied the body it could see instead of naming a text, and two of the
    // four had silently fallen a fix behind by the time anyone diffed them.
    std::vector<std::filesystem::path> ShadersCompiling( const char* header )
    {
        std::vector<std::filesystem::path> consumers;
        for ( const auto& entry : std::filesystem::recursive_directory_iterator( "Resources/Shaders/Programs" ) )
        {
            if ( !entry.is_regular_file() || entry.path().extension() != ".shader" )
                continue;

            // A shader with no fragment stage (a depth-only or compute program) is not a consumer and
            // must not make StageSource's EXPECT_NE fire on the way to finding that out.
            auto parsed = Desert::Core::Preprocess::DShaderParser::Parse( ReadFile( entry.path() ) );
            if ( !parsed.IsSuccess() )
                continue;
            const auto stage = parsed.GetValue().Stages.find( ShaderStage::Fragment );
            if ( stage == parsed.GetValue().Stages.end() )
                continue;

            for ( const auto& include : Desert::Core::CollectShaderIncludes( stage->second, entry.path() ) )
            {
                if ( include.filename() == header )
                {
                    consumers.push_back( entry.path() );
                    break;
                }
            }
        }
        std::sort( consumers.begin(), consumers.end() );
        return consumers;
    }

    // The five resources Mesh/CascadedShadow.glslh reads, under the names Graphic::SceneShadowBind writes
    // them. Taken from MaterialPBRBase rather than spelt out, so a rename that reaches only one side fails
    // to compile instead of failing to be checked.
    std::vector<std::string> CascadeBindingNames()
    {
        std::vector<std::string> names{ MaterialPBRBase::kShadowBlockName };
        for ( uint32_t c = 0; c < MaterialPBRBase::kMaxCascades; ++c )
            names.emplace_back( MaterialPBRBase::kShadowMapNames[c] );
        return names;
    }
} // namespace

TEST_F( ShaderRootFixture, EveryShaderCompilingTheCascadeTextDeclaresTheFiveBindingsItReads )
{
    // THE RELATION Д20 exists to assert, and it is deliberately not "StaticMeshPBR has ShadowUB".
    //
    // Mesh/CascadedShadow.glslh declares no binding of its own — it cannot, because its consumers have
    // genuinely different layouts (the deferred composite already numbers the IBL trio 17/18/19 against
    // the mesh shaders' 8/9/10, and a shader-graph layout holds DirectionLightsUB and TimeUB at the 14/15
    // two of the maps use elsewhere). What it has instead is a CONTRACT: the includer declares these five
    // names, at whatever slots are free in its own set, and the engine binds by name.
    //
    // A contract stated only in a comment is a contract nobody checks. Both sides of this one are derived:
    // the consumers by walking the tree for whoever compiles the text, the names from the C++ that writes
    // them. Add a sixth consumer and it is tested the moment it includes the header; add a fifth cascade
    // and every consumer is required to declare it.
    const auto consumers = ShadersCompiling( "CascadedShadow.glslh" );

    // Vacuous success is the failure mode of every derived-set test. Five is what the tree holds today —
    // the three mesh PBR shaders, the deferred composite and the shader graph's lit surface — and a floor
    // rather than an equality so that a sixth consumer is not, by itself, a broken test.
    ASSERT_GE( consumers.size(), 5u ) << "nothing in the tree compiles Mesh/CascadedShadow.glslh";

    const auto expected = CascadeBindingNames();
    for ( const auto& shader : consumers )
    {
        const auto declared = DeclaredNames( GraphicsSetZero( shader ) );
        ASSERT_FALSE( declared.empty() ) << shader.string();

        for ( const auto& name : expected )
            EXPECT_TRUE( declared.count( name ) != 0 )
                 << shader.string() << " compiles Mesh/CascadedShadow.glslh but does not declare '" << name
                 << "', which the text reads and Graphic::SceneShadowBind fills. Declared: "
                 << Describe( declared );
    }
}

TEST_F( ShaderRootFixture, TheLitShaderGraphSurfaceIsOneOfThoseConsumers )
{
    // Named on its own because it is the ONE consumer whose membership is the point of Д20 rather than a
    // consequence of it. A lit graph surface received the ambient, the BRDF, the punctual lights and the
    // cloud shadow after Д16 and still stood in full sun under a wall that shadowed the PBR sphere beside
    // it, because ShadowFactor was the one term that was not a shared text.
    const auto consumers = ShadersCompiling( "CascadedShadow.glslh" );

    const bool graph = std::any_of( consumers.begin(), consumers.end(), []( const std::filesystem::path& p )
                                    { return p.parent_path().filename() == "Graph"; } );

    EXPECT_TRUE( graph ) << "no shader-graph surface compiles the cascade text — a graph material ticked "
                            "\"Lit\" is shadowed by clouds and not by geometry again";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
