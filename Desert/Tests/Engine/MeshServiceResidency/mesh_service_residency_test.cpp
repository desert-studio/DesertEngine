// MeshService, EXECUTED WITHOUT A DEVICE (AL1-5c).
//
// The service decides four things a frame depends on: a mesh whose bytes are not in memory is PENDING and
// draws nothing; the read it needs goes to AsyncAssetLoader, never to the frame; the mesh the read produced
// is built on the next ask and drawn from then on; and a build is only cached when it matches the asset it
// came from. Until the device half moved behind IMeshUploader none of that could run in a suite — the
// order of BuildAndCache was pinned over the source text in StaticMeshCooked instead. Here the uploader is
// a fake that records what it was handed, and everything else is the real service.

#include <gtest/gtest.h>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Assets/SyncLoadLedger.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Runtime/Services/Mesh/MeshService.hpp>

#include <Common/Core/AssetPathIndex.hpp>
#include <Common/Core/Constants.hpp>

#include <filesystem>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using namespace Desert;
using Assets::AsyncAssetLoader;
using Assets::SyncLoadLedger;

namespace
{
    std::atomic<int> g_LiveMeshes{ 0 };

    /// A parsed-on-a-worker mesh asset with no file behind it: its LoadFromFile fills two submeshes.
    class ProbeMeshAsset final : public Assets::MeshAsset
    {
    public:
        explicit ProbeMeshAsset( const std::string& name )
             : MeshAsset( Common::Filepath( name ), Assets::AssetTypeID::Unknown )
        {
        }

        Common::BoolResultStr Unload() override
        {
            m_Submeshes.clear();
            m_Ready = false;
            return BOOLSUCCESS;
        }
        bool IsReadyForUse() const override
        {
            return m_Ready;
        }
        const std::vector<Common::UUID>& GetMaterialHandles() const override
        {
            return m_Materials;
        }
        bool IsSkinned() const override
        {
            return false;
        }
        const std::vector<Submesh>& GetSubmeshes() const override
        {
            return m_Submeshes;
        }

        std::atomic<int> Reads{ 0 };

    protected:
        Common::BoolResultStr LoadFromFile() override
        {
            Reads.fetch_add( 1 );
            m_Submeshes.assign( 2, Submesh{} );
            m_Ready = true;
            return BOOLSUCCESS;
        }

    private:
        std::atomic<bool>         m_Ready{ false };
        std::vector<Submesh>      m_Submeshes;
        std::vector<Common::UUID> m_Materials;
    };

    class FakeMesh final : public Mesh
    {
    public:
        explicit FakeMesh( const std::size_t submeshes )
        {
            m_Submeshes.assign( submeshes, Submesh{} );
            ++g_LiveMeshes;
        }
        ~FakeMesh() override
        {
            --g_LiveMeshes;
        }
        [[nodiscard]] MeshType GetType() const override
        {
            return MeshType::Static;
        }
        Common::BoolResultWithCodes<MeshError> Invalidate() override
        {
            return Common::MakeSuccessWithCodes<bool, MeshError>( true );
        }
    };

    /// The device half, recorded. `SubmeshSkew` makes the build disagree with its asset.
    struct FakeUploader final : Runtime::IMeshUploader
    {
        std::shared_ptr<int> Uploads         = std::make_shared<int>( 0 );
        std::shared_ptr<int> UploadsOfShells = std::make_shared<int>( 0 );
        std::shared_ptr<int> SubmeshSkew     = std::make_shared<int>( 0 );

        std::shared_ptr<Mesh> Upload( const std::shared_ptr<Assets::MeshAsset>& asset ) override
        {
            ++*Uploads;
            if ( !asset->IsReadyForUse() )
                ++*UploadsOfShells;
            return std::make_shared<FakeMesh>( asset->GetSubmeshes().size() + *SubmeshSkew );
        }
        Common::BoolResultStr UploadProcedural( const std::shared_ptr<Mesh>&, const Assets::AssetHandle& ) override
        {
            return BOOLSUCCESS;
        }
    };

    class MeshServiceResidency : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            AsyncAssetLoader::Get().ResetForTest();
            SyncLoadLedger::ResetForTest();

            auto uploader = std::make_unique<FakeUploader>();
            Uploads       = uploader->Uploads;
            ShellUploads  = uploader->UploadsOfShells;
            Skew          = uploader->SubmeshSkew;
            Service       = std::make_unique<Runtime::MeshService>( std::move( uploader ) );

