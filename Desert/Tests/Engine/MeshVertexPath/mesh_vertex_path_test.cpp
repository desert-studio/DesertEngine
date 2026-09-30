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

#include <Engine/Core/Formats/MaterialParamRow.hpp>
#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanShaderReflection.hpp>
#include <Engine/Graphic/InstanceWind.hpp>
#include <Engine/Graphic/Materials/MaterialBinder.hpp>
#include <Engine/Graphic/Materials/Mesh/MaterialShadow.hpp>
#include <Engine/Graphic/Materials/Mesh/MeshVertexPath.hpp>

#include <Common/Content/ShaderAssetHeader.hpp>
#include <Common/Core/Constants.hpp>

#include <shaderc/shaderc.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using Desert::Core::Formats::ShaderStage;
using Desert::Graphic::MaterialShadowSkinned;
using Desert::Graphic::MeshPass;
using Desert::Graphic::MeshPathOwnBinding;
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
    //
    // A table entry is either a program ("StaticMeshGlass") or a CELL of a surface template
    // ("StandardSurface/Static.Forward", DShaderParser's SurfaceCellName): the file is named by the part
    // before the slash, the cell by the part after it.
    std::string TemplateOf( const char* tableName )
    {
        const std::string name( tableName );
        return name.substr( 0, name.find( '/' ) );
    }

    std::string CellOf( const char* tableName )
    {
        const std::string name( tableName );
        const size_t      slash = name.find( '/' );
        return slash == std::string::npos ? std::string() : name.substr( slash + 1 );
    }

    std::filesystem::path ShaderFileFor( const char* shaderName )
    {
        const std::string           name = TemplateOf( shaderName );
        const std::filesystem::path programs( "Resources/Shaders/Programs" );
        for ( const char* dir : { "PBR", "Silhouette", "Unlit" } )
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

    // THE TEMPLATE THE FILE-LEVEL RELATIONS ARE ASSERTED ON, found BY ROLE as the engine finds it: the one shipped
    // shader declaring `Default Surface` (ReadShaderManifest), never a name written here.
    const std::string& DefaultTemplate()
    {
        static const std::string name = []
        {
            std::vector<std::string> found;
            for ( const auto& entry :
                  std::filesystem::recursive_directory_iterator( "Resources/Shaders/Programs" ) )
                if ( entry.path().extension() == ".shader" )
                    if ( const auto m = Common::Content::ReadShaderManifest( ReadFile( entry.path() ) );
                         m && m.GetValue().DefaultSurface )
                        found.push_back( entry.path().stem().string() );
            EXPECT_EQ( found.size(), 1u ) << "exactly one shipped shader may declare `Default Surface`";
            return found.empty() ? std::string() : found.front();
        }();
        return name;
    }

    // The (path x pass) shader of the default template (of the glass template for Glass), or nullptr for a hole.
    const char* TableShader( MeshVertexPath path, Desert::Graphic::MeshPass pass )
    {
        static std::map<std::pair<int, int>, std::optional<std::string>> cache;
        auto& slot = cache[{ static_cast<int>( path ), static_cast<int>( pass ) }];
        if ( !slot )
            // The translucency pass is drawn by a TRANSLUCENT template's own cell; the shipped one is the glass.
            slot = Desert::Graphic::MeshShaderFor( pass == Desert::Graphic::MeshPass::Glass
                                                        ? std::string( "StaticMeshGlass" )
                                                        : DefaultTemplate(),
                                                   path, pass )
                        .value_or( std::string() );
        return slot->empty() ? nullptr : slot->c_str();
    }

    // The stages of the program the table names: the default program, or the named cell/pass.
    std::unordered_map<ShaderStage, std::string> StagesOf( const Desert::Core::Preprocess::DShaderParseResult& parsed,
                                                           const std::string&                                cell )
    {
        if ( cell.empty() )
            return parsed.Stages;
        for ( const auto& pass : parsed.Passes )
            if ( pass.Name == cell )
                return pass.Stages;
        ADD_FAILURE() << parsed.Name << " has no cell '" << cell << "'";
        return {};
    }

    std::string StageSource( const char* tableName, ShaderStage stage )
    {
        const auto file   = ShaderFileFor( tableName );
        auto       parsed = Desert::Core::Preprocess::DShaderParser::Parse( ReadFile( file ) );
        EXPECT_TRUE( parsed.IsSuccess() ) << file.string();
        if ( !parsed.IsSuccess() )
            return {};
        const auto stages = StagesOf( parsed.GetValue(), CellOf( tableName ) );
        const auto it     = stages.find( stage );
        EXPECT_NE( it, stages.end() ) << tableName;
        return it == stages.end() ? std::string{} : it->second;
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
    ShaderResource::ReflectionData ReflectGraphics( const char* tableName )
    {
        const auto shaderFile = ShaderFileFor( tableName );
        const auto vertexSpirv =
             CompileStage( StageSource( tableName, ShaderStage::Vertex ), shaderFile, shaderc_vertex_shader );
        const auto fragmentSpirv =
             CompileStage( StageSource( tableName, ShaderStage::Fragment ), shaderFile, shaderc_fragment_shader );

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
            const char* name = TableShader( path, pass );
            if ( !name )
                continue; // a deliberate hole; MeshVertexPath.hpp says why each one is one

            const auto file = ShaderFileFor( name );
            ASSERT_FALSE( file.empty() )
                 << "TableShader(" << MeshVertexPathName( path ) << ", " << Desert::Graphic::MeshPassName( pass )
                 << ") names '" << name << "', and no such .shader exists";

            const auto parsed = Desert::Core::Preprocess::DShaderParser::Parse( ReadFile( file ) );
            ASSERT_TRUE( parsed.IsSuccess() ) << file.string();
            EXPECT_EQ( parsed.GetValue().Name, TemplateOf( name ) )
                 << file.string() << " declares itself '" << parsed.GetValue().Name
                 << "' but the table asks the shader service for '" << name << "'";
            const std::string cell = CellOf( name );
            if ( !cell.empty() )
            {
                const auto& passes = parsed.GetValue().Meta.PassNames;
                EXPECT_NE( std::find( passes.begin(), passes.end(), cell ), passes.end() )
                     << file.string() << " expands into no cell '" << cell << "' ('" << name << "')";
            }
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
        if ( !TableShader( path, MeshPass::Forward ) )
            continue;
        EXPECT_NE( TableShader( path, MeshPass::ShadowDepth ), nullptr )
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
        if ( TableShader( path, MeshPass::Forward ) == nullptr || !drawnIntoTheGBuffer( path ) )
        {
            continue;
        }
        EXPECT_NE( TableShader( path, MeshPass::GBuffer ), nullptr )
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
    const char* staticName    = TableShader( MeshVertexPath::Static, MeshPass::GBuffer );
    const char* instancedName = TableShader( MeshVertexPath::Instanced, MeshPass::GBuffer );
    ASSERT_NE( staticName, nullptr );
    ASSERT_NE( instancedName, nullptr );

    auto staticBindings    = BindingMap( SetZero( ReflectGraphics( staticName ) ) );
    auto instancedBindings = BindingMap( SetZero( ReflectGraphics( instancedName ) ) );
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
        const char* name = TableShader( path, MeshPass::Forward );
        ASSERT_NE( name, nullptr ) << MeshVertexPathName( path );
        bindings[path] = BindingMap( SetZero( ReflectGraphics( name ) ) );
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
// payload, and the shader is `TableShader(path, pass)` — so a descriptor layout is a property of the
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
        const char* name = TableShader( MeshVertexPath::Static, pass );
        ASSERT_NE( name, nullptr ) << Desert::Graphic::MeshPassName( pass );
        bindings[pass] = BindingMap( SetZero( ReflectGraphics( name ) ) );
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
        const char* name = TableShader( path, MeshPass::ShadowDepth );
        ASSERT_NE( name, nullptr ) << MeshVertexPathName( path );
        bindings[path] = BindingMap( SetZero( ReflectGraphics( name ) ) );
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

// ---- Foliage wind: one offset for every pass that draws an instance (FO-7) --------------------------

// The shadow and the depth a swaying plant leaves must be the shadow and the depth of the plant that is
// drawn. That holds only if every instanced vertex stage reaches its position through the SAME function,
// so this asserts it three ways per cell: the stage includes Common/FoliageWind.glslh, calls
// InstancedWorldPosition with the push block's wind, and projects THAT position (no second
// `model * a_Position` path beside it); and the compiled SPIR-V really contains the function.
TEST_F( ShaderRootFixture, EveryInstancedVertexStagePositionsThroughTheOneWindFunction )
{
    for ( const MeshPass pass : { MeshPass::Forward, MeshPass::GBuffer, MeshPass::ShadowDepth } )
    {
        const char* name = TableShader( MeshVertexPath::Instanced, pass );
        ASSERT_NE( name, nullptr );
        const auto  file  = ShaderFileFor( name );
        std::string vertex = StageSource( name, ShaderStage::Vertex );
        // A template cell's vertex stage is the path's engine header, included; what is asserted is that
        // header's text. Whitespace is dropped on both sides: the claims are about calls, not layout.
        if ( !CellOf( name ).empty() )
            vertex += ReadFile( Common::Constants::Path::SHADERDIR_PATH /
                                Desert::Core::Preprocess::SurfaceVertexInclude( "Instanced" ) );
        vertex.erase( std::remove_if( vertex.begin(), vertex.end(), []( char c ) { return c == ' '; } ),
                      vertex.end() );

        EXPECT_NE( vertex.find( "#include<Common/FoliageWind.glslh>" ), std::string::npos ) << name;
        EXPECT_NE( vertex.find( "InstancedWorldPosition(model,a_Position,m_PushConstants.WindA,"
                                "m_PushConstants.WindB)" ),
                   std::string::npos )
             << name << " does not position its vertex through the shared wind function";
        EXPECT_NE( vertex.find( "gl_Position=cameraUB.Projection*cameraUB.View*vec4(worldPosition,1.0);" ),
                   std::string::npos )
             << name << " projects something other than the wind-displaced position";
        EXPECT_EQ( vertex.find( "model*vec4(a_Position" ), std::string::npos )
             << name << " still computes an undisplaced position beside the shared one";

        // Compiled from the stage as assembled — `vertex` above is a whitespace-free text for the finds.
        const auto        spirv = CompileStage( StageSource( name, ShaderStage::Vertex ), file, shaderc_vertex_shader );
        const std::string words( reinterpret_cast<const char*>( spirv.data() ),
                                 spirv.size() * sizeof( uint32_t ) );
        EXPECT_NE( words.find( "FoliageWindOffset(" ), std::string::npos )
             << name << "'s SPIR-V has no FoliageWindOffset";
    }
}

// ---- MaterialLayout: one layout, held to every compiled stage (MAT1h) ---------------------------------

namespace
{
    using Desert::Core::Formats::MaterialLayout;

    struct ReconciledCell
    {
        MaterialLayout           Layout;
        std::vector<std::string> Errors;
    };

    // Parse -> the layout the generator wrote from -> compile each stage -> reflect -> reconcile. The same
    // road a template takes at runtime, with nothing in between that knows any field by name.
    ReconciledCell Reconcile( const std::string& source, const std::string& label, const std::string& cell = {} )
    {
        auto parsed = Desert::Core::Preprocess::DShaderParser::Parse( source );
        EXPECT_TRUE( parsed.IsSuccess() ) << label;
        if ( !parsed.IsSuccess() )
            return {};
        std::vector<Desert::Core::ShaderMapStage> stages;
        const auto                                cellStages = StagesOf( parsed.GetValue(), cell );
        for ( const auto [stage, kind] : { std::pair{ ShaderStage::Vertex, shaderc_vertex_shader },
                                           std::pair{ ShaderStage::Fragment, shaderc_fragment_shader } } )
        {
            const auto it = cellStages.find( stage );
            if ( it == cellStages.end() )
                continue;
            stages.push_back( { stage, CompileStage( it->second, label, kind ) } );
            EXPECT_FALSE( stages.back().Spirv.empty() ) << label;
        }
        // THE LOAD PATH: VulkanShader::BuildFromSpirv calls exactly this, and refuses the cell on any error.
        auto reconciled =
             ShaderReflection::ReconcileCellLayout( parsed.GetValue().Meta, stages, parsed.GetValue().Name, cell );
        return { std::move( reconciled.Layout ), std::move( reconciled.Errors ) };
    }

    std::string MockTemplate( const std::string& extraProperties, const std::string& vertexPush,
                              const std::string& fragmentPush )
    {
        return R"(Shader "LayoutMock"
{
    Domain Surface
    Properties Binding(30) TextureBinding(31)
    {
        Color     Tint    ("Tint")    = (1, 1, 1, 1)
        Float     Tiling  ("Tiling")  = 2
)" + extraProperties +
               R"(        Texture2D u_Mask ("Mask") = "white"
    }
    Vertex
    {
)" + vertexPush +
               R"(        void main() { gl_Position = m_PushConstants.Transform * vec4( float( m_PushConstants.MaterialIndex ) ); }
    }
    Fragment
    {
)" + fragmentPush +
               R"(        layout( location = 0 ) out vec4 o_Color;
        void main() { o_Color = texture( u_Mask, vec2( 0.5 ) ) * u_Material.Tint * u_Material.Tiling; }
    }
}
)";
    }

    std::string PushMockTemplate( const std::string& vertexPush, const std::string& fragmentPush )
    {
        return R"(Shader "PushMock"
{
    Domain Surface
    Properties TextureBinding(31)
    {
        Texture2D u_Mask ("Mask") = "white"
    }
    Vertex
    {
        )" + vertexPush +
               R"(
        void main() { gl_Position = m_PushConstants.Transform * vec4( float( m_PushConstants.MaterialIndex ) ); }
    }
    Fragment
    {
        )" + fragmentPush +
               R"(
        layout( location = 0 ) out vec4 o_Color;
        void main() { o_Color = texture( u_Mask, vec2( 0.5 ) ) * float( m_PushConstants.MaterialIndex ); }
    }
}
)";
    }

    const std::string kTransportInclude = "        #include <Common/MaterialTransport.glslh>\n";
} // namespace

