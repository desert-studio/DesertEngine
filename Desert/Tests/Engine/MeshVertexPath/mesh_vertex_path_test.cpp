// One surface, several vertex paths — asserted as RELATIONS between the paths rather than as facts about
// each one.
//
// The defects this suite was written for are four faces of a single mistake: the vertex path was encoded
// as the material's C++ CLASS, so `MaterialService` resolved one `.demat` into exactly one path and every
// other path had nothing it could draw with.
//
//   1. an imported character with authored materials did not draw at all — MeshRenderer looked for a slot
//      whose parent was the skinned class, and the factory could not produce that class from an asset;
//   2. a skinned mesh cast no shadow — the (Skinned x ShadowDepth) shader did not exist, and the cascade
//      pass named the static queue instead of taking a path;
//   3. a skinned mesh ignored per-instance overrides — its Bind built the GPU material from the parent;
//   4. two skinned meshes sharing a material rendered in one pose — the bones lived on the material.
//
// None of the four is a wrong line in isolation, which is why every assertion below is about two things
// AGREEING:
//
//   * every cell of the (path x pass) table names a shader that EXISTS and calls itself that;
//   * every path that can be drawn can also be shadowed — the relation whose absence WAS defect (2);
//   * the forward variants declare one surface set and differ only by the ONE binding the path adds,
//     and that binding is the one `MeshPathOwnBinding` claims;
//   * the caster variants declare one caster set on the same terms;
//   * each push block is exactly as long as the last field the C++ writes into it — the only statement
//     reflection can make about a push block's members, and enough to fire if a field is inserted before
//     BoneOffset and the renderer starts writing the bone offset into MaterialIndex.
//
// No device: the shaders are compiled with shaderc and reflected with the engine's own reflection,
// exactly as Tests/Engine/PBRSceneFrame and Tests/Engine/ShaderCacheKey do.

#include <gtest/gtest.h>

#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanShaderReflection.hpp>
#include <Engine/Graphic/Materials/Mesh/MaterialShadow.hpp>
#include <Engine/Graphic/Materials/Mesh/MeshVertexPath.hpp>
#include <Engine/Graphic/Materials/Mesh/PBR/MaterialPBR.hpp>

#include <Common/Core/Constants.hpp>

#include <shaderc/shaderc.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using Desert::Core::Formats::ShaderStage;
using Desert::Graphic::MaterialPBR;
using Desert::Graphic::MaterialShadowSkinned;
using Desert::Graphic::MeshPass;
using Desert::Graphic::MeshPathOwnBinding;
using Desert::Graphic::MeshShaderFor;
using Desert::Graphic::MeshVertexPath;
using Desert::Graphic::MeshVertexPathName;
using namespace Desert::Graphic::API::Vulkan;

namespace
{
    constexpr MeshVertexPath kAllPaths[] = { MeshVertexPath::Static, MeshVertexPath::Skinned,
                                             MeshVertexPath::Instanced };

    // Where each mesh shader lives, so the table's NAME can be resolved to a file. Derived from the name
    // rather than listed beside it: a second list of the same shaders is exactly the drift this suite is
    // about, so the only thing recorded here is the directory a family lives in.
    std::filesystem::path ShaderFileFor( const char* shaderName )
    {
        const std::string           name( shaderName );
        const std::filesystem::path programs( "Resources/Shaders/Programs" );
        for ( const char* dir : { "PBR", "Shadow", "Silhouette" } )
        {
            const auto candidate = programs / dir / ( name + ".shader" );
            if ( std::filesystem::exists( candidate ) )
                return candidate;
        }
        return {};
    }

