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

#include <filesystem>
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
        // Desert/Tests/Engine/EngineShaderByGuid/engine_shader_by_guid_test.cpp -> repo root is four
        // directories up from this file's parent.
        const std::filesystem::path here = std::filesystem::path( __FILE__ ).parent_path();
        return std::filesystem::weakly_canonical( here / ".." / ".." / ".." / ".." / "Editor" );
    }

    // Changes the process's working directory to `Editor/` for the lifetime of the guard and restores it
    // after — `Common::Constants::Path::RESOURCE_PATH` ("Resources/") is a literal relative to the
    // working directory and is deliberately never remapped by a project (StartupLayout.hpp), so it is
    // the ONE thing a test that wants the real engine content has to set up itself.
    class WorkingDirectoryGuard
    {
    public:
        WorkingDirectoryGuard( const std::filesystem::path& next )
             : m_Previous( std::filesystem::current_path() )
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
        for ( const auto& entry :
              std::filesystem::recursive_directory_iterator( editorDir / "Resources", ec ) )
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
        if ( manager.CreateAsset<Desert::Assets::ShaderAsset>( Desert::Assets::AssetPriority::Medium,
                                                                shaderPath ) )
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
        const auto shell = manager.CreateAsset<Desert::Assets::SurfaceMaterialAsset>(
             Desert::Assets::AssetPriority::Medium, materialPath, /*loadAfterCreate=*/false );
        if ( !shell )
        {
            unresolved.push_back( materialPath.string() + ": did not create as a shell" );
            continue;
        }

        if ( !shell->IsReadyForUse() )
            (void)shell->EnsureLoaded( manager );

        const Desert::Assets::MaterialData& data = shell->Data();
        if ( !data.Shader.has_value() )
            continue; // states no shader by absence — the standard surface, not this defect's shape.

        if ( shell->GetShaderName().empty() )
        {
            unresolved.push_back( std::format(
                 "{}: shader GUID {} ('{}') did not resolve to a name", materialPath.string(),
                 data.Shader->Guid, data.Shader->Path ) );
        }
    }

    std::string joined;
    for ( const std::string& line : unresolved )
        joined += "\n  " + line;
    EXPECT_TRUE( unresolved.empty() )
         << unresolved.size() << " material(s) did not resolve their shader:" << joined;
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