// The template is the only input: its parameters, its textures and the push block its include declares all
// arrive in the layout with their compiled offsets, and the compiled stages agree with it.
TEST_F( ShaderRootFixture, AMockTemplatesLayoutIsItsPropertiesAndItsPushBlock )
{
    auto cell = Reconcile( MockTemplate( "", kTransportInclude, "" ), "LayoutMock" );
    EXPECT_TRUE( cell.Errors.empty() ) << ( cell.Errors.empty() ? "" : cell.Errors.front() );

    if ( !cell.Layout.RowBinding.has_value() )
        FAIL() << "the layout has no row binding";
    EXPECT_EQ( *cell.Layout.RowBinding, 30u );
    ASSERT_NE( cell.Layout.FindParam( "Tiling" ), nullptr );
    EXPECT_EQ( cell.Layout.FindParam( "Tiling" )->Offset, 16u );
    ASSERT_NE( cell.Layout.FindTexture( "u_Mask" ), nullptr );
    EXPECT_EQ( cell.Layout.FindTexture( "u_Mask" )->Binding, 31u );

    const auto* index = cell.Layout.FindPush( "MaterialIndex" );
    ASSERT_NE( index, nullptr );
    EXPECT_EQ( index->Offset, 64u );
    EXPECT_EQ( static_cast<uint32_t>( index->Stages ),
               static_cast<uint32_t>( ShaderStage::Vertex ) | static_cast<uint32_t>( ShaderStage::Fragment ) );
}

