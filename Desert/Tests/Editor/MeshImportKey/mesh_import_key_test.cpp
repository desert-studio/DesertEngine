// THE MESH IMPORTER'S KEY — a suite about a COLLISION BY CONSTRUCTION, not about a hash.
//
// What was broken. An imported material's stable id was hashed from `<fileStem>::<materialName>#<index>`,
// and the material's file was written to `Materials/<fileStem>/<materialName>.demat`. Neither carries the
// source's DIRECTORY. Two meshes with the same file name in different folders were therefore one asset as
// far as the importer was concerned: same id, same output folder — and since the writer skips a .demat
// that already exists, the second mesh did not overwrite the first, it silently ADOPTED it. The second
// model came in wearing the first one's surface, with nothing logged and nothing null.
//
// Why this suite and not the registry's type check. The two colliding records are both Materials. A
// lookup that verifies the stored type against the requested one — which AssetManager now does — cannot
// tell two Materials apart and never could. This one has to be fixed where it is made: in the key.
//
// The repository already stands one file away from it. Assets/Meshes/base.fbx, base_basic_pbr.fbx and
// base_basic_shaded.fbx each contain a material named "model" (hence three files all called model.demat,
// in three folders). All three keys are `<stem>::model#0`; the ONLY thing separating them is that the
// three stems differ. Drop a second base.fbx in from any other pack, in any folder, and two of them merge.
//
// The tests below assert a RELATION in both directions: two sources that are different assets must never
// produce one key, and a source that has not moved must never produce a different one — the second half
// is what keeps the three .demat files committed in this repository valid, and it is asserted against
// their literal ids rather than against a restatement of the formula.

#include <gtest/gtest.h>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>

#include <Editor/Import/CookPaths.hpp>
#include <Editor/Import/MaterialAdoption.hpp>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace CookPaths = Desert::Editor::CookPaths;

namespace
{
    // Restores the project root CookPaths reads through. The content directories are process-wide
    // state, so a test that opens a project and walks away leaves every test after it measuring that
    // project; the directories are all derived from the ONE root pair, so the pair is the whole state
    // worth saving.
    class ProjectRootGuard
    {
    public:
        ProjectRootGuard() : m_Saved( Common::Constants::Path::CurrentProjectRoot() )
        {
        }

        ~ProjectRootGuard()
        {
            Common::Constants::Path::SetProjectRoot( m_Saved.ProjectDir, m_Saved.AssetsRoot );
        }

    private:
        Common::Constants::Path::ProjectRootState m_Saved;
    };

    // A project opened the way the editor opens one, so that the roots under test are remapped roots and
    // not the built-in sandbox defaults. The folder NAMES are arbitrary; what every test here measures is
    // where a source sits RELATIVE to them.
    void OpenProject()
    {
        Common::Constants::Path::SetProjectRoot( std::filesystem::path( "Project" ), "Content" );
    }

    std::filesystem::path MeshSource( const std::string& relativeToMeshes )
    {
        return Common::Constants::Path::MESH_PATH / relativeToMeshes;
    }
} // namespace

TEST( MeshImportKey, TwoSameNamedMeshesInDifferentFoldersDoNotShareAMaterialKey )
{
    ProjectRootGuard guard;
    OpenProject();

    // The sabotage, written as the scenario rather than as a mutation of the code: two source files whose
    // ONLY difference is the folder they sit in, and a material name they happen to share — which is the
    // normal case, not a contrived one ("model", "Material", "lambert1" are what exporters emit).
    const auto propsBase = MeshSource( "Props/base.fbx" );
    const auto charsBase = MeshSource( "Characters/base.fbx" );

    const std::string propsKey = CookPaths::MaterialKey( propsBase, "model", 0 );
    const std::string charsKey = CookPaths::MaterialKey( charsBase, "model", 0 );

    EXPECT_NE( propsKey, charsKey )
         << "both meshes derive the material key '" << propsKey
         << "', so both stamp the same MaterialId into their submeshes and both write into one folder — "
            "where the importer's write-only-if-missing rule makes the second mesh adopt the first's "
            "material instead of getting its own";

    // The id is what actually reaches the submesh and the registry, so assert THAT and not only the string
    // it is hashed from: a derivation that folded the two keys back together would satisfy the line above.
    EXPECT_NE( Common::AssetHandle::FromKey( propsKey ), Common::AssetHandle::FromKey( charsKey ) );

    // And the material FILES must not land on top of each other either. Same folder plus same name is the
    // half of the collision that survives even when the ids differ: the second mesh's .demat is never
    // written, so its new id resolves to nothing at all.
    EXPECT_NE( CookPaths::MaterialFolder( propsBase ), CookPaths::MaterialFolder( charsBase ) );
}

