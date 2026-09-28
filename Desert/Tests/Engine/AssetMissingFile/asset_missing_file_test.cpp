// The MISSING-FILE branch of the loaders, which was DEAD CODE until 2026-09-05. Each of these
// loaders was written with a deliberate answer to "the file is not there":
//
//   - SurfaceMaterialAsset::Load: "New / empty material — canonical defaults; editable and
//     re-savable" (a success, by design — the editor creates materials by naming a file that does
//     not exist yet);
//   - CloudTypeAsset::Load: "a file that is missing ... is an ERROR carrying the reason" (its own
//     header says so);
//   - PrefabAsset::Load: a missing file is the read's own named error, an empty file is "Prefab
//     file is empty" — both errors (the scene loader logs and
//     survives — covered by the live editor run, not here, because PrefabAsset.cpp includes
//     Scene.hpp and no GPU-free suite can compile it).
//
// None of those branches could execute for a genuinely absent file, because
// FileSystem::ReadFileContent aborted the process before returning. These tests pin the branches
// now that the primitive is soft: each Load RETURNS (the suite being alive is half the assertion)
// and answers with exactly the policy its author wrote. Reverting the primitive's miss path to
// DESERT_VERIFY kills this suite outright.

#include <Engine/Assets/CloudLayoutAsset.hpp>
#include <Engine/Assets/CloudTypeAsset.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Assets/SyncLoadLedger.hpp>
#include <Engine/Runtime/Services/CloudType/CloudTypeService.hpp>

#include <chrono>
#include <thread>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Assets/RegistryDiscovery.hpp>
#include <Engine/Assets/Skybox/SkyboxAsset.hpp>
#include <Engine/Assets/Serialization/EnvironmentStaging.hpp>

#include <Common/Content/ContentKinds.hpp>
#include <Common/Core/AssetPathIndex.hpp>
#include <Common/Core/Constants.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

namespace fs = std::filesystem;

namespace
{
    fs::path MissingPath( const char* name )
    {
        const fs::path dir = fs::temp_directory_path() / "desert_missing_asset_test";
        fs::remove_all( dir );
        fs::create_directories( dir );
        return dir / name; // never created
    }

    // A file that IS there, with the given bytes. The companion MissingPath needs: half of what these
    // loaders answer wrong is "the file is not there" and half is "the file is there and unusable",
    // and until this existed the second half had nothing to build.
    fs::path PathWith( const char* name, const std::string& content )
    {
        const fs::path path = MissingPath( name );
        std::ofstream  out( path, std::ios::binary );
        out << content;
        out.close();
        return path;
    }
} // namespace

TEST( AssetMissingFile, SurfaceMaterialLoadsCanonicalDefaults )
{
    const fs::path path = MissingPath( "brand_new.demat" );
    ASSERT_FALSE( fs::exists( path ) );

    Desert::Assets::SurfaceMaterialAsset material( path );
    const auto                           result = material.Load();

    // The branch's own comment: a missing .demat is a NEW material — usable, editable, re-savable.
    EXPECT_TRUE( result.IsSuccess() );
    EXPECT_TRUE( material.IsReadyForUse() );
    // Canonical defaults: no authored parameters, and the shader falls back to the standard surface.
    EXPECT_TRUE( material.Data().Params.empty() );
    EXPECT_FALSE( material.Data().Shader.has_value() );
}

TEST( AssetMissingFile, CloudTypeLoadRefusesWithTheReason )
{
    const fs::path path = MissingPath( "gone.decloudtype" );
    ASSERT_FALSE( fs::exists( path ) );

    Desert::Assets::CloudTypeAsset type( path );
    const auto                     result = type.Load();

    // The header's contract: missing is an ERROR carrying the reason — never a quiet default type.
    ASSERT_FALSE( result.IsSuccess() );
    EXPECT_NE( result.GetError().find( "empty or could not be opened" ), std::string::npos ) << result.GetError();
    EXPECT_FALSE( type.IsReadyForUse() );
}