// A parameter written into the template reaches the layout, at its slot, with no C++ naming it.
TEST_F( ShaderRootFixture, ANewTemplateParameterAppearsInTheLayoutWithoutACppEdit )
{
    auto cell = Reconcile( MockTemplate( "        Float     Gloss   (\"Gloss\")   = 1\n", kTransportInclude, "" ),
                           "LayoutMock" );
    EXPECT_TRUE( cell.Errors.empty() ) << ( cell.Errors.empty() ? "" : cell.Errors.front() );
    ASSERT_NE( cell.Layout.FindParam( "Gloss" ), nullptr );
    EXPECT_EQ( cell.Layout.FindParam( "Gloss" )->Offset, 2 * Desert::Core::Formats::kMaterialParamSlotSize );
    EXPECT_EQ( cell.Layout.RowStride, 3 * Desert::Core::Formats::kMaterialParamSlotSize );
}

// A push field declared in the shader reaches the layout the same way: the block is read off the SPIR-V.
// (No row here, so the parser injects no include and each stage's block is exactly the one written.)
TEST_F( ShaderRootFixture, ANewPushFieldAppearsInTheLayoutWithoutACppEdit )
{
    const std::string wider =
         "layout( push_constant ) uniform PushConstants { mat4 Transform; uint MaterialIndex; "
         "uint BoneOffset; vec4 NewField; } m_PushConstants;";
    auto cell = Reconcile( PushMockTemplate( wider, wider ), "PushMock" );
    EXPECT_TRUE( cell.Errors.empty() ) << ( cell.Errors.empty() ? "" : cell.Errors.front() );
    const auto* field = cell.Layout.FindPush( "NewField" );
    ASSERT_NE( field, nullptr );
    EXPECT_EQ( field->Offset, 80u );
    EXPECT_EQ( cell.Layout.PushSize, 96u );
    EXPECT_FALSE( cell.Layout.RowBinding.has_value() );
}

