#include <gtest/gtest.h>

#include <Editor/Import/MaterialImportContract.hpp>

#include "../../TestSupport/scratch_dir.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

using namespace Desert::Editor;
using Common::Content::ReadShaderManifest;

// MAT1b: the importer's template choice is made by the templates' Import contracts, never by name.
namespace
{
    ImportTemplate Template( std::string name, bool isDefault, std::vector<std::string> needs,
                             std::vector<std::string> keys )
    {
        ImportTemplate t;
        t.ShaderName              = std::move( name );
        t.Manifest.DeclaresImport = true;
        t.Manifest.DefaultSurface = isDefault;
        t.Manifest.ImportRequires = std::move( needs );
        for ( auto& k : keys )
            t.Manifest.Import.push_back( { std::move( k ), "P", "" } );
        return t;
    }

    SourceMaterial Material( const std::vector<std::string>& keys )
    {
        SourceMaterial m{ "Leaves", {} };
        for ( const auto& k : keys )
            m.Entries[k] = {};
        return m;
    }

    std::vector<ImportTemplate> PBRAndUnlit()
    {
        return { Template( "StaticMeshPBR", true, {}, { "gltf.baseColorFactor", "gltf.baseColorTexture" } ),
                 Template( "Unlit", false, { "gltf.KHR_materials_unlit" }, { "gltf.baseColorFactor" } ) };
    }
} // namespace

TEST( MaterialImportContract, AGltfPBRMaterialGoesToTheTemplateThatRequiresNothing )
{
    const auto chosen = ChooseImportTemplate( Material( { "gltf.baseColorTexture" } ), PBRAndUnlit(), "a.gltf" );
    ASSERT_TRUE( chosen.IsSuccess() ) << chosen.GetError();
    EXPECT_EQ( chosen.GetValue(), 0u );
}

TEST( MaterialImportContract, TheTemplateThatRequiresMoreWins )
{
    const auto chosen = ChooseImportTemplate( Material( { "gltf.KHR_materials_unlit", "gltf.baseColorFactor" } ),
                                              PBRAndUnlit(), "a.gltf" );
    ASSERT_TRUE( chosen.IsSuccess() ) << chosen.GetError();
    EXPECT_EQ( chosen.GetValue(), 1u );
}

TEST( MaterialImportContract, NoTakerIsARefusalNamingMaterialAndFile )
{
    const auto chosen = ChooseImportTemplate( Material( { "obj.Kd" } ), PBRAndUnlit(), "Meshes/tree.obj" );
    ASSERT_FALSE( chosen.IsSuccess() );
    EXPECT_NE( chosen.GetError().find( "Leaves" ), std::string::npos ) << chosen.GetError();
    EXPECT_NE( chosen.GetError().find( "Meshes/tree.obj" ), std::string::npos ) << chosen.GetError();
    EXPECT_NE( chosen.GetError().find( "obj.Kd" ), std::string::npos ) << chosen.GetError();
}

TEST( MaterialImportContract, ATemplateWithoutAnImportBlockTakesNothingEvenIfItIsTheDefault )
{
    std::vector<ImportTemplate> onlyDefault( 1 );
    onlyDefault[0].ShaderName              = "StaticMeshPBR";
    onlyDefault[0].Manifest.DefaultSurface = true;
    EXPECT_FALSE( ChooseImportTemplate( Material( {} ), onlyDefault, "a.fbx" ).IsSuccess() );
}

TEST( MaterialImportContract, ATieIsBrokenOnlyByDefaultSurface )
{
    const std::vector<ImportTemplate> twoPlain = { Template( "A", false, {}, { "fbx.DiffuseColor" } ),
                                                   Template( "B", false, {}, { "fbx.DiffuseColor" } ) };
    const auto refused = ChooseImportTemplate( Material( { "fbx.DiffuseColor" } ), twoPlain, "a.fbx" );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "A, B" ), std::string::npos ) << refused.GetError();

    std::vector<ImportTemplate> withDefault = twoPlain;
    withDefault[1].Manifest.DefaultSurface  = true;
    const auto chosen = ChooseImportTemplate( Material( { "fbx.DiffuseColor" } ), withDefault, "a.fbx" );
    ASSERT_TRUE( chosen.IsSuccess() ) << chosen.GetError();
    EXPECT_EQ( chosen.GetValue(), 1u );
}