// A missing .demat is a new material and a SUCCESS (above). A .demat that is THERE and will not parse is
// a different thing entirely, and the difference is the authored parameters: they are still in the file,
// and the moment anything writes this asset back they are gone. The loader's own message has always said
// so ("re-saving will overwrite the file") — it just had no way to stop it, because Save() handed out the
// substituted defaults like any other material's data.
TEST( AssetMissingFile, AnUnparseableMaterialLoadsUsableAndRefusesToSaveOverItsFile )
{
    const fs::path path = PathWith( "corrupt.demat", "{ this is not json" );

    Desert::Assets::SurfaceMaterialAsset material( path );
    const auto                           result = material.Load();

    // Deliberately still a success and still usable: AssetManager::CreateAsset drops an asset whose
    // Load fails, so refusing here would delete the material from the asset database and leave every
    // mesh slot pointing at it resolving to nothing. See the comment on that branch.
    EXPECT_TRUE( result.IsSuccess() );
    EXPECT_TRUE( material.IsReadyForUse() );

    const auto saved = material.Save();
    ASSERT_FALSE( saved.IsSuccess() ) << "the material offered to serialize its substituted defaults; "
                                         "writing them out is the step that makes the loss permanent";
    EXPECT_NE( saved.GetError().find( path.string() ), std::string::npos )
         << "the refusal does not name the file the user has to fix: " << saved.GetError();

    fs::remove_all( path.parent_path() );
}

// THE SHADER IS THE ASSET'S ANSWER, not the data's. A material that states no shader draws with the
// standard surface; one that names a shader draws with exactly that one — and the two engine PBR names are
// the only ones that are not "custom" (the batched backend), which is what hot reload asks.
TEST( AssetMissingFile, AMaterialWithoutAShaderResolvesToTheStandardSurface )
{
    const fs::path                       path = MissingPath( "no_shader.demat" );
    Desert::Assets::SurfaceMaterialAsset material( path );
    ASSERT_TRUE( material.Load().IsSuccess() );

    EXPECT_FALSE( material.Data().Shader.has_value() );
    EXPECT_EQ( material.GetShaderName(), "StaticMeshPBR" );
    EXPECT_FALSE( material.UsesCustomShader() );
}

// A material naming a shader resolves its name only against a manager that holds the shader, by GUID:
// AssetHandleStability (ShaderAssetIdentity.AMaterialResolvesItsShaderNameByGuid...) pins that end.
TEST( AssetMissingFile, AParsedMaterialSavesNormally )
{
    const fs::path path = PathWith(
         "fine.demat",
         R"({"Header":{"Kind":"Material","Guid":"5a1f0c0e9d3b4e7a8c21f00d0000a001","Versions":{"MATL":4},"Dependencies":[]},"Params":[],"Textures":[],"CloudAssets":[]})" );

    Desert::Assets::SurfaceMaterialAsset material( path );
    ASSERT_TRUE( material.Load().IsSuccess() );

    const auto saved = material.Save();
    EXPECT_TRUE( saved.IsSuccess() ) << saved.GetError();
    EXPECT_FALSE( saved.GetValue().empty() );

    fs::remove_all( path.parent_path() );
}