            Manager = std::make_shared<Assets::AssetManager>();
            Asset   = std::make_shared<ProbeMeshAsset>( "Probe/residency_probe.stmesh" );
            Handle  = Asset->GetMetadata().Handle;
            ASSERT_TRUE( Service->RegisterAsset( Asset, Manager ) );

            // Every ask below is a FRAME's ask: a read on this thread from here on is a hitch.
            SyncLoadLedger::NoteBootFinished();
        }

        void TearDown() override
        {
            Service.reset();
            AsyncAssetLoader::Get().ShutdownAndDrain();
        }

        /// One frame's worth of the loader: deliver whatever the workers finished. Bounded, so a wedged
        /// read fails the test instead of hanging it.
        static bool PumpUntilQuiet()
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5 );
            while ( std::chrono::steady_clock::now() < deadline )
            {
                AsyncAssetLoader::Get().Pump();
                if ( AsyncAssetLoader::Get().Outstanding() == 0 )
                    return true;
                std::this_thread::yield();
            }
            return false;
        }

        std::unique_ptr<Runtime::MeshService> Service;
        std::shared_ptr<Assets::AssetManager> Manager;
        std::shared_ptr<ProbeMeshAsset>       Asset;
        Assets::AssetHandle                   Handle{ static_cast<uint64_t>( 0 ) };
        std::shared_ptr<int>                  Uploads;
        std::shared_ptr<int>                  ShellUploads;
        std::shared_ptr<int>                  Skew;
    };
} // namespace

// PENDING DRAWS NOTHING, AND ASKING DOES NOT READ. The first ask about an unparsed mesh answers nullptr and
// hands the read to a worker; it neither builds a mesh out of the shell nor stops the frame to parse it.
TEST_F( MeshServiceResidency, APendingMeshIsNotDrawnAndItsAskReadsNothingInTheFrame )
{
    EXPECT_EQ( Service->Get( Handle ), nullptr ) << "a mesh whose bytes are not in memory was handed out to draw";
    EXPECT_EQ( *Uploads, 0 ) << "the service uploaded a mesh it had not parsed yet";
    EXPECT_TRUE( AsyncAssetLoader::Get().IsRequested( Handle ) )
         << "pending, but no read was requested — the mesh would never arrive";
    EXPECT_EQ( SyncLoadLedger::InFrameLoads(), 0u ) << "the ask parsed the mesh on the frame's thread";
}

// ARRIVAL DRAWS FROM THE NEXT ASK, ONCE. After the loader delivers, the next Get builds from the PARSED
// asset, and the ask after that is the cached mesh — one upload, no read in the frame at any point.
TEST_F( MeshServiceResidency, AnArrivedMeshIsDrawnFromTheNextAskAndBuiltOnce )
{
    ASSERT_EQ( Service->Get( Handle ), nullptr );
    ASSERT_TRUE( PumpUntilQuiet() );

    const Mesh* drawn = Service->Get( Handle );
    ASSERT_NE( drawn, nullptr ) << "the read arrived and the next ask still drew nothing";
    EXPECT_EQ( drawn->GetSubmeshes().size(), 2u );
    EXPECT_EQ( Service->Get( Handle ), drawn )
         << "a second ask rebuilt the mesh instead of drawing the cached one";
    EXPECT_EQ( *Uploads, 1 );
    EXPECT_EQ( *ShellUploads, 0 ) << "the uploader was handed an unparsed shell";
    EXPECT_EQ( Asset->Reads.load(), 1 );
    EXPECT_EQ( SyncLoadLedger::InFrameLoads(), 0u );
}

// THE SCENE-OPEN WAIT (AL1-5) IS A WORKER READ TOO. AwaitResident blocks the caller until the mesh is
// drawable, and the read it waited for is counted as async, not as a frame that stopped to read.
TEST_F( MeshServiceResidency, AwaitResidentMakesTheMeshDrawableWithoutAnInFrameRead )
{
    const std::array<Assets::AssetHandle, 1> handles{ Handle };
    EXPECT_EQ( Service->AwaitResident( handles ), 1u ) << "the awaited mesh is not drawable after the wait";
    EXPECT_NE( Service->Get( Handle ), nullptr );
    EXPECT_EQ( SyncLoadLedger::InFrameLoads(), 0u ) << "the wait read the mesh on the frame's thread";
    EXPECT_EQ( *ShellUploads, 0 );
}

