// RED-THEN-GREEN over every committed `.demat`: each one that names a shader must resolve it by GUID,
// no matter which content root the shader sits under. MS1 fixes a defect where a shader rooted at
// `engine:` (Editor/Resources/Shaders/...) never resolved, and the failure read back as an EMPTY name —
// `SurfaceMaterialAsset::ResolveShader` cleared `m_ShaderName` on the refusal and left it that way, so
// the Material Editor and the thumbnail sweep both printed "shader '' is not registered/loaded" with no
// GUID or path in sight. See MaterialEditorPanel::DrawnShaderName and ThumbnailSubject::PreviewRouteFor.
//
// This suite drives the SAME production path the Editor's boot does (AssetPreloader::PreloadShaders,
// then AssetPreloader::PreloadCookedAssetsAndMaterials): register every `.shader` under the real content
// roots as a ShaderAsset, THEN load every `.demat` as a SurfaceMaterialAsset and read back its resolved
// shader name. No GPU is touched — ShaderAsset::LoadFromFile only reads and parses text, exactly as the
// asset-handle-stability suite already proves for this same asset layer.
//
// MATERIALS ARE ENUMERATED FROM DISK, not from a hand-written list, so a new `.demat` added to the
// project is covered automatically and a fixed one cannot be quietly dropped from the census.

#include <gtest/gtest.h>

#include <Common/Core/Constants.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Assets/Shader/ShaderAsset.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <format>
#include <string>
#include <vector>

namespace
{
    // The repository's `Editor/` directory, derived from this file's own path rather than from the
    // process's working directory — the suite binary runs from `build/Bin/Tests/<Config>/`, and a path
    // baked in at compile time is the one thing that does not depend on how the test was launched.
    std::filesystem::path EditorDirectory()
    {
        // Walks up from the working directory (suites run from the repo root): __FILE__ is relative to
        // wherever the project file sits (build/Projects since BLD1), so it cannot name the tree.
        std::filesystem::path prefix = ".";
        for ( int up = 0; up < 6; ++up, prefix /= ".." )
            if ( std::filesystem::exists( prefix / "Editor" / "Resources" / "Shaders" ) )
                return std::filesystem::weakly_canonical( prefix / "Editor" );
        return {};
    }

    // Changes the process's working directory to `Editor/` for the lifetime of the guard and restores it
    // after — `Common::Constants::Path::RESOURCE_PATH` ("Resources/") is a literal relative to the
    // working directory and is deliberately never remapped by a project (StartupLayout.hpp), so it is
    // the ONE thing a test that wants the real engine content has to set up itself.
    class WorkingDirectoryGuard
    {
    public:
        WorkingDirectoryGuard( const std::filesystem::path& next ) : m_Previous( std::filesystem::current_path() )
        {
            std::filesystem::current_path( next );
        }
        ~WorkingDirectoryGuard()
        {
            std::error_code ec;
            std::filesystem::current_path( m_Previous, ec );
        }
        WorkingDirectoryGuard( const WorkingDirectoryGuard& )            = delete;
        WorkingDirectoryGuard& operator=( const WorkingDirectoryGuard& ) = delete;

    private:
        std::filesystem::path m_Previous;
    };

    // Every `.demat` under the real content tree, walked directly rather than through
    // `ContentRegistry::FilesOfKind` — the mechanism under test is shader resolution, not the content
    // scan, so the census of WHICH files to check must not depend on the same machinery being tested.
    std::vector<std::filesystem::path> EveryCommittedMaterial( const std::filesystem::path& editorDir )
    {
        std::vector<std::filesystem::path> found;
        std::error_code                    ec;
        for ( const auto& entry : std::filesystem::recursive_directory_iterator( editorDir / "Resources", ec ) )
        {
            if ( entry.is_regular_file() && entry.path().extension() == ".demat" )
                found.push_back( entry.path() );
        }
        return found;
    }
} // namespace

