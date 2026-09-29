// SURF1a: a surface template is ONE authored function; the engine owns how it is drawn. The parser expands
// the `Surface` block into a cell per (vertex path x pass), named `<Path>.<Pass>`, and each cell is a real
// program: it compiles, its material layout holds against its SPIR-V, and the engine headers it is built from
// move the shader map key when they are edited — a header outside the key would leave a warm start drawing
// the old cell.

#include <gtest/gtest.h>

#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Engine/Core/ShaderCompiler/ShaderCacheKey.hpp>
#include <Engine/Core/ShaderCompiler/ShaderMapBuild.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanShaderReflection.hpp>

#include <TestSupport/derived_data_sandbox.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace
{
    namespace PP = Desert::Core::Preprocess;

    // Masked + TwoSided so every branch of the expansion is exercised: the clip threshold is a parameter
    // and the texture makes the row and the sampler both reach the fragment stage.
    constexpr const char* kMockTemplate = R"(Shader "MockSurface"
{
    Domain Surface
    TwoSided
    BlendMode Masked

    Properties Binding(2) TextureBinding(20)
    {
        Color     Tint                 ("Tint") = (1, 1, 1, 1)
        Float     OpacityMaskClipValue ("Opacity Mask Clip", Range(0,1)) = 0.5
        Texture2D u_BaseTexture        ("Base")
    }

    Surface
    {
        SurfaceOutput EvaluateSurface( SurfaceInput i )
        {
            SurfaceOutput s = DefaultSurfaceOutput();
            const vec4 base = texture( u_BaseTexture, i.UV0 ) * u_Material.Tint;
            s.BaseColor     = base.rgb;
            s.Opacity       = base.a;
            return s;
        }
    }
}
)";

    std::filesystem::path s_EditorDir;

    struct SurfaceTemplateFixture : ::testing::Test
    {
        static void SetUpTestSuite()
        {
            // Includes resolve against "Resources/Shaders/", relative: run from Editor/ as the editor does.
            std::filesystem::path here = std::filesystem::current_path();
            for ( int up = 0; up < 8 && !std::filesystem::exists( here / "Editor" / "Resources" / "Shaders" );
                  ++up )
                here = here.parent_path();
            ASSERT_TRUE( std::filesystem::exists( here / "Editor" / "Resources" / "Shaders" ) )
                 << "could not find Editor/Resources/Shaders above " << std::filesystem::current_path();
            s_EditorDir = here / "Editor";
            std::filesystem::current_path( s_EditorDir );
        }
    };

    std::vector<std::string> ExpectedCells()
    {
        return { "Static.Forward",    "Static.GBuffer",    "Static.ShadowDepth",
                 "Instanced.Forward", "Instanced.GBuffer", "Instanced.ShadowDepth",
                 "Skinned.Forward",   "Skinned.GBuffer",   "Skinned.ShadowDepth" };
    }

    std::string ParseError( const std::string& text )
    {
        const auto parsed = PP::DShaderParser::Parse( text );
        return parsed.IsSuccess() ? std::string() : parsed.GetError();
    }
} // namespace

TEST_F( SurfaceTemplateFixture, TheSurfaceBlockExpandsIntoTheNineNamedCells )
{
    const auto parsed = PP::DShaderParser::Parse( kMockTemplate );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
    const auto& result = parsed.GetValue();

    EXPECT_EQ( result.Surface.Cells, ExpectedCells() );
    EXPECT_EQ( result.Meta.PassNames, ExpectedCells() ) << "a cell the boot build does not see is never compiled";
    EXPECT_TRUE( result.Surface.TwoSided );
    EXPECT_EQ( result.Surface.Blend, PP::SurfaceBlendMode::Masked );

    for ( const auto& name : ExpectedCells() )
    {
        const auto* cell = result.FindPass( name );
        ASSERT_NE( cell, nullptr ) << name;
        EXPECT_EQ( cell->Stages.size(), 2u ) << name << ": a cell is one vertex and one fragment stage";
        ASSERT_TRUE( cell->State.Cull.has_value() ) << name;
        EXPECT_EQ( *cell->State.Cull, Desert::Core::Formats::StateCull::None ) << name << ": TwoSided lost";

        const auto& fragment = cell->Stages.at( Desert::Core::Formats::ShaderStage::Fragment );
        const auto  pass     = name.substr( name.find( '.' ) + 1 );
        EXPECT_NE( fragment.find( PP::SurfacePassInclude( pass, result.Surface.Shading, result.Surface.Blend ) ), std::string::npos )
             << name;
        EXPECT_NE( fragment.find( "EvaluateSurface" ), std::string::npos ) << name;
        EXPECT_NE( fragment.find( "#define DESERT_SURFACE_MASKED" ), std::string::npos ) << name;
        const auto& vertex = cell->Stages.at( Desert::Core::Formats::ShaderStage::Vertex );
        EXPECT_NE( vertex.find( PP::SurfaceVertexInclude( name.substr( 0, name.find( '.' ) ) ) ),
                   std::string::npos )
             << name;
    }
}