TEST( MeshImportKey, MaterialsWithinOneMeshStaySeparate )
{
    ProjectRootGuard guard;
    OpenProject();

    // The companion: a key that answered "different" to everything above could be a counter, and a key
    // that ignored the material entirely would collapse every material of one mesh into one record.
    const auto source = MeshSource( "Props/base.fbx" );

    std::set<std::string> keys;
    keys.insert( CookPaths::MaterialKey( source, "model", 0 ) );
    keys.insert( CookPaths::MaterialKey( source, "glass", 1 ) );
    // Two slots that share a NAME — exporters do emit this — separated by their index alone.
    keys.insert( CookPaths::MaterialKey( source, "model", 2 ) );

    EXPECT_EQ( keys.size(), 3u ) << "two materials of one mesh collapsed onto one key";
}

TEST( MeshImportKey, AMeshThatHasNotMovedKeepsItsKey )
{
    ProjectRootGuard guard;
    OpenProject();

    // The other direction of the relation, and the reason no content in this repository had to be
    // rewritten when the key gained the directory: for a mesh sitting directly in Assets/Meshes — where
    // all three of the repository's meshes sit — the directory-relative identity IS the stem, so the key
    // is character-for-character what it was.
    //
    // These are the ids literally present in Resources/Assets/Materials/<stem>/model.demat, read out of
    // the committed files. Asserting against the FILES rather than against a second copy of the formula is
    // what makes this a test: if the derivation drifts, every material in the repository silently stops
    // resolving, and this line is the only thing that says so.
    struct Committed
    {
        const char* stem;
        uint64_t    materialId;
    };
    const Committed committed[] = {
         { "base", 4958558474483748124ull },
         { "base_basic_pbr", 17955490653248971586ull },
         { "base_basic_shaded", 1820073035653820619ull },
    };

    for ( const auto& entry : committed )
    {
        const auto source = MeshSource( std::string( entry.stem ) + ".fbx" );

        EXPECT_EQ( CookPaths::MeshRelativeId( source ).generic_string(), entry.stem )
             << "a mesh directly under Assets/Meshes must still identify as its own name";

        const auto id = Common::AssetHandle::FromKey( CookPaths::MaterialKey( source, "model", 0 ) );
        EXPECT_EQ( static_cast<uint64_t>( id ), entry.materialId )
             << "the id derived for " << entry.stem
             << "'s 'model' material no longer matches the one committed in its .demat, so the mesh's "
                "submesh reference and the material asset have come apart";

        EXPECT_EQ( CookPaths::MaterialFolder( source ), Common::Constants::Path::MATERIAL_PATH / entry.stem )
             << "the committed .demat for " << entry.stem << " is no longer where the importer writes";
    }
}

TEST( MeshImportKey, ASourceOutsideTheMeshFolderKeepsItsPlaceInTheKey )
{
    ProjectRootGuard guard;
    OpenProject();

    // Mesh sources are not only found in Assets/Meshes — a character pack lands in
    // Assets/Collections/<pack>/ and cooks through the same ladder. Two packs shipping a "body.fbx" is the
    // same collision as above, arriving by the route the engine actually uses for third-party content.
    const auto packA = Common::Constants::Path::ASSETS_PATH / "Collections/PackA/body.fbx";
    const auto packB = Common::Constants::Path::ASSETS_PATH / "Collections/PackB/body.fbx";

    EXPECT_EQ( CookPaths::MeshRelativeId( packA ).generic_string(), "Collections/PackA/body" );
    EXPECT_NE( CookPaths::MaterialKey( packA, "Material", 0 ), CookPaths::MaterialKey( packB, "Material", 0 ) );
    EXPECT_NE( CookPaths::MaterialFolder( packA ), CookPaths::MaterialFolder( packB ) );
}

// AN EXISTING .demat IS ADOPTED, NOT SHADOWED (AF4a). The writer keeps a .demat that already exists, and the
// repository's committed materials were lifted by a migrator under GUIDs of their own — so the GUID the
// importer derives from the mesh's key is NOT the one the file states. The importer's cooked mesh used to
// stamp the derived GUID into its submeshes anyway, and every dependency it declared named no registry row.
// The ImportResult below has the importer's shape: each material under its derived GUID, each submesh naming
// its material by that GUID; CreateAssetsFromImport runs exactly this adoption before it writes anything.
namespace
{
    namespace Adoption = Desert::Editor::MaterialAdoption;
    using Common::Content::AssetGuid;