// Boots the shader registry from the real engine content, then loads every committed `.demat` and
// asserts each one that names a shader resolved it to a non-empty name — the exact value
// MaterialEditorPanel::DrawnShaderName() and ThumbnailSubject::PreviewRouteFor() read. An empty name
// means SurfaceMaterialAsset::ResolveShader (Engine/Assets/Mesh/SurfaceMaterialAsset.cpp) refused the
// GUID it was given, and the assertion below names the material, the GUID and the shader path the file
// itself states — never an empty pair of quotes.
TEST( EngineShaderByGuid, EveryCommittedMaterialResolvesItsShaderByGuid )
{
    const std::filesystem::path editorDir = EditorDirectory();
    ASSERT_TRUE( std::filesystem::exists( editorDir / "Resources" / "Shaders" ) )
         << "expected the real engine content at '" << editorDir.string() << "'";

    const WorkingDirectoryGuard cwdGuard( editorDir );

    const auto gathered = Desert::Assets::ContentRegistry::Gather();
    ASSERT_TRUE( gathered ) << gathered.GetError();

    Desert::Assets::AssetManager manager;

    // Every `.shader` under every content root (project AND engine — ContentKindSpec::Shader's root is
    // SHADERDIR_PATH, "Resources/Shaders/") becomes a ShaderAsset, exactly as
    // AssetPreloader::PreloadShaders does at boot, and strictly BEFORE any material is loaded — shaders
    // must exist first (AssetPreloader.cpp, EditorLayer.cpp's own comment says so).
    std::size_t shaderCount = 0;
    for ( const std::filesystem::path& shaderPath :
          Desert::Assets::ContentRegistry::FilesOfKind( Common::Content::ContentKind::Shader ) )
    {
        if ( manager.CreateAsset<Desert::Assets::ShaderAsset>( shaderPath ) )
            ++shaderCount;
    }
    ASSERT_GT( shaderCount, 0u ) << "no .shader file registered — the content roots did not resolve";

    const std::vector<std::filesystem::path> materials = EveryCommittedMaterial( editorDir );
    ASSERT_GT( materials.size(), 0u ) << "no .demat file found under '" << editorDir.string() << "'";

    std::vector<std::string> unresolved;
    for ( const std::filesystem::path& materialPath : materials )
    {
        // MIRRORS THE REAL TWO-STEP SEQUENCE, not a single eager CreateAsset. `AssetPreloader::
        // PreloadCookedAssetsAndMaterials` registers every `.demat` as an UNPARSED SHELL
        // (`loadAfterCreate=false`) — that is the shape of `manager.FindByPath` finding something
        // already there but not ready. `ThumbnailSubject::ResolveMaterial` then finishes loading it on
        // demand. The defect (MS1) was in THAT second step: it used to call the bare, non-virtual
        // `AssetBase::Load()` — which parses the file but cannot re-resolve the shader GUID, because it
        // takes no `AssetManager` — instead of `AssetBase::EnsureLoaded(manager)`, the one entry point
        // `AssetBase::EnsureLoaded`'s own comment says a caller must use for exactly this reason ("Load()
        // and ResolveDependencies() as two statements... the second one is what gets forgotten").
        const auto shell =
             manager.CreateAsset<Desert::Assets::SurfaceMaterialAsset>( materialPath, /*loadAfterCreate=*/false );
        if ( !shell )
        {
            unresolved.push_back( materialPath.string() + ": did not create as a shell" );
            continue;
        }

        if ( !shell->IsReadyForUse() )
            (void)shell->EnsureLoaded( manager );

        const Desert::Assets::MaterialData& data = shell->Data();
        if ( !data.Shader.has_value() )
        {
            // An instance's template is its parent's; anything else must name one (there is no default).
            if ( !data.InstanceParentId().has_value() )
                unresolved.push_back( materialPath.string() + ": names no surface template" );
            continue;
        }

        if ( shell->GetShaderName().empty() || shell->GetShaderHandle().IsNull() )
        {
            unresolved.push_back( std::format( "{}: shader GUID {} ('{}') did not resolve to a name",
                                               materialPath.string(), data.Shader->Guid, data.Shader->Path ) );
        }
    }

    std::string joined;
    for ( const std::string& line : unresolved )
        joined += "\n  " + line;
    EXPECT_TRUE( unresolved.empty() ) << unresolved.size()
                                      << " material(s) did not resolve their shader:" << joined;

    // THE TEMPLATE REGISTRY over the engine corpus: each role and the default are declared by exactly one file.
    const auto pbr = Desert::Assets::FindTemplateByRole( manager, Common::Content::kPBRSurfaceRole );
    ASSERT_TRUE( pbr ) << pbr.GetError();
    const auto debugColor = Desert::Assets::FindTemplateByRole( manager, Common::Content::kDebugColorRole );
    ASSERT_TRUE( debugColor ) << debugColor.GetError();
    const auto byDefault = Desert::Assets::FindDefaultSurfaceTemplate( manager, "", "" );
    ASSERT_TRUE( byDefault ) << byDefault.GetError();
    EXPECT_EQ( byDefault.GetValue(), pbr.GetValue() ) << "StaticMeshPBR.shader declares `Default Surface`";
}