// THE T1b REGRESSION. A vertex stage with a 72-byte block beside the fragment's 68 is one pipeline with two
// blocks; reflection merged it into "the larger wins" and nothing noticed. Reconciling refuses it by name.
TEST_F( ShaderRootFixture, StagesWhosePushBlocksDifferInLengthAreRefused )
{
    const std::string vertex72 =
         "layout( push_constant ) uniform PushConstants { mat4 Transform; uint MaterialIndex; "
         "uint BoneOffset; } m_PushConstants;";
    const std::string fragment68 =
         "layout( push_constant ) uniform PushConstants { mat4 Transform; uint MaterialIndex; } m_PushConstants;";
    auto cell = Reconcile( PushMockTemplate( vertex72, fragment68 ), "PushMock" );
    ASSERT_FALSE( cell.Errors.empty() ) << "a 72-byte vertex block beside a 68-byte fragment block was accepted";
    EXPECT_NE( cell.Errors.front().find( "PushMock" ), std::string::npos ) << cell.Errors.front();
    EXPECT_NE( cell.Errors.front().find( "72" ), std::string::npos ) << cell.Errors.front();
    EXPECT_NE( cell.Errors.front().find( "68" ), std::string::npos ) << cell.Errors.front();
}

// ---- Every mesh cell declares the fields its writers name ----------------------------------------------