// A BUILD THAT DISAGREES WITH ITS ASSET IS NOT CACHED — the relation StaticMeshCooked used to pin over the
// source text. An empty mesh is a perfectly constructible mesh; caching one is irreversible.
TEST_F( MeshServiceResidency, ABuildWhoseSubmeshesDisagreeWithTheAssetIsNeverDrawn )
{
    *Skew = 1;
    ASSERT_EQ( Service->Get( Handle ), nullptr );
    ASSERT_TRUE( PumpUntilQuiet() );

    EXPECT_EQ( Service->Get( Handle ), nullptr )
         << "a mesh with 3 submeshes built from a 2-submesh asset was drawn";
    EXPECT_EQ( Service->Get( Handle ), nullptr );
    EXPECT_EQ( *Uploads, 1 ) << "a failed mesh is final; the service rebuilt it on every ask";
    EXPECT_EQ( g_LiveMeshes.load(), 0 ) << "the refused build is still held somewhere";
}

// UNLOAD IS FORGETTING. Eviction drops the built mesh — its buffers go with its destructor — and the next
// ask rebuilds from the still-parsed asset without reading the file again.
TEST_F( MeshServiceResidency, AnEvictedMeshReleasesItsBuffersAndRebuildsWithoutAReread )
{
    ASSERT_EQ( Service->Get( Handle ), nullptr );
    ASSERT_TRUE( PumpUntilQuiet() );
    ASSERT_NE( Service->Get( Handle ), nullptr );
    ASSERT_EQ( g_LiveMeshes.load(), 1 );

    // WP13: an evicted mesh is PARKED with the sweep's absolute frame and released once every frame recorded
    // before the sweep has had its fence waited on (FrameRetireQueue) - not at the eviction itself.
    auto& frames = Engine::FrameManager::CreateInstance();
    EXPECT_TRUE( Service->EvictBuilt( Handle ) );
    EXPECT_EQ( g_LiveMeshes.load(), 1 ) << "released while frames recorded before the sweep may still read it";
    for ( uint32_t i = 0; i < frames.GetMaxFramesInFlight(); ++i )
        frames.NextFrame();
    EXPECT_EQ( Service->RetireEvicted(), 0u ) << "released one frame before the margin";
    frames.NextFrame();
    EXPECT_EQ( Service->RetireEvicted(), 1u );
    EXPECT_EQ( g_LiveMeshes.load(), 0 ) << "retirement kept the device buffers alive";

    EXPECT_NE( Service->Get( Handle ), nullptr );
    EXPECT_EQ( *Uploads, 2 );
    EXPECT_EQ( Asset->Reads.load(), 1 );
    EXPECT_EQ( SyncLoadLedger::InFrameLoads(), 0u );
}

// THE CLOSURE A SCENE OPEN WAITS FOR IS THE REGISTRY'S `deps` COLUMN, WALKED (AL1-8b). A mesh reaches its
// material, the material its parent and its texture; a cycle back to the instance is walked once, an edge to no
// row is dropped (no file to read), a row nothing names stays out, and a ROOT with no row is kept with the kind
// its caller stated — a mesh cooked this session before its row was noted must still be read.
TEST( SceneClosure, TheDepsColumnIsWalkedTransitivelyOnceEachAndRootsKeepTheirKind )
{
    Common::Utils::AssetRegistry registry;
    const auto                   row =
         [&registry]( const char* key, const char* kind, uint64_t identity, std::vector<uint64_t> deps )
    {
        Common::Utils::AssetRegistryEntry entry;
        entry.Key          = key;
        entry.Kind         = kind;
        entry.Size         = 1;
        entry.Identity     = identity;
        entry.Dependencies = std::move( deps );
        ASSERT_TRUE( registry.Insert( std::move( entry ) ).IsSuccess() ) << key;
    };
    row( "assets:Meshes/m.stmesh", "StaticMesh", 0x10, { 0x20 } );
    row( "assets:Materials/inst.demat", "Material", 0x20, { 0x21, 0x30, 0x99 } );
    row( "assets:Materials/parent.demat", "Material", 0x21, { 0x20 } );
    row( "assets:Textures/t.detex", "Texture", 0x30, {} );
    row( "assets:Materials/other.demat", "Material", 0x40, {} );
    (void)Assets::ContentRegistry::Detail::Publish( std::move( registry ) );

    auto closure = Assets::ContentRegistry::Closure(
         { { Assets::AssetHandle( static_cast<uint64_t>( 0x10 ) ), "StaticMesh" },
           { Assets::AssetHandle( static_cast<uint64_t>( 0x55 ) ), "StaticMesh" } } );
    Assets::ContentRegistry::ResetForTest();

    std::vector<std::pair<uint64_t, std::string>> got;
    got.reserve( closure.size() );
    for ( const auto& entry : closure )
        got.emplace_back( static_cast<uint64_t>( entry.Handle ), entry.Kind );
    std::sort( got.begin(), got.end() );
    const std::vector<std::pair<uint64_t, std::string>> expected{ { 0x10, "StaticMesh" },
                                                                  { 0x20, "Material" },
                                                                  { 0x21, "Material" },
                                                                  { 0x30, "Texture" },
                                                                  { 0x55, "StaticMesh" } };
    EXPECT_EQ( got, expected );
}