namespace
{
    namespace fs = std::filesystem;

    struct ScratchDir
    {
        fs::path Root =
             fs::temp_directory_path() /
             std::format( "mat1g_templates_{}", ::testing::UnitTest::GetInstance()->current_test_info()->name() );
        ScratchDir()
        {
            fs::remove_all( Root );
            fs::create_directories( Root );
        }
        ~ScratchDir()
        {
            std::error_code ec;
            fs::remove_all( Root, ec );
        }
    };

    fs::path WriteMockShader( const fs::path& dir, const std::string& name, const std::string& guid,
                              const std::string& manifest )
    {
        const fs::path path = dir / ( name + ".shader" );
        std::ofstream( path ) << "// DesertAsset {\"Kind\":\"Shader\",\"Guid\":\"" << guid
                              << "\",\"Versions\":{\"SHDR\":1},\"Dependencies\":[]}\nShader \"" << name
                              << "\"\n{\n    Domain Surface\n"
                              << manifest << "}\n";
        return path;
    }

    fs::path WriteMaterial( const fs::path& dir, const std::string& shaderPart, const std::string& guid,
                            const std::string& dependencies )
    {
        const fs::path path = dir / "M_Mock.demat";
        std::ofstream( path ) << "{\n    \"Header\": {\n        \"Kind\": \"Material\",\n        \"Guid\": "
                                 "\""
                              << guid
                              << "\",\n        \"Versions\": {\n            "
                                 "\"MATL\": 4\n        },\n        \"Dependencies\": ["
                              << dependencies << "]\n    },\n"
                              << shaderPart
                              << "    \"Params\": [],\n    \"Textures\": [],\n    \"CloudAssets\": []\n}\n";
        return path;
    }

    constexpr const char* kMockGuidA = "1111aaaa1111aaaa1111aaaa1111aaaa";
    constexpr const char* kMockGuidB = "2222bbbb2222bbbb2222bbbb2222bbbb";
} // namespace

// NO DEFAULT BY ABSENCE: a material (not an instance) that names no template is refused on load, by path.
TEST( EngineShaderByGuid, MaterialWithoutTemplateIsRefused )
{
    const ScratchDir dir;
    const fs::path   material = WriteMaterial( dir.Root, "", "0badc0de0badc0de0badc0de0badc0d1", "" );
    Desert::Assets::SurfaceMaterialAsset asset( material );
    const auto                           loaded = asset.LoadFromFile();
    ASSERT_FALSE( loaded ) << "a material without \"Shader\" loaded — a default template came back";
    EXPECT_NE( loaded.GetError().find( material.generic_string() ), std::string::npos ) << loaded.GetError();
    EXPECT_TRUE( asset.GetShaderHandle().IsNull() );
}