// The renderer writes push fields BY NAME (Graphic/Materials/MaterialBinder.hpp), so the offsets are the
// layout's and cannot be written wrong; what can still go wrong is a cell that stops DECLARING a field its
// writer names — the write would then be dropped as Absent. Each cell is loaded the way the runtime loads it
// (ReconcileCellLayout), which also refuses a pipeline whose stages disagree about the block (the T1b
// regression: a 72-byte vertex block beside a 68-byte fragment one).
TEST_F( ShaderRootFixture, EveryMeshCellDeclaresEachPushFieldTheRendererWritesByName )
{
    struct Expectation
    {
        MeshVertexPath           Path;
        MeshPass                 Pass;
        std::vector<std::string> Fields;
    };

    // The fields each cell's writers name (Material::SetPushMatrix/SetMaterialIndex/SetInstancedWind,
    // Material::SetSkinnedBoneOffset, MaterialShadowSkinned::SetBoneOffset). A cell that lost one would silently drop the
    // write (MaterialBinder: Absent), so the cell must declare it; the layout, not C++, says where it sits.
    const Expectation expectations[] = {
         { MeshVertexPath::Static, MeshPass::Forward, { "Transform", "MaterialIndex" } },
         { MeshVertexPath::Static, MeshPass::GBuffer, { "Transform", "MaterialIndex" } },
         { MeshVertexPath::Static, MeshPass::Glass, { "Transform", "MaterialIndex" } },
         { MeshVertexPath::Instanced, MeshPass::Forward, { "Transform", "MaterialIndex", "WindA", "WindB" } },
         { MeshVertexPath::Instanced, MeshPass::GBuffer, { "Transform", "MaterialIndex", "WindA", "WindB" } },
         { MeshVertexPath::Instanced, MeshPass::ShadowDepth, { "Transform", "WindA", "WindB" } },
         { MeshVertexPath::Skinned, MeshPass::Forward, { "Transform", "MaterialIndex", "BoneOffset" } },
         { MeshVertexPath::Skinned, MeshPass::ShadowDepth, { "Transform", "BoneOffset" } },
    };

    for ( const auto& expected : expectations )
    {
        const char* name = TableShader( expected.Path, expected.Pass );
        ASSERT_NE( name, nullptr );
        auto cell = Reconcile( ReadFile( ShaderFileFor( name ) ), name, CellOf( name ) );
        EXPECT_TRUE( cell.Errors.empty() ) << ( cell.Errors.empty() ? "" : cell.Errors.front() );
        for ( const auto& field : expected.Fields )
        {
            const auto* f = cell.Layout.FindPush( field );
            ASSERT_NE( f, nullptr ) << name << " declares no push field '" << field << "'";
            EXPECT_LE( f->Offset + f->Size, cell.Layout.PushSize ) << name << " " << field;
        }
    }
}