    void WriteText( const std::filesystem::path& path, const std::string& text )
    {
        std::filesystem::create_directories( path.parent_path() );
        std::ofstream( path, std::ios::binary ) << text;
    }

    std::string MaterialFileStating( const AssetGuid& guid )
    {
        return R"({"Header":{"Kind":"Material","Guid":")" + Common::Content::AssetGuidToText( guid ) +
               R"(","Versions":{},"Dependencies":[]},"Material":{}})";
    }

    class TempProject
    {
    public:
        TempProject()
            : m_Root( std::filesystem::temp_directory_path() /
                      ( "MeshImportKey_" + std::to_string( ::testing::UnitTest::GetInstance()->random_seed() ) +
                        "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name() ) )
        {
            std::filesystem::remove_all( m_Root );
            Common::Constants::Path::SetProjectRoot( m_Root, "Content" );
        }
        ~TempProject()
        {
            std::error_code ec;
            std::filesystem::remove_all( m_Root, ec );
        }

    private:
        std::filesystem::path m_Root;
    };

    Desert::Editor::ImportResult ImportedTwoMaterials( const AssetGuid& modelDerived, const AssetGuid& trimDerived )
    {
        Desert::Editor::ImportResult result;
        result.Materials.push_back( { .Name = "model", .Data = {}, .Guid = modelDerived, .Textures = {} } );
        result.Materials.push_back( { .Name = "trim", .Data = {}, .Guid = trimDerived, .Textures = {} } );
        result.Mesh.emplace();
        result.Mesh->Submeshes.resize( 3 );
        result.Mesh->Submeshes[0].MaterialGuid = modelDerived;
        result.Mesh->Submeshes[1].MaterialGuid = trimDerived;
        result.Mesh->Submeshes[2].MaterialGuid = modelDerived;
        return result;
    }
} // namespace

TEST( MeshImportKey, AnExistingMaterialFileGivesTheSubmeshItsGuid )
{
    ProjectRootGuard guard;
    TempProject      project;

    const auto      source       = MeshSource( "Props/crate.fbx" );
    const AssetGuid modelDerived = { 0x1111, 0x2222 };
    const AssetGuid trimDerived  = { 0x3333, 0x4444 };
    const AssetGuid onDisk       = { 0x0e9b8e4c00000000ull, 0x00000000abcdef01ull };
    WriteText( Adoption::MaterialAssetPath( source, "model" ), MaterialFileStating( onDisk ) );

    auto       result  = ImportedTwoMaterials( modelDerived, trimDerived );
    const auto adopted = Adoption::AdoptExistingMaterials( result, source );
    ASSERT_TRUE( adopted ) << adopted.GetError();

    EXPECT_EQ( result.Materials[0].Guid, onDisk ) << "the material that exists keeps the GUID its file states";
    EXPECT_EQ( result.Mesh->Submeshes[0].MaterialGuid, onDisk )
         << "the submesh still names the derived GUID, which no .demat states: its dependency dangles";
    EXPECT_EQ( result.Mesh->Submeshes[2].MaterialGuid, onDisk ) << "EVERY submesh of the material is re-pointed";
    // A material with no file yet is written under the derived GUID, so the derived GUID is the right name.
    EXPECT_EQ( result.Materials[1].Guid, trimDerived );
    EXPECT_EQ( result.Mesh->Submeshes[1].MaterialGuid, trimDerived );
}

TEST( MeshImportKey, AMaterialFileWithNoReadableGuidRefusesTheImportByName )
{
    ProjectRootGuard guard;
    TempProject      project;

    const auto source = MeshSource( "Props/crate.fbx" );
    const auto path   = Adoption::MaterialAssetPath( source, "model" );
    WriteText( path, R"({"Material":{}})" );

    auto       result  = ImportedTwoMaterials( { 1, 2 }, { 3, 4 } );
    const auto adopted = Adoption::AdoptExistingMaterials( result, source );
    ASSERT_FALSE( adopted ) << "a .demat whose GUID cannot be read must not leave the mesh naming a guess";
    EXPECT_NE( adopted.GetError().find( path.filename().string() ), std::string::npos ) << adopted.GetError();
}

TEST( MeshImportKey, TheWriterAndTheAdoptionNameOneFile )
{
    ProjectRootGuard guard;
    OpenProject();
    const auto source = MeshSource( "Props/crate.fbx" );
    EXPECT_EQ( Adoption::MaterialAssetPath( source, "wood panel" ),
               CookPaths::MaterialFolder( source ) / "wood_panel.demat" );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