// The template is identified by its GUID: renaming the shader (file stem + DSL `Shader "…"`) changes the display
// name and nothing the material resolves.
TEST( EngineShaderByGuid, RenamingTheShaderDoesNotChangeResolution )
{
    const ScratchDir dir;
    const fs::path   material = WriteMaterial(
         dir.Root,
         std::format( "    \"Shader\": {{\n        \"Guid\": \"{}\",\n        \"Path\": \"mock\"\n    }},\n",
                        kMockGuidA ),
         "0badc0de0badc0de0badc0de0badc0d2", std::format( "\"{}\"", kMockGuidA ) );
    Common::AssetHandle before;
    for ( const std::string name : { "MockBefore", "MockAfter" } )
    {
        const fs::path sub = dir.Root / name;
        fs::create_directories( sub );
        Desert::Assets::AssetManager manager;
        ASSERT_TRUE(
             manager.CreateAsset<Desert::Assets::ShaderAsset>( WriteMockShader( sub, name, kMockGuidA, "" ) ) );
        Desert::Assets::SurfaceMaterialAsset asset( material );
        ASSERT_TRUE( asset.LoadFromFile() );
        asset.ResolveDependencies( manager );
        ASSERT_FALSE( asset.GetShaderHandle().IsNull() ) << "did not resolve through shader '" << name << "'";
        EXPECT_EQ( asset.GetShaderName(), name ) << "the name is display text read from the shader";
        if ( name == "MockBefore" )
            before = asset.GetShaderHandle();
        else
            EXPECT_EQ( asset.GetShaderHandle(), before ) << "renaming the shader moved the material's template";
    }
}