// Every shipped forward cell loads (reconciles), carries a row, and declares the two fields every draw writes.
TEST_F( ShaderRootFixture, EveryShippedForwardCellReconcilesWithTheTransportFields )
{
    for ( const auto path : kAllPaths )
    {
        const char* name = TableShader( path, MeshPass::Forward );
        ASSERT_NE( name, nullptr );
        auto cell = Reconcile( ReadFile( ShaderFileFor( name ) ), name, CellOf( name ) );
        EXPECT_TRUE( cell.Errors.empty() ) << ( cell.Errors.empty() ? "" : cell.Errors.front() );
        EXPECT_NE( cell.Layout.FindPush( "Transform" ), nullptr ) << name;
        EXPECT_NE( cell.Layout.FindPush( "MaterialIndex" ), nullptr ) << name;
        EXPECT_TRUE( cell.Layout.RowBinding.has_value() ) << name;
    }
}

// ---- MaterialBinder: the push bytes are placed by name (MAT1h-2) ---------------------------------------

namespace
{
    struct PushBytes
    {
        Common::Memory::Buffer Buffer;
        explicit PushBytes( uint32_t size )
        {
            Buffer.Allocate( size );
            Buffer.ZeroInitialize();
        }
        ~PushBytes()
        {
            Buffer.Release();
        }
        template <class T>
        [[nodiscard]] T At( uint32_t offset ) const
        {
            T value{};
            std::memcpy( &value, static_cast<const std::byte*>( Buffer.Data ) + offset, sizeof( T ) );
            return value;
        }
    };
} // namespace

// A push field written only into the shader reaches the push BYTES: the binder finds it in the reconciled
// layout, and no C++ spells its offset.
TEST_F( ShaderRootFixture, ANewPushFieldReachesThePushBytesWithoutACppEdit )
{
    const std::string wider =
         "layout( push_constant ) uniform PushConstants { mat4 Transform; uint MaterialIndex; "
         "uint BoneOffset; vec4 NewField; } m_PushConstants;";
    auto cell = Reconcile( PushMockTemplate( wider, wider ), "PushMock" );
    ASSERT_TRUE( cell.Errors.empty() ) << cell.Errors.front();

    PushBytes       push( cell.Layout.PushSize );
    const glm::vec4 value( 1.0f, 2.0f, 3.0f, 4.0f );
    EXPECT_EQ( Desert::Graphic::MaterialBinder::WritePush( push.Buffer, cell.Layout, "NewField", &value,
                                                           sizeof( value ) ),
               Desert::Graphic::MaterialBinder::PushWrite::Written );
    EXPECT_EQ( push.At<glm::vec4>( cell.Layout.FindPush( "NewField" )->Offset ), value );
}

// THE MUTATION TARGET. A block with a field inserted before MaterialIndex moves it off 64; the index must
// land where the layout says, and 64 — the old constant — must stay untouched.
TEST_F( ShaderRootFixture, MaterialIndexLandsWhereTheLayoutPutsItNotAtTheOldOffset )
{
    const std::string shifted = "layout( push_constant ) uniform PushConstants { mat4 Transform; vec4 Inserted; "
                                "uint MaterialIndex; } m_PushConstants;";
    auto              cell    = Reconcile( PushMockTemplate( shifted, shifted ), "PushMock" );
    ASSERT_TRUE( cell.Errors.empty() ) << cell.Errors.front();
    const auto* index = cell.Layout.FindPush( "MaterialIndex" );
    ASSERT_NE( index, nullptr );
    ASSERT_EQ( index->Offset, 80u );

    PushBytes      push( cell.Layout.PushSize );
    const uint32_t row = 7;
    EXPECT_EQ( Desert::Graphic::MaterialBinder::WritePush( push.Buffer, cell.Layout, "MaterialIndex", &row,
                                                           sizeof( row ) ),
               Desert::Graphic::MaterialBinder::PushWrite::Written );
    EXPECT_EQ( push.At<uint32_t>( 80 ), row );
    EXPECT_EQ( push.At<uint32_t>( 64 ), 0u ) << "MaterialIndex was written at the retired offset 64";
}