    std::string ReadFile( const std::filesystem::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

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

    // Resolves `#include <...>` exactly as ShaderIncluder does, so the SPIR-V under test is the SPIR-V the
    // engine compiles.
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

    // Both stages of a graphics shader folded into one reflection — which is what a material allocates and
    // what a pipeline layout is built from.
    ShaderResource::ReflectionData ReflectGraphics( const std::filesystem::path& shaderFile )
    {
        const auto vertexSpirv =
             CompileStage( StageSource( shaderFile, ShaderStage::Vertex ), shaderFile, shaderc_vertex_shader );
        const auto fragmentSpirv =
             CompileStage( StageSource( shaderFile, ShaderStage::Fragment ), shaderFile, shaderc_fragment_shader );

        ShaderResource::ReflectionData data;
        if ( vertexSpirv.empty() || fragmentSpirv.empty() )
            return data;

        auto diagnostics = ShaderReflection::ReflectStage( vertexSpirv, ShaderStage::Vertex, data );
        EXPECT_TRUE( diagnostics.empty() ) << ( diagnostics.empty() ? "" : diagnostics.front() );
        diagnostics = ShaderReflection::ReflectStage( fragmentSpirv, ShaderStage::Fragment, data );
        EXPECT_TRUE( diagnostics.empty() ) << ( diagnostics.empty() ? "" : diagnostics.front() );
        return data;
    }

    ShaderResource::ShaderDescriptorSet SetZero( const ShaderResource::ReflectionData& data )
    {
        const auto it = data.ShaderDescriptorSets.find( 0 );
        return it == data.ShaderDescriptorSets.end() ? ShaderResource::ShaderDescriptorSet{} : it->second;
    }

    // binding -> name, for every descriptor set 0 declares. Slot AND name, because the two halves of the
    // relations below are different: "the same surface" is about names (a material looks a property up by
    // name), and "the path's own binding" is about a slot number the C++ table states.
    std::map<uint32_t, std::string> BindingMap( const ShaderResource::ShaderDescriptorSet& set )
    {
        std::map<uint32_t, std::string> out;
        for ( const auto& [binding, resource] : set.UniformBuffers )
            out[binding] = resource.Name;
        for ( const auto& [binding, resource] : set.StorageBuffers )
            out[binding] = resource.Name;
        for ( const auto& [binding, resource] : set.Image2DSamplers )
            out[binding] = resource.Name;
        for ( const auto& [binding, resource] : set.ImageCubeSamplers )
            out[binding] = resource.Name;
        for ( const auto& [binding, resource] : set.Image3DSamplers )
            out[binding] = resource.Name;
        return out;
    }

    std::string Describe( const std::map<uint32_t, std::string>& bindings )
    {
        std::ostringstream out;
        for ( const auto& [binding, name] : bindings )
            out << binding << ':' << name << ' ';
        return out.str();
    }

    // The engine resolves `#include <...>` against a path relative to the editor's working directory.
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
} // namespace

// ---- The table against the tree ---------------------------------------------------------------------

// A cell that names a shader nobody shipped is a material that fails to build at runtime, in a log line
// somebody has to be looking at. Here it is a red test instead. The second half — the shader calling
// itself what the table calls it — is what a rename breaks: the DSL's `Shader "Name"` is what the shader
// service registers under, so a file renamed without its declaration (or the reverse) resolves to
// nothing.
TEST_F( ShaderRootFixture, EveryCellOfTheTableNamesAShaderThatExistsAndCallsItselfThat )
{
    for ( const auto path : kAllPaths )
    {
        for ( const auto pass : { MeshPass::Forward, MeshPass::GBuffer, MeshPass::Glass, MeshPass::ShadowDepth } )
        {
            const char* name = MeshShaderFor( path, pass );
            if ( !name )
                continue; // a deliberate hole; MeshVertexPath.hpp says why each one is one

            const auto file = ShaderFileFor( name );
            ASSERT_FALSE( file.empty() )
                 << "MeshShaderFor(" << MeshVertexPathName( path ) << ", " << Desert::Graphic::MeshPassName( pass )
                 << ") names '" << name << "', and no such .shader exists";

            const auto parsed = Desert::Core::Preprocess::DShaderParser::Parse( ReadFile( file ) );
            ASSERT_TRUE( parsed.IsSuccess() ) << file.string();
            EXPECT_EQ( parsed.GetValue().Name, std::string( name ) )
                 << file.string() << " declares itself '" << parsed.GetValue().Name
                 << "' but the table asks the shader service for '" << name << "'";
        }
    }
}

// THE relation defect (2) was the absence of. A mesh that the engine can DRAW is a mesh that occludes the
// sun, so a path with a forward shader and no caster shader is a class of geometry that is lit and casts
// nothing — which is exactly what a character did, standing in full sun over its own unshadowed ground.
//
// Stated over the paths rather than checked for the skinned one, because the next path added (terrain
// blades, decals, anything) inherits the requirement without anyone remembering it.
TEST_F( ShaderRootFixture, EveryVertexPathThatCanBeDrawnCanAlsoCastAShadow )
{
    for ( const auto path : kAllPaths )
    {
        if ( !MeshShaderFor( path, MeshPass::Forward ) )
            continue;
        EXPECT_NE( MeshShaderFor( path, MeshPass::ShadowDepth ), nullptr )
             << "the " << MeshVertexPathName( path )
             << " vertex path has a forward shader but no shadow-depth one, so geometry drawn on it is "
                "lit by the sun and casts nothing";
    }
}

// THE SECOND ABSENCE OF THE SAME KIND, AND IT COST NINE INVISIBLE CUBES.
//
// (Instanced x GBuffer) was a hole with a written reason: "instancing is disabled in the G-buffer pass,
// every static takes the per-object path there". That is true of the meshes MeshRenderer AUTO-BATCHES --
// when instancing is off they fall back to single draws -- and false of an InstancedStaticMesh entity,
// which is ONE entity holding N transforms and has no per-object path to fall back to. So the deferred
// pass dropped the entire ISM queue, with no line in the log, in the render path 81 of this
// repository's 88 scenes state. Measured on Resources/Assets/Scenes/G26_ISMProbe.desce: nine instances
// absent in Deferred and all nine present in Forward, from the same file.
//
// Stated over the paths, like the shadow rule above, with the ONE exception named and argued rather
// than the rule being written as "the instanced path must have a G-buffer cell". A path added tomorrow
// inherits the question.
TEST_F( ShaderRootFixture, EveryVertexPathDrawnINTOTheGBufferHasACellForIt )
{
    // The exception, and why it is one: a skinned mesh is not rasterized into the G-buffer at all. It is
    // drawn FORWARD over the deferred composite (MeshRenderer::RenderSkinnedManual), so a (Skinned x
    // GBuffer) cell would be a shader nothing binds.
    const auto drawnIntoTheGBuffer = []( MeshVertexPath path ) { return path != MeshVertexPath::Skinned; };

    for ( const auto path : kAllPaths )
    {
        if ( MeshShaderFor( path, MeshPass::Forward ) == nullptr || !drawnIntoTheGBuffer( path ) )
        {
            continue;
        }
        EXPECT_NE( MeshShaderFor( path, MeshPass::GBuffer ), nullptr )
             << "the " << MeshVertexPathName( path )
             << " vertex path is rasterized into the deferred G-buffer and has no shader for it, so "
                "every object on that path is missing from a deferred scene";
    }
}

// The instanced G-buffer cell is the static one plus the path's own binding, and NOTHING else. Both
// cells write the same four MRT targets from the same material payload; if they drifted apart, a scene
// would shade its ISM entities by one G-buffer contract and its plain meshes by another, and the only
// symptom would be that two cubes with one material look different.
TEST_F( ShaderRootFixture, TheInstancedGBufferCellIsTheStaticOnePlusItsOwnBinding )
{
    const char* staticName    = MeshShaderFor( MeshVertexPath::Static, MeshPass::GBuffer );
    const char* instancedName = MeshShaderFor( MeshVertexPath::Instanced, MeshPass::GBuffer );
    ASSERT_NE( staticName, nullptr );
    ASSERT_NE( instancedName, nullptr );

    auto staticBindings    = BindingMap( SetZero( ReflectGraphics( ShaderFileFor( staticName ) ) ) );
    auto instancedBindings = BindingMap( SetZero( ReflectGraphics( ShaderFileFor( instancedName ) ) ) );
    ASSERT_FALSE( staticBindings.empty() ) << staticName;
    ASSERT_FALSE( instancedBindings.empty() ) << instancedName;

    const auto own = MeshPathOwnBinding( MeshVertexPath::Instanced );
    ASSERT_TRUE( own.has_value() );
    const uint32_t ownBinding = own.value_or( 0 );
    EXPECT_TRUE( instancedBindings.count( ownBinding ) )
         << instancedName << " does not read binding " << ownBinding
         << ", so it is not fetching a per-instance model matrix at all";
    instancedBindings.erase( ownBinding );

    EXPECT_EQ( Describe( instancedBindings ), Describe( staticBindings ) )
         << "the two G-buffer cells declare different surfaces, so one of them is writing the G-buffer "
            "from data the other does not have";
}

// ---- One surface, one binding per path --------------------------------------------------------------

// The forward variants shade ONE surface, so their descriptor sets must be one set — differing only by
// what the path's own vertex stage reads, and at exactly the slot the C++ table claims for it.
//
// Both directions matter and only together. Without "declares its own", MeshPathOwnBinding could name a
// slot no shader uses and the C++ would be describing a shader that does not exist. Without "declares
// nobody else's", a surface binding could quietly move onto a path slot and the sets would still look
// equal after subtraction.
TEST_F( ShaderRootFixture, TheForwardVariantsAreOneSurfacePlusExactlyThePathsOwnBinding )
{
    std::map<MeshVertexPath, std::map<uint32_t, std::string>> bindings;
    for ( const auto path : kAllPaths )
    {
        const char* name = MeshShaderFor( path, MeshPass::Forward );
        ASSERT_NE( name, nullptr ) << MeshVertexPathName( path );
        bindings[path] = BindingMap( SetZero( ReflectGraphics( ShaderFileFor( name ) ) ) );
        ASSERT_FALSE( bindings[path].empty() ) << name;
    }

    // Every path's own binding is declared by that path...
    for ( const auto path : kAllPaths )
    {
        const auto own = MeshPathOwnBinding( path );
        if ( !own )
        {
            // Only the static path adds nothing — it reads its model matrix from the push constant.
            EXPECT_EQ( path, MeshVertexPath::Static );
            continue;
        }
        EXPECT_TRUE( bindings[path].count( *own ) ) << MeshVertexPathName( path ) << " claims binding " << *own
                                                    << " as its own, and its forward shader does not declare it";

        // ...and by nobody else, which is what lets one set-0 layout be shared across the paths.
        for ( const auto other : kAllPaths )
        {
            if ( other == path )
                continue;
            EXPECT_FALSE( bindings[other].count( *own ) )
                 << MeshVertexPathName( other ) << " declares binding " << *own << ", which belongs to the "
                 << MeshVertexPathName( path ) << " path";
        }
    }

    // And with each path's own slot removed, the three sets are the SAME surface — same slots, same names.
    std::map<MeshVertexPath, std::map<uint32_t, std::string>> surfaceOnly = bindings;
    for ( const auto path : kAllPaths )
        if ( const auto own = MeshPathOwnBinding( path ) )
            surfaceOnly[path].erase( *own );

    for ( const auto path : kAllPaths )
    {
        if ( path == MeshVertexPath::Static )
            continue;
        EXPECT_EQ( Describe( surfaceOnly[path] ), Describe( surfaceOnly[MeshVertexPath::Static] ) )
             << "the " << MeshVertexPathName( path )
             << " forward shader shades a different surface than the static one, so a single `.demat` "
                "cannot mean the same thing on both";
    }
}

// ---- The layout belongs to the CELL, not to a neighbouring cell -------------------------------------

// THE RELATION THIS REPLACED A COMMENT WITH. A `Graphic::Material` is one shader's descriptor sets plus a
// payload, and the shader is `MeshShaderFor(path, pass)` — so a descriptor layout is a property of the
// CELL. A pass that owns no material has to bind a neighbouring cell's sets against its own pipeline
// layout, which Vulkan tolerates only while the two shaders reflect identically, and the deferred
// G-buffer pass did exactly that: `StaticMeshGBuffer.shader` declared four cascade maps, three
// environment samplers, two light SSBOs, the lights-metadata block, the directional-light block, ShadowUB
// and the cloud-shadow pair — fourteen descriptors it never read — and multiplied all of them by 1e-20 so
// that SPIR-V reflection would keep them. `MaterialService` keys by (asset x path x PASS) now, so the
// pass binds its own cell's sets and the padding is gone.
//
// Two assertions, and the second is the one that fires if somebody puts the padding back:
//
//   * where two cells of ONE path declare the same slot, they must give it the same NAME. A material
//     writes by name and a pipeline binds by number, so a slot that means `u_NormalTexture` in one cell
//     and something else in another is a texture bound into the wrong sampler with nothing to notice it.
//   * the G-buffer cell is a PROPER subset of its path's forward cell. Subset, because it shades the same
//     surface and can read nothing the surface does not have; PROPER, because a G-buffer write reads no
//     lights, no cascades and no environment — and a G-buffer set that has grown back to the forward
//     set's size is a shader padded to borrow somebody else's descriptors.
TEST_F( ShaderRootFixture, TheGBufferCellIsAProperSubsetOfItsPathsForwardSurface )
{
    std::map<MeshPass, std::map<uint32_t, std::string>> bindings;
    for ( const auto pass : { MeshPass::Forward, MeshPass::GBuffer, MeshPass::Glass } )
    {
        const char* name = MeshShaderFor( MeshVertexPath::Static, pass );
        ASSERT_NE( name, nullptr ) << Desert::Graphic::MeshPassName( pass );
        bindings[pass] = BindingMap( SetZero( ReflectGraphics( ShaderFileFor( name ) ) ) );
        ASSERT_FALSE( bindings[pass].empty() ) << name;
    }

    // One numbering across the passes of a path.
    for ( const auto& [slot, name] : bindings[MeshPass::GBuffer] )
    {
        for ( const auto pass : { MeshPass::Forward, MeshPass::Glass } )
        {
            const auto it = bindings[pass].find( slot );
            if ( it == bindings[pass].end() )
                continue;
            EXPECT_EQ( it->second, name )
                 << "binding " << slot << " is '" << name << "' in the G-buffer cell and '" << it->second
                 << "' in the " << Desert::Graphic::MeshPassName( pass ) << " cell of the same path";
        }
    }

    // Subset...
    for ( const auto& [slot, name] : bindings[MeshPass::GBuffer] )
    {
        const auto it = bindings[MeshPass::Forward].find( slot );
        ASSERT_NE( it, bindings[MeshPass::Forward].end() )
             << "the G-buffer cell declares binding " << slot << " ('" << name
             << "') that its path's forward cell does not — the two shade one surface, so this is a "
                "resource no `.demat` can fill";
        EXPECT_EQ( it->second, name );
    }

    // ...and PROPER — for the G-buffer cell AND for the glass cell, which carried the same padding for the
    // same reason and had ALREADY had a material of its own when it was written. Glass is not a subset of
    // the forward cell (it reads the composited scene for refraction, which no forward binding is), so
    // what is asserted for both is the SIZE: a pass that declares as much as the lit forward pass is a
    // pass padded to keep another cell's material bindable against it, which is what the pass axis exists
    // to retire.
    for ( const auto pass : { MeshPass::GBuffer, MeshPass::Glass } )
    {
        EXPECT_LT( bindings[pass].size(), bindings[MeshPass::Forward].size() )
             << "the " << Desert::Graphic::MeshPassName( pass )
             << " cell declares as much as the forward cell: " << Describe( bindings[pass] );
    }
}

// The caster variants, on the same terms. A caster is depth, so its set is small; the point is that the
// skinned caster added the SKINNED path's binding and nothing else — a caster that quietly grew a surface
// binding would need the surface's descriptors bound to it, which the cascade pass does not do.
TEST_F( ShaderRootFixture, TheCasterVariantsAreOneCasterPlusExactlyThePathsOwnBinding )
{
    std::map<MeshVertexPath, std::map<uint32_t, std::string>> bindings;
    for ( const auto path : kAllPaths )
    {
        const char* name = MeshShaderFor( path, MeshPass::ShadowDepth );
        ASSERT_NE( name, nullptr ) << MeshVertexPathName( path );
        bindings[path] = BindingMap( SetZero( ReflectGraphics( ShaderFileFor( name ) ) ) );
        ASSERT_FALSE( bindings[path].empty() ) << name;
    }

    for ( const auto path : kAllPaths )
    {
        const auto own = MeshPathOwnBinding( path );
        if ( !own )
            continue;
        EXPECT_TRUE( bindings[path].count( *own ) )
             << "the " << MeshVertexPathName( path ) << " caster does not read its own binding " << *own
             << ", so it cannot be transforming its vertices the way the forward pass does — and a "
                "caster that disagrees with its own forward shader casts the wrong silhouette";
        bindings[path].erase( *own );
    }

    for ( const auto path : kAllPaths )
    {
        if ( path == MeshVertexPath::Static )
            continue;
        EXPECT_EQ( Describe( bindings[path] ), Describe( bindings[MeshVertexPath::Static] ) )
             << "the " << MeshVertexPathName( path )
             << " caster declares resources the plain caster does not; the cascade pass binds one "
                "material per cascade and nothing would fill them";
    }
}

// ---- The push block against the offsets the C++ writes at -------------------------------------------

// Reflection reports a push block's total SIZE but not its members, so this is the strongest statement
// available about the layout — and it is enough. Insert a field before BoneOffset and the block grows,
// which fires here; delete BoneOffset and it shrinks, which fires here; and either way the alternative is
// a renderer writing the bone offset over the material index and a character rendered with somebody
// else's albedo, silently.
TEST_F( ShaderRootFixture, EachPushBlockIsAsLongAsTheLastFieldTheRendererWritesIntoIt )
{
    struct Expectation
    {
        MeshVertexPath Path;
        MeshPass       Pass;
        uint32_t       Size;
    };

    const Expectation expectations[] = {
         { MeshVertexPath::Static, MeshPass::Forward, MaterialPBR::kPushSizeWithoutBones },
         { MeshVertexPath::Instanced, MeshPass::Forward, MaterialPBR::kPushSizeWithoutBones },
         { MeshVertexPath::Skinned, MeshPass::Forward, MaterialPBR::kPushSizeWithBones },
         { MeshVertexPath::Skinned, MeshPass::ShadowDepth, MaterialShadowSkinned::kPushSize },
    };

    for ( const auto& expected : expectations )
    {
        const char* name = MeshShaderFor( expected.Path, expected.Pass );
        ASSERT_NE( name, nullptr );

        const auto data = ReflectGraphics( ShaderFileFor( name ) );
        ASSERT_TRUE( data.PushConstantRanges.has_value() ) << name << " declares no push constant block";
        EXPECT_EQ( data.PushConstantRanges->Size, expected.Size )
             << name << "'s push block is " << data.PushConstantRanges->Size
             << " bytes, and the renderer writes its last field at offset " << ( expected.Size - 4 );
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