// Exactly one `Default Surface` and one shader per role: none or several is a refusal naming every path.
TEST( EngineShaderByGuid, DefaultAndRolesAreDeclaredExactlyOnce )
{
    const ScratchDir             dir;
    Desert::Assets::AssetManager none;
    ASSERT_TRUE( none.CreateAsset<Desert::Assets::ShaderAsset>(
         WriteMockShader( dir.Root, "MockPlain", kMockGuidA, "" ) ) );
    EXPECT_FALSE( Desert::Assets::FindDefaultSurfaceTemplate( none, "", "" ) );
    EXPECT_FALSE( Desert::Assets::FindTemplateByRole( none, "DebugColor" ) );

    const fs::path twice = dir.Root / "twice";
    fs::create_directories( twice );
    Desert::Assets::AssetManager two;
    const fs::path a = WriteMockShader( twice, "MockA", kMockGuidA, "    Role DebugColor\n    Default Surface\n" );
    const fs::path b = WriteMockShader( twice, "MockB", kMockGuidB, "    Role DebugColor\n    Default Surface\n" );
    ASSERT_TRUE( two.CreateAsset<Desert::Assets::ShaderAsset>( a ) );
    ASSERT_TRUE( two.CreateAsset<Desert::Assets::ShaderAsset>( b ) );
    for ( const auto& refused : { Desert::Assets::FindDefaultSurfaceTemplate( two, "", "" ),
                                  Desert::Assets::FindTemplateByRole( two, "DebugColor" ) } )
    {
        ASSERT_FALSE( refused );
        EXPECT_NE( refused.GetError().find( "MockA.shader" ), std::string::npos ) << refused.GetError();
        EXPECT_NE( refused.GetError().find( "MockB.shader" ), std::string::npos ) << refused.GetError();
    }

    // The project override picks one of them; a GUID no loaded shader has is refused naming the .deproj.
    const auto picked = Desert::Assets::FindDefaultSurfaceTemplate( two, kMockGuidB, "Game.deproj" );
    ASSERT_TRUE( picked ) << picked.GetError();
    const auto unknown =
         Desert::Assets::FindDefaultSurfaceTemplate( two, "3333cccc3333cccc3333cccc3333cccc", "Game.deproj" );
    ASSERT_FALSE( unknown );
    EXPECT_NE( unknown.GetError().find( "Game.deproj" ), std::string::npos ) << unknown.GetError();
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

// CENSUS: no decision in the sources takes a material TEMPLATE by its name. A template is a `.shader` under
// Editor/Resources/Shaders that declares a material domain (`Domain Surface`/`Domain Terrain`) or a
// template manifest line (`Role …`, `Default Surface`); its identity is its handle, found by role or by the
// project's default (FindTemplateByRole / FindDefaultSurfaceTemplate). A quoted template name in a code
// line is a decision by name unless it is listed below, BY FILE AND NAME, with the reason it may stay —
// never a count. Comment lines are not code and are skipped.
namespace
{
    struct AllowedTemplateName
    {
        const char* File; // repo-relative, generic separators
        const char* Name;
        const char* Why;
    };

    constexpr AllowedTemplateName kAllowedTemplateNames[] = {
         { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.cpp", "StaticMeshPBR",
           "ShaderService compile key of the batched PBR backend's geometry program (MAT1a-T1 owns it)" },
         { "Desert/Desert/Source/Engine/Graphic/Materials/Mesh/MeshVertexPath.cpp", "StaticMeshPBR",
           "ShaderService compile-key table of the PBR backend's per-pass programs (MAT1a-T1 owns it)" },
         { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.cpp", "DefaultSurface",
           "compile key of the renderer's fallback surface program (MAT1a-T1 owns it)" },
         { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Terrain/TerrainRenderer.cpp", "Terrain",
           "compile key of the terrain renderer's own program, not a material's template" },
         { "Desert/Common/Source/Common/Content/ShaderAssetHeader.hpp", "Terrain",
           "the ROLE's spelling (`Role Terrain`, kTerrainRole), which shares the word with the template" },
         { "Desert/Desert/Source/Engine/Graphic/Clouds/CloudMaterialValues.hpp", "CloudRaymarch",
           "compile key of the cloud renderer's own program, whose Properties block is the cloud material "
           "schema" },
         { "Desert/Desert/Source/Engine/ECS/System/TextECSSystem.hpp", "TextSDF",
           "compile key of the text system's own program, not a material's template" },
         { "Desert/Desert/Source/Engine/Core/Formats/ShaderProgramMeta.hpp", "Terrain",
           "the DOMAIN's spelling (`Domain Terrain`), which shares the word with the template" },
         { "Editor/Source/Editor/Panels/MaterialEditor/MaterialEditorPanel.cpp", "Terrain",
           "the DOMAIN's display label, which shares the word with the template" },
         { "Desert/Desert/Source/Engine/Geometry/PrimitiveType.hpp", "Terrain",
           "a primitive type's display name, which shares the word with the template" },
         { "Editor/Source/Editor/Panels/NodeGraph/NodeGraphPanel.cpp", "NewShaderGraph",
           "the file name a NEW graph document is saved under; the graph compiles to a shader of its own name" },
    };

    std::vector<std::string> TemplateStems( const std::filesystem::path& shadersDir )
    {
        std::vector<std::string> stems;
        std::error_code          ec;
        for ( const auto& entry : std::filesystem::recursive_directory_iterator( shadersDir, ec ) )
        {
            if ( !entry.is_regular_file() || entry.path().extension() != ".shader" )
                continue;
            std::ifstream in( entry.path() );
            for ( std::string line; std::getline( in, line ); )
            {
                const auto first = line.find_first_not_of( " \t" );
                if ( first == std::string::npos )
                    continue;
                const std::string_view text = std::string_view( line ).substr( first );
                if ( text.starts_with( "Domain Surface" ) || text.starts_with( "Domain Terrain" ) ||
                     text.starts_with( "Role " ) || text.starts_with( "Default Surface" ) )
                {
                    stems.push_back( entry.path().stem().string() );
                    break;
                }
            }
        }
        return stems;
    }
} // namespace

// A MaterialComponent's Shader names an override only (MAT1g): the PBRSurface template is refused
// with the path the reference states - not dropped, not substituted - and any other template resolves.
TEST( EngineShaderByGuid, AComponentShaderNamingThePBRSurfaceTemplateIsRefused )
{
    const ScratchDir             dir;
    Desert::Assets::AssetManager manager;
    ASSERT_TRUE( manager.CreateAsset<Desert::Assets::ShaderAsset>(
         WriteMockShader( dir.Root, "MockPBR", kMockGuidA, "    Role PBRSurface\n" ) ) );
    ASSERT_TRUE( manager.CreateAsset<Desert::Assets::ShaderAsset>(
         WriteMockShader( dir.Root, "MockOverride", kMockGuidB, "" ) ) );
    const Desert::Assets::AssetRefSite site{ "shader", "Material.Shader", "Entities[id=1]" };

    const auto refused = Desert::Assets::FindOverrideShaderNameByRef(
         manager, { kMockGuidA, "Resources/Shaders/MockPBR.shader" }, site );
    ASSERT_FALSE( refused ) << "the PBRSurface template was accepted as an override";
    EXPECT_NE( refused.GetError().find( "Resources/Shaders/MockPBR.shader" ), std::string::npos ) << refused.GetError();
    EXPECT_NE( refused.GetError().find( "Entities[id=1]" ), std::string::npos ) << refused.GetError();

    const auto kept = Desert::Assets::FindOverrideShaderNameByRef(
         manager, { kMockGuidB, "Resources/Shaders/MockOverride.shader" }, site );
    ASSERT_TRUE( kept ) << kept.GetError();
    EXPECT_EQ( kept.GetValue(), "MockOverride" );
}

TEST( EngineShaderByGuid, NoDecisionNamesATemplate )
{
    const std::filesystem::path editorDir = EditorDirectory();
    const std::filesystem::path repo      = editorDir.parent_path();
    const auto                  stems     = TemplateStems( editorDir / "Resources" / "Shaders" );
    ASSERT_GE( stems.size(), 3u ) << "the template census found too few templates to mean anything";
    for ( const char* expected : { "Unlit", "StaticMeshPBR", "Terrain" } )
        EXPECT_NE( std::find( stems.begin(), stems.end(), expected ), stems.end() ) << expected;

    std::vector<std::string> offenders;
    for ( const char* root :
          { "Desert/Desert/Source", "Desert/Common/Source", "Editor/Source", "Runtime/Source" } )
    {
        std::error_code ec;
        for ( const auto& entry : std::filesystem::recursive_directory_iterator( repo / root, ec ) )
        {
            const auto ext = entry.path().extension();
            if ( !entry.is_regular_file() || ( ext != ".cpp" && ext != ".hpp" && ext != ".h" ) )
                continue;
            const std::string rel = entry.path().lexically_relative( repo ).generic_string();
            std::ifstream     in( entry.path() );
            int               number = 0;
            for ( std::string line; std::getline( in, line ); )
            {
                ++number;
                const auto first = line.find_first_not_of( " \t" );
                if ( first == std::string::npos )
                    continue;
                const std::string_view text = std::string_view( line ).substr( first );
                if ( text.starts_with( "//" ) || text.starts_with( "*" ) || text.starts_with( "/*" ) )
                    continue;
                for ( const auto& stem : stems )
                {
                    if ( line.find( "\"" + stem + "\"" ) == std::string::npos )
                        continue;
                    const bool allowed = std::any_of(
                         std::begin( kAllowedTemplateNames ), std::end( kAllowedTemplateNames ),
                         [&]( const AllowedTemplateName& a ) { return rel == a.File && stem == a.Name; } );
                    if ( !allowed )
                        offenders.push_back( std::format( "{}:{}: \"{}\"", rel, number, stem ) );
                }
            }
        }
    }
    std::string list;
    for ( const auto& o : offenders )
        list += o + "\n";
    EXPECT_TRUE( offenders.empty() ) << "a template chosen by name (use its handle / role, or allow-list it by "
                                        "file with a reason):\n"
                                     << list;
}