// A field the cell does not declare is not written — explicitly, by the layout — and a value of the wrong
// size is refused rather than spilled over the next field.
TEST_F( ShaderRootFixture, AFieldTheCellLacksIsNotWrittenAndAWrongSizeIsRefused )
{
    const std::string noBones =
         "layout( push_constant ) uniform PushConstants { mat4 Transform; uint MaterialIndex; } m_PushConstants;";
    auto cell = Reconcile( PushMockTemplate( noBones, noBones ), "PushMock" );
    ASSERT_TRUE( cell.Errors.empty() ) << cell.Errors.front();

    PushBytes      push( 128 );
    const uint32_t bone = 0xABCDu;
    EXPECT_EQ( Desert::Graphic::MaterialBinder::WritePush( push.Buffer, cell.Layout, "BoneOffset", &bone,
                                                           sizeof( bone ) ),
               Desert::Graphic::MaterialBinder::PushWrite::Absent );
    for ( uint32_t offset = 0; offset < 128; offset += 4 )
        EXPECT_EQ( push.At<uint32_t>( offset ), 0u ) << offset;

    const glm::vec4 tooWide( 1.0f );
    EXPECT_EQ( Desert::Graphic::MaterialBinder::WritePush( push.Buffer, cell.Layout, "MaterialIndex", &tooWide,
                                                           sizeof( tooWide ) ),
               Desert::Graphic::MaterialBinder::PushWrite::SizeMismatch );
    EXPECT_EQ( push.At<uint32_t>( 64 ), 0u );
}

// THE ROUTING QUESTION (MeshCellPath): a material is drawn by the batched path of the vertex path its SHADER
// is a cell of, and by the generic path when its shader is no cell at all. The mesh system and the renderer
// both ask exactly this, of the shader the material was allocated from (SurfaceCellShader), never of its
// C++ class or its template's name. So the relation under test is the round trip: every cell the PBR
// template is allocated into answers the path it was allocated for, and a DSL surface's own cell answers
// nothing, which is what sends it to the generic queue. A cell that answered the wrong path would bind a
// skinned material without its Bones; one that answered nothing would draw a PBR mesh through the generic
// forward pipeline of a G-buffer shader.
TEST( MeshCellPath, EveryCellOfAnyTemplateRoutesToThePathItWasAllocatedFor )
{
    for ( const std::string templateName : { "SomeSurface", "Unlit" } )
        for ( uint32_t p = 0; p < Desert::Graphic::kMeshVertexPathCount; ++p )
            for ( uint32_t s = 0; s < Desert::Graphic::kMeshPassCount; ++s )
            {
                const auto path = static_cast<MeshVertexPath>( p );
                const auto pass = static_cast<MeshPass>( s );
                const auto cell = Desert::Graphic::MeshShaderFor( templateName, path, pass );
                if ( !cell || pass == MeshPass::Glass )
                    continue;
                const auto routed = Desert::Graphic::MeshCellPath( *cell );
                ASSERT_TRUE( routed.has_value() ) << *cell << " is a cell of the table and must take the batched path";
                EXPECT_EQ( *routed, path ) << *cell << " routes to " << MeshVertexPathName( *routed )
                                           << ", allocated for " << MeshVertexPathName( path );
            }
    EXPECT_FALSE( Desert::Graphic::MeshCellPath( "TextSDF" ).has_value() ) << "a template's default program is no cell";
    EXPECT_FALSE( Desert::Graphic::MeshCellPath( "" ).has_value() );
}