// The RELATION with the shipped files: the two templates that declare Import today choose as the rule says.
TEST( MaterialImportContract, TheShippedTemplatesChooseByTheirOwnContracts )
{
    const std::vector<std::pair<std::string, std::string>> files = {
         { "StandardSurface", "Editor/Resources/Shaders/Programs/PBR/StandardSurface.shader" },
         { "Unlit", "Editor/Resources/Shaders/Programs/Unlit/Unlit.shader" } };
    std::vector<ImportTemplate> shipped;
    for ( const auto& [name, file] : files )
    {
        const std::filesystem::path path = Desert::TestSupport::RepositoryRoot() / file;
        const std::ifstream         in( path );
        ASSERT_TRUE( in ) << path.string();
        std::stringstream text;
        text << in.rdbuf();
        auto manifest = ReadShaderManifest( text.str() );
        ASSERT_TRUE( manifest.IsSuccess() ) << file << ": " << manifest.GetError();
        ImportTemplate& t = shipped.emplace_back();
        t.ShaderName      = name;
        t.Manifest        = manifest.GetValue();
    }
    const auto pbr = ChooseImportTemplate(
         Material( { "gltf.baseColorTexture", "gltf.metallicRoughnessTexture" } ), shipped, "a.gltf" );
    ASSERT_TRUE( pbr.IsSuccess() ) << pbr.GetError();
    EXPECT_EQ( shipped[pbr.GetValue()].ShaderName, "StandardSurface" );

    const auto unlit = ChooseImportTemplate( Material( { "gltf.KHR_materials_unlit", "gltf.baseColorTexture" } ),
                                             shipped, "a.gltf" );
    ASSERT_TRUE( unlit.IsSuccess() ) << unlit.GetError();
    EXPECT_EQ( shipped[unlit.GetValue()].ShaderName, "Unlit" );

    const auto fbx = ChooseImportTemplate( Material( { "fbx.DiffuseColor" } ), shipped, "a.fbx" );
    ASSERT_TRUE( fbx.IsSuccess() ) << fbx.GetError();
    EXPECT_EQ( shipped[fbx.GetValue()].ShaderName, "StandardSurface" );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    // Parsing a surface template resolves its `ShadingModel` through the shading models of the engine's shader
    // root, which the engine finds against the working directory; the runner starts the suite in its scratch
    // directory, so the process works from the engine resources, as the editor does.
    const Desert::TestSupport::EngineResourcesWorkingDirectory engineResources;
    if ( !engineResources.Error().empty() )
    {
        std::fprintf( stderr, "MaterialImportContract: %s\n", engineResources.Error().c_str() );
        return 1;
    }
    return RUN_ALL_TESTS();
}

// THM1k (owner): AN IMPORT WRITES NO PreviewMesh - every imported material's thumbnail is the ball, as in UE.
// The document is the chosen template and the fill's values, nothing that names a mesh.
TEST( MaterialImportContract, AnImportedMaterialNamesNoPreviewMesh )
{
    ImportTemplate chosen = Template( "StaticMeshPBR", true, {}, { "gltf.baseColorFactor" } );
    chosen.Guid           = "0123456789abcdef0123456789abcdef";
    chosen.Locator        = "engine:Shaders/StaticMeshPBR.dshader";
    TemplateFill fill;
    fill.Params.push_back( { "BaseColor", glm::vec4( 0.5f ) } );

    const auto data = ImportedMaterialDocument( chosen, fill );
    EXPECT_FALSE( data.Thumbnail.has_value() ) << "an import named a thumbnail mesh; the ball is the owner's rule";
    if ( !data.Shader.has_value() )
        FAIL() << "an imported material names no shader";
    EXPECT_EQ( data.Shader->Guid, chosen.Guid );
    EXPECT_EQ( data.Shader->Path, chosen.Locator );
    ASSERT_EQ( data.Params.size(), 1u );
    EXPECT_EQ( data.Params[0].Name, "BaseColor" );
}