// A PARAMETER THAT IS NOT A NUMBER IS REFUSED BY NAME, and the alternative is not a bad file.
//
// `rfl::json::write` is `std::string( yyjson_mut_write( doc, 0, nullptr ) )`. yyjson with no write flags
// has no spelling for a NaN or an infinity and answers a NULL pointer — measured on the vendored copy:
// `{"v":0.5}` writes, NaN and inf both answer NULL — and that constructor then reads it. So the failure
// mode of saving such a material is undefined behaviour inside a third-party header while the editor is
// writing the artist's work, not a `.demat` somebody can repair.
//
// IT IS REACHABLE, AND NOT THROUGH THE FILE. yyjson refuses those tokens on the way IN as well, so no
// `.demat` on disk can carry one and no test needs to pretend otherwise — this one puts the value in
// through the same door the editor does, `MaterialData::SetParam`. The editor's door is an ImGui drag,
// whose Ctrl-click text entry parses with `sscanf( buf, "%f", … )`; `%f` accepts `nan`, `inf` and `1e40`.
TEST( AssetMissingFile, AMaterialHoldingANonNumberRefusesToSaveAndNamesTheParameter )
{
    const fs::path path = PathWith(
         "not_a_number.demat",
         R"({"Header":{"Kind":"Material","Guid":"5a1f0c0e9d3b4e7a8c21f00d0000a002","Versions":{"MATL":4},"Dependencies":[]},"Params":[],"Textures":[],"CloudAssets":[]})" );

    for ( const float bad : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::infinity() } )
    {
        Desert::Assets::SurfaceMaterialAsset material( path );
        ASSERT_TRUE( material.Load().IsSuccess() );

        material.Data().SetParam( "Coverage", glm::vec4( bad, 0.0f, 0.0f, 0.0f ) );

        const auto saved = material.Save();
        ASSERT_FALSE( saved.IsSuccess() ) << "the writer was handed a value JSON cannot spell";
        EXPECT_NE( saved.GetError().find( "Coverage" ), std::string::npos )
             << "the refusal does not name the parameter the user has to fix: " << saved.GetError();
        EXPECT_NE( saved.GetError().find( path.string() ), std::string::npos ) << saved.GetError();
    }

    // The control, and it is not decorative: a guard that read the whole vec4 of every parameter would
    // refuse this one too, because a scalar's unused lanes are whatever the writer left in them. They are
    // zero here, which is what the editor writes — the point of the control is that an ORDINARY material
    // still saves after the guard exists.
    {
        Desert::Assets::SurfaceMaterialAsset material( path );
        ASSERT_TRUE( material.Load().IsSuccess() );
        material.Data().SetParam( "Coverage", glm::vec4( 0.45f, 0.0f, 0.0f, 0.0f ) );
        const auto saved = material.Save();
        EXPECT_TRUE( saved.IsSuccess() ) << saved.GetError();
        EXPECT_NE( saved.GetValue().find( "Coverage" ), std::string::npos );
    }

    fs::remove_all( path.parent_path() );
}

// A brand-new material (no file at all) must STILL save — that is how the editor creates one, and a
// refusal here would make "New Material" impossible.
TEST( AssetMissingFile, ABrandNewMaterialSavesNormally )
{
    const fs::path path = MissingPath( "brand_new_saves.demat" );

    Desert::Assets::SurfaceMaterialAsset material( path );
    ASSERT_TRUE( material.Load().IsSuccess() );

    EXPECT_TRUE( material.Save().IsSuccess() );

    fs::remove_all( path.parent_path() );
}

// SkyboxAsset::Load was `m_ReadyForUse = true; return BOOLSUCCESS;` with its only check commented out —
// it never opened the file it named. A skybox whose .hdr had been moved, renamed or left out of a
// package therefore loaded, registered and reported ready, and the sky came out black with every
// diagnostic in the editor saying the skybox was fine.
TEST( AssetMissingFile, SkyboxLoadRefusesAPanoramaThatIsNotThere )
{
    const fs::path path = MissingPath( "gone.hdr" );
    ASSERT_FALSE( fs::exists( path ) );

    Desert::Assets::SkyboxAsset skybox( path );
    const auto                  result = skybox.Load();

    ASSERT_FALSE( result.IsSuccess() );
    EXPECT_NE( result.GetError().find( path.string() ), std::string::npos ) << result.GetError();
    EXPECT_FALSE( skybox.IsReadyForUse() );
}

// The control: a check that refused everything would satisfy the test above and make every skybox in
// the project unloadable.
TEST( AssetMissingFile, SkyboxLoadAcceptsAPanoramaThatIsThere )
{
    const fs::path path = PathWith( "present.hdr", "not really an HDR, and Load does not read it" );

    Desert::Assets::SkyboxAsset skybox( path );
    EXPECT_TRUE( skybox.Load().IsSuccess() );
    EXPECT_TRUE( skybox.IsReadyForUse() );

    fs::remove_all( path.parent_path() );
}