// A template's default program (the empty pass name: what a lookup by shader name returns and what the top-level
// Stages/State hold) is the cell NAMED kSurfaceDefaultCell — the lookup and the parser's copy are that one cell.
TEST_F( SurfaceTemplateFixture, TheDefaultProgramIsTheNamedDefaultCell )
{
    const auto parsed = PP::DShaderParser::Parse( kMockTemplate );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
    const auto& result = parsed.GetValue();

    const auto* named = result.FindPass( std::string( PP::kSurfaceDefaultCell ) );
    ASSERT_NE( named, nullptr ) << PP::kSurfaceDefaultCell;
    EXPECT_EQ( result.FindPass( "" ), named );
    EXPECT_EQ( result.Stages, named->Stages );
    EXPECT_EQ( result.Meta.State, named->State );
}

TEST_F( SurfaceTemplateFixture, EveryCellCompilesAndItsMaterialLayoutHolds )
{
    const Desert::TestSupport::DerivedDataSandbox cache( "SurfaceTemplateCells" );
    const std::filesystem::path                   path = "Resources/Shaders/Programs/Test/MockSurface.shader";

    // Cells only: a surface template has no default ("") program of its own (SURF1c decides what boot
    // builds for it).
    for ( const auto& cell : ExpectedCells() )
    {
        const auto built = Desert::Core::BuildShaderMap(
             { kMockTemplate, path, cell, {}, std::format( "MockSurface/{}", cell ) } );
        ASSERT_TRUE( built.IsSuccess() ) << "cell '" << cell << "': " << built.GetError();
        const auto& map = built.GetValue();
        EXPECT_EQ( map.Stages.size(), 2u ) << cell;
        for ( const auto& stage : map.Stages )
            EXPECT_FALSE( stage.Spirv.empty() ) << cell;

        const auto reconciled = Desert::Graphic::API::Vulkan::ShaderReflection::ReconcileCellLayout(
             map.Meta, map.Stages, "MockSurface", cell );
        for ( const auto& error : reconciled.Errors )
            ADD_FAILURE() << error;
        EXPECT_EQ( reconciled.Layout.Params.size(), 2u ) << cell << ": Tint and the clip threshold";
    }
}

TEST_F( SurfaceTemplateFixture, TheTemplateSettingsAreRefusedWhereTheyCannotApply )
{
    EXPECT_NE( ParseError( R"(Shader "M" { Domain Surface BlendMode Masked
        Properties Binding(2) { Color Tint ("Tint") = (1,1,1,1) }
        Surface { SurfaceOutput EvaluateSurface( SurfaceInput i ) { return DefaultSurfaceOutput(); } } })" )
                    .find( std::string( PP::kSurfaceMaskClipParam ) ),
               std::string::npos )
         << "Masked without its threshold parameter must be refused by name";

    EXPECT_NE( ParseError( R"(Shader "M" { Domain PostProcess
        Surface { SurfaceOutput EvaluateSurface( SurfaceInput i ) { return DefaultSurfaceOutput(); } } })" )
                    .find( "Domain Surface" ),
               std::string::npos );

    EXPECT_NE( ParseError( R"(Shader "M" { Domain Surface TwoSided
        Vertex { void main() { gl_Position = vec4( 0.0 ); } } })" )
                    .find( "TwoSided" ),
               std::string::npos )
         << "TwoSided without a Surface block shapes nothing";

    EXPECT_NE( ParseError( R"(Shader "M" { Domain Surface
        Surface { SurfaceOutput EvaluateSurface( SurfaceInput i ) { return DefaultSurfaceOutput(); } }
        Vertex { void main() { gl_Position = vec4( 0.0 ); } } })" )
                    .find( "cells" ),
               std::string::npos )
         << "a stage block beside the cells is a program no mesh pass selects";

    EXPECT_FALSE( ParseError( R"(Shader "M" { Domain Surface
        Surface { SurfaceOutput EvaluateSurface( SurfaceInput i ) { return DefaultSurfaceOutput(); } } })" )
                       .size() )
         << "an opaque one-sided template is the plain case";
}