// THE CASTER IS CHOSEN BY BLEND MODE (SURF2-mask). A Masked material casts through ITS template's ShadowDepth
// cell, the only program that evaluates its OpacityMask and clips; every other caster shares the default
// surface template's. The defect this pins: the cascade pass drew every static caster with one shared
// pipeline, so a leaf card or a grass blade cast its whole quad. Asserted as a RELATION between two
// materials on two templates, on every path: the masked caster differs from the shared one and is the masked
// template's own cell; the opaque (and translucent) one IS the shared one, whatever template it names.
TEST( ShadowCaster, MaskedCastsThroughItsOwnTemplateCellOpaqueThroughTheSharedOne )
{
    using Desert::Core::Formats::SurfaceBlendMode;
    constexpr std::string_view kDefault = "StandardSurface";
    constexpr std::string_view kFoliage = "FoliageSurface";
    EXPECT_EQ( Desert::Graphic::ShadowCasterCellFor( SurfaceBlendMode::Masked ), Desert::Graphic::ShadowCasterCell::Own );
    EXPECT_EQ( Desert::Graphic::ShadowCasterCellFor( SurfaceBlendMode::Opaque ),
               Desert::Graphic::ShadowCasterCell::Shared );
    EXPECT_EQ( Desert::Graphic::ShadowCasterCellFor( SurfaceBlendMode::Translucent ),
               Desert::Graphic::ShadowCasterCell::Shared );
    for ( const auto path : kAllPaths )
    {
        const auto shared = Desert::Graphic::MeshShaderFor( kDefault, path, MeshPass::ShadowDepth );
        ASSERT_TRUE( shared.has_value() ) << MeshVertexPathName( path );

        const auto masked = Desert::Graphic::ShadowCasterShaderFor( kFoliage, kDefault, SurfaceBlendMode::Masked, path );
        ASSERT_TRUE( masked.has_value() ) << MeshVertexPathName( path );
        EXPECT_NE( *masked, *shared ) << "a masked caster on " << MeshVertexPathName( path )
                                      << " draws through the shared program and casts its whole quad";
        EXPECT_EQ( *masked, Desert::Graphic::MeshShaderFor( kFoliage, path, MeshPass::ShadowDepth ) );

        for ( const auto blend : { SurfaceBlendMode::Opaque, SurfaceBlendMode::Translucent } )
        {
            const auto caster = Desert::Graphic::ShadowCasterShaderFor( kFoliage, kDefault, blend, path );
            ASSERT_TRUE( caster.has_value() );
            EXPECT_EQ( *caster, *shared ) << "a non-masked caster on " << MeshVertexPathName( path )
                                          << " must share the one position-only program and batch by mesh";
        }
    }
}

// THE TABLE NAMES NO TEMPLATE (UE: the shader map is the MATERIAL's). No entry of the cell table carries a
// "<Template>/" prefix, and no shipped template's name appears in it: which template draws is the material's.
TEST_F( ShaderRootFixture, TheTableNamesNoTemplate )
{
    std::set<std::string> templates;
    for ( const auto& entry : std::filesystem::recursive_directory_iterator( "Resources/Shaders/Programs" ) )
        if ( entry.path().extension() == ".shader" )
            templates.insert( entry.path().stem().string() );
    ASSERT_FALSE( templates.empty() );
    for ( const auto path : kAllPaths )
        for ( const auto pass : { MeshPass::Forward, MeshPass::GBuffer, MeshPass::Glass, MeshPass::ShadowDepth } )
            if ( const char* cell = Desert::Graphic::MeshCellFor( path, pass ) )
            {
                EXPECT_EQ( std::string_view( cell ).find( '/' ), std::string_view::npos ) << cell;
                for ( const auto& t : templates )
                    EXPECT_EQ( std::string_view( cell ).find( t ), std::string_view::npos )
                         << "the cell table names the template '" << t << "' (" << cell << ")";
            }
    EXPECT_FALSE( Desert::Graphic::MeshShaderFor( "", MeshVertexPath::Static, MeshPass::Forward ) );
}

// A MATERIAL ON THE UNLIT TEMPLATE GETS THE UNLIT CELLS on every path, and the shipped Unlit.shader expands into
// each of them — so a skinned or instanced unlit mesh is drawn by Unlit, not by the default surface.
TEST_F( ShaderRootFixture, AnUnlitMaterialGetsTheUnlitCells )
{
    const auto file = ShaderFileFor( "Unlit" );
    ASSERT_FALSE( file.empty() );
    const auto parsed = Desert::Core::Preprocess::DShaderParser::Parse( ReadFile( file ) );
    ASSERT_TRUE( parsed.IsSuccess() ) << file.string();
    const auto& passes = parsed.GetValue().Meta.PassNames;
    for ( const auto path : kAllPaths )
        for ( const auto pass : { MeshPass::Forward, MeshPass::GBuffer, MeshPass::ShadowDepth } )
        {
            const auto shader = Desert::Graphic::MeshShaderFor( "Unlit", path, pass );
            ASSERT_TRUE( shader.has_value() ) << MeshVertexPathName( path );
            EXPECT_EQ( *shader, std::string( "Unlit/" ) + Desert::Graphic::MeshCellFor( path, pass ) );
            EXPECT_NE( std::find( passes.begin(), passes.end(), CellOf( shader->c_str() ) ), passes.end() )
                 << file.string() << " expands into no cell '" << CellOf( shader->c_str() ) << "'";
        }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