// THE WORKER'S HALF OF THE ENVIRONMENT IS STATED, NOT SILENT (AL1-3). A skybox file that is there but is no
// cooked panorama still loads (the asset is the file's identity), and what the loader's worker staged must
// carry the sentence naming the file, because `EnvironmentManager::Create` logs exactly that and nothing else.
TEST( AssetMissingFile, SkyboxStagingNamesTheFileThatIsNoPanorama )
{
    const fs::path path = PathWith( "junk.detex", "not a cooked texture container" );

    Desert::Assets::SkyboxAsset skybox( path );
    ASSERT_TRUE( skybox.Load().IsSuccess() );
    const auto staged = skybox.Staged();
    ASSERT_NE( staged, nullptr ) << "Load ran on the worker and staged nothing";
    EXPECT_FALSE( staged->Error.empty() );
    EXPECT_FALSE( staged->Cached.has_value() );
    EXPECT_NE( staged->Error.find( path.filename().string() ), std::string::npos ) << staged->Error;

    // Unload drops what was staged, so a re-request re-reads the cache instead of binding a stale plan.
    ASSERT_TRUE( skybox.Unload().IsSuccess() );
    EXPECT_EQ( skybox.Staged(), nullptr );

    fs::remove_all( path.parent_path() );
}

// THE CACHE PLAN, WITHOUT A DEVICE: three cubes of one panorama address three files, the same inputs address
// the same file on every call, and a cube that is not there yet is a miss that names its path.
TEST( AssetMissingFile, EnvironmentCacheMissNamesTheCubeItLookedFor )
{
    using namespace Desert::Assets;
    const uint64_t source = 0x1234abcd5678ef00ull;
    const uint64_t radiance =
         EnvironmentBakeSignature( BakedEnvironmentCube::Radiance, kSkyEnvCubeFaceSize, kSkyEnvRadianceMips );
    const uint64_t irradiance =
         EnvironmentBakeSignature( BakedEnvironmentCube::Irradiance, kSkyEnvIrradianceFaceSize, 1u );
    const uint64_t prefilter = EnvironmentBakeSignature( BakedEnvironmentCube::Prefiltered,
                                                         kSkyEnvPrefilterFaceSize, kSkyEnvPrefilterMips );
    EXPECT_NE( radiance, irradiance );
    EXPECT_NE( radiance, prefilter );
    EXPECT_NE( irradiance, prefilter );
    EXPECT_NE( EnvironmentBakePath( source, radiance ), EnvironmentBakePath( source, prefilter ) );
    EXPECT_EQ( EnvironmentBakePath( source, radiance ), EnvironmentBakePath( source, radiance ) );
    EXPECT_EQ( EnvironmentBakePath( source, radiance ).extension(), ".tex" );

    const fs::path missing = MissingPath( "never-baked.tex" );
    const auto read = ReadBakedEnvironmentCube( missing, "EnvRadiance", kSkyEnvCubeFaceSize, kSkyEnvRadianceMips,
                                                source, radiance );
    ASSERT_FALSE( read.IsSuccess() );
    EXPECT_NE( read.GetError().find( missing.string() ), std::string::npos ) << read.GetError();
}