TEST_F( SurfaceTemplateFixture, EditingAnyCellHeaderMovesTheKeyOfEveryCell )
{
    // A private copy of the shader root, so the edit never touches the repository's headers.
    const auto root = std::filesystem::temp_directory_path() / "desert_surface_template_key";
    std::filesystem::remove_all( root );
    const auto shaders = root / "Resources" / "Shaders";
    std::filesystem::create_directories( shaders / "Mesh" );
    std::filesystem::copy( s_EditorDir / "Resources/Shaders/Common", shaders / "Common",
                           std::filesystem::copy_options::recursive );
    std::filesystem::copy( s_EditorDir / "Resources/Shaders/Mesh", shaders / "Mesh",
                           std::filesystem::copy_options::recursive );

    const auto previous = std::filesystem::current_path();
    std::filesystem::current_path( root );
    const std::filesystem::path program = "Resources/Shaders/Programs/Test/MockSurface.shader";

    const auto keys = [&]
    {
        std::vector<uint64_t> out;
        for ( const auto& cell : ExpectedCells() )
            out.push_back( Desert::Core::ComputeShaderMapKey( kMockTemplate, program, cell, false ) );
        return out;
    };

    const auto headers = PP::SurfaceTemplateIncludes();
    // Types + one vertex header per path + one pass header per (pass x shading model), the depth pass shared by
    // both models (SurfacePassInclude).
    EXPECT_EQ( headers.size(), 1u + PP::kSurfaceVertexPaths.size() + 2u * PP::kSurfaceCellPasses.size() - 1u );
    for ( const auto& header : headers )
    {
        ASSERT_TRUE( std::filesystem::exists( shaders / header ) ) << header;
        const auto before = keys();
        // A different SIZE as well: the per-process file cache is invalidated by size or write time.
        std::ofstream( shaders / header, std::ios::binary | std::ios::app ) << "\n// edited by the test\n";
        const auto after = keys();
        for ( size_t i = 0; i < before.size(); ++i )
            EXPECT_NE( before[i], after[i] )
                 << "editing " << header << " left the key of cell " << ExpectedCells()[i] << " where it was";
    }

    std::filesystem::current_path( previous );
    std::filesystem::remove_all( root );
}