// A SKINNED MESH THAT FAILED ON ITS MISSING RIG IS DRAWN ONCE THE RIG IS WRITTEN (THM1l-b12). The import
// used to be able to write the mesh before its skeleton, and a draw or thumbnail in between failed the mesh
// for the whole session. UE retries a load that failed on a missing dependency when the dependency is added;
// here the registry's write journal is that event: the Skeleton row stating the awaited signature re-arms it.
TEST_F( MeshServiceResidency, ASkinnedMeshFailedForItsMissingRigIsDrawnOnceTheRigIsWritten )
{
    namespace fs   = std::filesystem;
    namespace Path = Common::Constants::Path;
    fs::path repo;
    for ( fs::path here = fs::current_path(); !here.empty(); here = here.parent_path() )
    {
        if ( fs::exists( here / ".gitignore" ) && fs::exists( here / "Editor" ) )
        {
            repo = here;
            break;
        }
        if ( here == here.parent_path() )
            break;
    }
    ASSERT_FALSE( repo.empty() );
    const fs::path shipped = repo / "Editor/Resources/Assets/Meshes/Skinned";

    const Path::ProjectRootState saved = Path::CurrentProjectRoot();
    const fs::path               project =
         fs::temp_directory_path() /
         ( "mesh_service_rig_" + std::to_string( ::testing::UnitTest::GetInstance()->random_seed() ) );
    fs::remove_all( project );
    const fs::path dir = project / "Resources/Assets/Meshes/Skinned";
    fs::create_directories( dir );
    Path::SetProjectRoot( project, "Resources/Assets" );
    Assets::ContentRegistry::ResetForTest();
    std::vector<std::string> refused;
    ASSERT_TRUE( Assets::ContentRegistry::Gather( &refused ) );

    // The mesh is written first and asked for before its rig exists.
    fs::copy_file( shipped / "SkinProbe.skmesh", dir / "SkinProbe.skmesh" );
    Assets::ContentRegistry::NoteFile( dir / "SkinProbe.skmesh" );
    const auto meshes = Assets::ContentRegistry::WrittenSince( Common::Content::ContentKind::SkinnedMesh, 0 );
    ASSERT_EQ( meshes.Rows.size(), 1U );
    const Assets::AssetHandle mesh = meshes.Rows.front().Handle;

    EXPECT_EQ( Service->Get( mesh ), nullptr );
    const std::array<Assets::AssetHandle, 1> asked{ mesh };
    EXPECT_EQ( Service->AwaitResident( asked ), 0U ) << "a mesh with no rig on record was built";

    // An unrelated write does not re-arm it: still failed, and nothing is read for it.
    fs::copy_file( shipped / "SkinProbe_Tilt.anim", dir / "SkinProbe_Tilt.anim" );
    Assets::ContentRegistry::NoteFile( dir / "SkinProbe_Tilt.anim" );
    EXPECT_EQ( Service->AwaitResident( asked ), 0U );

    // The rig is written: the same service, the same entry, now drawn.
    fs::copy_file( shipped / "SkinProbe.skeleton", dir / "SkinProbe.skeleton" );
    Assets::ContentRegistry::NoteFile( dir / "SkinProbe.skeleton" );
    EXPECT_EQ( Service->AwaitResident( asked ), 1U )
         << "the mesh failed for its missing rig stays failed after the rig was written";
    EXPECT_NE( Service->Get( mesh ), nullptr );
    EXPECT_EQ( SyncLoadLedger::InFrameLoads(), 0u );

    Service.reset();
    AsyncAssetLoader::Get().ShutdownAndDrain();
    Path::SetProjectRoot( saved.ProjectDir, saved.AssetsRoot );
    Assets::ContentRegistry::ResetForTest();
    Common::AssetPathIndex::Clear();
    fs::remove_all( project );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