// A CLOUD TYPE IS A SCENE DEPENDENCY READ ON A WORKER, NOT IN THE FRAME (AL1-7). No boot stage reads every
// `.decloudtype`; the first layer naming one creates it from its registry row and REQUESTS the read. Until it
// lands the service answers Pending with the built-in shape; afterwards it answers the file's own shape - and
// the ledger never saw a frame stop to read it, which is what the eager boot stage used to guarantee.
TEST( AssetMissingFile, ACloudTypeNamedByHandleIsReadFromItsRegistryRowOnAWorkerThenRegistered )
{
    namespace Path            = Common::Constants::Path;
    namespace ContentRegistry = Desert::Assets::ContentRegistry;
    using Common::Content::ContentKind;
    using Desert::Assets::AsyncAssetLoader;
    using Desert::Assets::SyncLoadLedger;

    fs::path repo;
    for ( const char* prefix : { "", "../", "../../", "../../../", "../../../../" } )
        if ( fs::is_directory( fs::path( prefix ) / "Editor/Resources/Assets/Clouds/Types" ) )
        {
            repo = fs::absolute( fs::path( prefix ).empty() ? fs::path( "." ) : fs::path( prefix ) );
            break;
        }
    ASSERT_FALSE( repo.empty() ) << "could not locate the repository root from the working directory";
    const fs::path source = repo / "Editor/Resources/Assets/Clouds/Types/Cirrus.decloudtype";
    ASSERT_TRUE( fs::exists( source ) );

    // A SNAPSHOT, not a reference: the root is changed below and put back from this copy.
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    const Path::ProjectRootState saved   = Path::CurrentProjectRoot();
    const fs::path               project = fs::temp_directory_path() / "al17_on_demand_cloud_type_project";
    fs::remove_all( project );
    Path::SetProjectRoot( project, "Assets" );
    const fs::path types = *Common::Content::KindSpec( ContentKind::CloudType ).Root;
    fs::create_directories( types );
    fs::copy_file( source, types / "Cirrus.decloudtype" );

    ContentRegistry::ResetForTest();
    ASSERT_TRUE( ContentRegistry::Gather() );
    const auto rows = ContentRegistry::Rows( ContentKind::CloudType );
    ASSERT_EQ( rows.size(), 1u );
    const Desert::Assets::AssetHandle handle = rows.front().Handle;

    AsyncAssetLoader::Get().ResetForTest();
    SyncLoadLedger::ResetForTest();
    SyncLoadLedger::NoteBootFinished();
    const uint64_t inFrameBefore = SyncLoadLedger::InFrameLoads();

    const auto                        manager = std::make_shared<Desert::Assets::AssetManager>();
    Desert::Runtime::CloudTypeService service;
    service.BindAssetManager( manager );
    const uint32_t generationBefore = service.GetGeneration();

    EXPECT_EQ( &service.GetShape( handle ), &Desert::Assets::CloudTypeDefaultShape() )
         << "the first answer for an unread type must be the built-in shape, not a read in this frame";
    EXPECT_TRUE( service.IsPending( handle ) ) << "naming a registry row's type did not request its read";

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
    while ( service.IsPending( handle ) && std::chrono::steady_clock::now() < deadline )
    {
        AsyncAssetLoader::Get().Pump();
        std::this_thread::yield();
    }
    ASSERT_FALSE( service.IsPending( handle ) ) << "the type's read never landed";
    // Before any further lookup: a later GetShape would meet the ready asset and register it itself,
    // which would hide a completion that forgot to.
    EXPECT_GT( service.GetGeneration(), generationBefore )
         << "the completion did not register the type; the renderer would never rebuild its table";

    const auto asset = manager->FindByHandle<Desert::Assets::CloudTypeAsset>( handle );
    ASSERT_TRUE( asset && asset->IsReadyForUse() );
    EXPECT_NE( &service.GetShape( handle ), &Desert::Assets::CloudTypeDefaultShape() )
         << "the type landed but the service still answers the built-in shape";
    EXPECT_EQ( service.GetShape( handle ).PlacementScale, asset->GetShape().PlacementScale );
    EXPECT_EQ( SyncLoadLedger::InFrameLoads(), inFrameBefore )
         << "the type was read by stopping a frame; it must arrive through a worker";

    AsyncAssetLoader::Get().ShutdownAndDrain();
    Path::SetProjectRoot( saved.ProjectDir, saved.AssetsRoot );
    ContentRegistry::ResetForTest();
    Common::AssetPathIndex::Clear();
    fs::remove_all( project );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

// AN ON-DEMAND CLOUD KIND WHOSE FILE IS GONE IS AN ERROR THAT NAMES IT (AL1-2). The boot no longer creates a
// shell for every `.dclayout`; the service creates one from the registry row when a scene names the handle.
// When the row outlives its file, the answer must carry the path and the GUID - the old outcome was a
// clear sky and one generic line, with nothing saying which file to restore.
TEST( AssetMissingFile, AnOnDemandCloudLayoutWhoseFileIsGoneNamesThePathAndTheGuid )
{
    namespace Path            = Common::Constants::Path;
    namespace ContentRegistry = Desert::Assets::ContentRegistry;
    using Common::Content::ContentKind;

    fs::path repo;
    for ( const char* prefix : { "", "../", "../../", "../../../", "../../../../" } )
        if ( fs::is_directory( fs::path( prefix ) / "Editor/Resources/Assets/Clouds/Layouts" ) )
        {
            repo = fs::absolute( fs::path( prefix ).empty() ? fs::path( "." ) : fs::path( prefix ) );
            break;
        }
    ASSERT_FALSE( repo.empty() ) << "could not locate the repository root from the working directory";
    const fs::path source = repo / "Editor/Resources/Assets/Clouds/Layouts/PTP_Channels_Green.dclayout";
    ASSERT_TRUE( fs::exists( source ) );

    // A SNAPSHOT, not a reference: the root is changed below and put back from this copy.
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    const Path::ProjectRootState saved   = Path::CurrentProjectRoot();
    const fs::path               project = fs::temp_directory_path() / "al1_on_demand_cloud_project";
    fs::remove_all( project );
    Path::SetProjectRoot( project, "Assets" );
    const fs::path layouts = *Common::Content::KindSpec( ContentKind::CloudLayout ).Root;
    fs::create_directories( layouts );
    const fs::path file = layouts / "Painted.dclayout";
    fs::copy_file( source, file );

    ContentRegistry::ResetForTest();
    ASSERT_TRUE( ContentRegistry::Gather() );
    const auto rows = ContentRegistry::Rows( ContentKind::CloudLayout );
    ASSERT_EQ( rows.size(), 1u );
    const auto& fixtureGuid = rows.front().Guid;
    if ( !fixtureGuid.has_value() )
        FAIL() << "the fixture layout states no GUID in its header";
    const Desert::Assets::AssetHandle handle = rows.front().Handle;
    const std::string                 guid   = Common::Content::AssetGuidToText( *fixtureGuid );

    {
        // The file is there: the shell is created unread, under the number it was asked for.
        const auto manager = std::make_shared<Desert::Assets::AssetManager>();
        const auto created = Desert::Assets::CreateFromRegistryRow<Desert::Assets::CloudLayoutAsset>(
             manager, handle, ContentKind::CloudLayout );
        ASSERT_TRUE( created.IsSuccess() ) << created.GetError();
        EXPECT_EQ( created.GetValue()->GetMetadata().Handle, handle );
    }
    {
        // The same number asked for as another kind has no row of that kind. A fresh manager: one that
        // already holds the shell answers from it before the registry is consulted.
        const auto manager   = std::make_shared<Desert::Assets::AssetManager>();
        const auto wrongKind = Desert::Assets::CreateFromRegistryRow<Desert::Assets::CloudLayoutAsset>(
             manager, handle, ContentKind::CloudNoiseVolume );
        ASSERT_FALSE( wrongKind.IsSuccess() );
        EXPECT_NE( wrongKind.GetError().find( "no row of that kind" ), std::string::npos ) << wrongKind.GetError();
    }

    fs::remove( file );
    {
        const auto manager = std::make_shared<Desert::Assets::AssetManager>();
        const auto missing = Desert::Assets::CreateFromRegistryRow<Desert::Assets::CloudLayoutAsset>(
             manager, handle, ContentKind::CloudLayout );
        ASSERT_FALSE( missing.IsSuccess() ) << "a row whose file is gone produced a shell";
        EXPECT_NE( missing.GetError().find( file.generic_string() ), std::string::npos ) << missing.GetError();
        EXPECT_NE( missing.GetError().find( guid ), std::string::npos ) << missing.GetError();
    }

    Path::SetProjectRoot( saved.ProjectDir, saved.AssetsRoot );
    ContentRegistry::ResetForTest();
    Common::AssetPathIndex::Clear();
    fs::remove_all( project );
}