namespace
{
    // THE STANDARD SURFACE, as shipped: the template whose cells replaced StaticMeshPBR(_Instanced),
    // StaticMeshGBuffer(_Instanced) and SkinnedMeshPBR (SURF1c). SURF1b held each cell to the program it
    // replaced by reflection before those programs were deleted; what remains checkable against a shipped
    // program is the shadow-depth cells, whose Shadow* programs are still drawn with (SURF1d).
    std::string StandardSurfaceText()
    {
        std::ifstream in( s_EditorDir / "Resources/Shaders/Programs/PBR/StandardSurface.shader", std::ios::binary );
        return std::string( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
    }

    // What a pipeline built from a program has to agree with: every descriptor (set, binding, type, count,
    // stages), the push range, and every stage's input and output locations — one line each, sorted, so a
    // mismatch names itself.
    std::vector<std::string> DescribeProgramLayout( const std::vector<Desert::Core::ShaderMapStage>& stages )
    {
        namespace Refl = Desert::Graphic::API::Vulkan::ShaderReflection;
        Desert::Graphic::API::Vulkan::ShaderResource::ReflectionData data;
        std::vector<std::string>                                     lines;
        for ( const auto& stage : stages )
        {
            for ( const auto& refused : Refl::ReflectStage( stage.Spirv, stage.Stage, data ) )
                lines.push_back( "refused: " + refused );
            const char*           name = Desert::Core::Formats::MaterialLayoutStageName( stage.Stage );
            spirv_cross::Compiler compiler( stage.Spirv );
            const auto            resources = compiler.get_shader_resources();
            for ( const auto& input : resources.stage_inputs )
                lines.push_back( std::format( "{} in location {}", name,
                                              compiler.get_decoration( input.id, spv::DecorationLocation ) ) );
            for ( const auto& output : resources.stage_outputs )
                lines.push_back( std::format( "{} out location {}", name,
                                              compiler.get_decoration( output.id, spv::DecorationLocation ) ) );
        }
        for ( const auto& [set, descriptors] : data.ShaderDescriptorSets )
            for ( const auto& b : Refl::BuildLayoutBindings( descriptors ) )
                lines.push_back( std::format( "set {} binding {} type {} count {} stages {:#x}", set, b.binding,
                                              static_cast<int>( b.descriptorType ), b.descriptorCount,
                                              b.stageFlags ) );
        if ( data.PushConstantRanges )
            lines.push_back( std::format( "push {} bytes, stages {:#x}", data.PushConstantRanges->Size,
                                          static_cast<uint32_t>( data.PushConstantRanges->ShaderStage ) ) );
        std::sort( lines.begin(), lines.end() );
        return lines;
    }
} // namespace

TEST_F( SurfaceTemplateFixture, EveryStandardSurfaceCellLoadsAndTheDepthCellsBindWhereShadowBinds )
{
    const Desert::TestSupport::DerivedDataSandbox cache( "SurfaceTemplateStandard" );
    const std::string                             text = StandardSurfaceText();
    ASSERT_FALSE( text.empty() ) << "StandardSurface.shader is missing";

    // The shadow-depth cells ARE the casters now (MeshShaderFor's ShadowDepth column); the three Shadow*
    // programs they replaced are gone, so their layout is pinned here instead of read from them. It is what
    // MaterialShadow* and MeshRenderer's cascade pipelines bind against: the light's CameraUB at 0, the
    // skinned path's Bones at 1 and the instanced path's InstanceTransforms at 17, the transform push block
    // (plus BoneOffset for skinned, plus the wind for instanced), no varyings and one R32F-bound output.
    // Type 6 = uniform buffer, 7 = storage buffer; stages 0x1 = vertex.
    const std::map<std::string, std::vector<std::string>> shadow = {
         { "Static.ShadowDepth",
           { "fragment out location 0", "push 64 bytes, stages 0x1", "set 0 binding 0 type 6 count 1 stages 0x1",
             "vertex in location 0", "vertex in location 1", "vertex in location 2", "vertex in location 3",
             "vertex in location 4" } },
         { "Instanced.ShadowDepth",
           { "fragment out location 0", "push 112 bytes, stages 0x1", "set 0 binding 0 type 6 count 1 stages 0x1",
             "set 0 binding 17 type 7 count 1 stages 0x1", "vertex in location 0", "vertex in location 1",
             "vertex in location 2", "vertex in location 3", "vertex in location 4" } },
         { "Skinned.ShadowDepth",
           { "fragment out location 0", "push 68 bytes, stages 0x1", "set 0 binding 0 type 6 count 1 stages 0x1",
             "set 0 binding 1 type 7 count 1 stages 0x1", "vertex in location 0", "vertex in location 1",
             "vertex in location 2", "vertex in location 3", "vertex in location 4", "vertex in location 5",
             "vertex in location 6" } },
    };
    for ( const std::string& cell : ExpectedCells() )
    {
        const auto builtCell = Desert::Core::BuildShaderMap(
             { text, "Resources/Shaders/Programs/PBR/StandardSurface.shader", cell, {},
               std::format( "StandardSurface/{}", cell ) } );
        ASSERT_TRUE( builtCell.IsSuccess() ) << "cell '" << cell << "': " << builtCell.GetError();

        // THE LOAD PATH: VulkanShader refuses a cell on any reconcile error.
        const auto reconciled = Desert::Graphic::API::Vulkan::ShaderReflection::ReconcileCellLayout(
             builtCell.GetValue().Meta, builtCell.GetValue().Stages, "StandardSurface", cell );
        for ( const auto& error : reconciled.Errors )
            ADD_FAILURE() << error;

        const auto pinned = shadow.find( cell );
        if ( pinned == shadow.end() )
            continue;
        EXPECT_EQ( DescribeProgramLayout( builtCell.GetValue().Stages ), pinned->second )
             << "cell " << cell << " no longer binds where the shadow casters bind";
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
