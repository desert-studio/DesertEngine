// AN ASSET'S STATE MUST CATCH UP WITH ITS DEPENDENCY — a suite about a RELATION, not about a call.
//
// The relation: WHEN THE THING A DEPENDENCY IS MATCHED BY ONLY EXISTS AFTER THE FILE IS PARSED, THE
// BINDING MUST BE CORRECT AFTER THE PARSE — by whichever route the parse happened. Two routes exist and
// they must agree: an eager load inside AssetManager::CreateAsset, and a deferred load through
// AssetBase::EnsureLoaded. Asserting either one alone is what let them disagree for months.
//
// What was broken. AssetPreloader registers every mesh as an UNPARSED SHELL, because a cooked mesh is tens
// of megabytes of JSON and the editor must not read all of them to reach its first frame. CreateAsset
// resolves dependencies immediately, which for a shell means resolving against a SkinnedMeshAsset whose
// skeleton reference is still unknown — it is a field inside the .skmesh (the signature then; since SKEL-TREE
// the header's SkeletonGuid, null until the parse). Nothing asked a second time
// except the editor's drag-and-drop, so every skinned mesh loaded FROM A SCENE had a null skeleton,
// MeshFactory refused to build it, and the frame contained an invisible character plus one
// "MeshFactory: Skeleton dependency invalid" per frame forever. MeshService::Get even carried a comment
// about not caching the failed build so the mesh "can never recover once the dependency is in place" —
// the recovery was designed and the "once" never arrived.
//
// Why the null test is here too. An unparsed mesh shell names no skeleton yet (a null GUID); a resolver that
// matched "unknown" against anything would bind a mesh to an arbitrary skeleton and call it resolved, which
// is strictly worse than the unresolved state because nothing downstream can tell the difference.
//
// SKEL-TREE (contract Engine/Animation/SkeletonReference.hpp): the mesh names its rig BY THE .skeleton's GUID
// (UE USkeletalMesh::Skeleton), and ResolveDependencies binds HandleForGuid of it. The bone hash is a payload
// hash and never an identity: a rig with the SAME bones under another GUID is another skeleton.
//
// Why the identity constants are pinned. The repository ships a hand-authored probe rig + mesh
// (Editor/Cooked/Meshes/SkinProbe.*) and a scene that places it, because before that there was NOT ONE
// skinned mesh in any scene in this project and the whole skinned path was therefore unobservable. The
// scene stores the mesh's handle as a number; the rig is matched by a signature. Both are derived, and
// both are written into files that no compiler reads. Pinning them here is what makes a rename or a
// re-cook fail loudly instead of silently emptying the one scene that covers this path.

#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <gtest/gtest.h>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/MeshBinaryHeader.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>

#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Mesh/SkeletonAsset.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Assets/Serialization/Mesh.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>

#include <Common/Content/TextAssetHeader.hpp>

#include <rflcpp/rfl.hpp>
#include <rflcpp/rfl/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include "../../TestSupport/scratch_dir.hpp"

using Desert::Assets::AssetManager;
using Desert::Assets::SkeletonAsset;
using Desert::Assets::SkinnedMeshAsset;

namespace
{
    // The probe rig: ONE bone named "Root" with no parent. Skeleton::ComputeSignature hashes the sorted set
    // of "name<parentName" entries, so this rig's only entry is "Root<" and its signature is a constant this
    // suite can name. Deliberately the smallest rig that is still a rig — the defect under test has nothing
    // to do with bone count, and a probe nobody can read by eye is a probe nobody checks.
    constexpr const char*   kProbeBoneName  = "Root";
    constexpr std::uint64_t kProbeSignature = 4699069763035776985ull;

    // THE RIG'S IDENTITY: the header GUID of the shipped SkinProbe.skeleton, which SkinProbe.skmesh names in
    // its header (v5 SkeletonGuid). The scratch rig is written with the same GUID, so every test binds by the
    // identity the shipped files use.
    constexpr const char* kProbeSkeletonPath = "Resources/Assets/Meshes/Skinned/SkinProbe.skeleton";
    constexpr Common::Content::AssetGuid kProbeSkeletonGuid{ 0xacf475090f8a76b0ull, 0xeac6ee0c3e5d32fdull };

    // The identity the shipped scene stores for the shipped probe mesh: MESH_SkinnedProbe.desce names it by
    // MeshGuid, and the mesh states the same GUID in its header (AF8b: authored, committed under the assets
    // root). The runtime handle is that GUID folded (Content::HandleForGuid), so it no longer depends on
    // where the file sits.
    constexpr const char*   kProbeMeshPath   = "Resources/Assets/Meshes/Skinned/SkinProbe.skmesh";
    constexpr std::uint64_t kProbeMeshGuidHi = 0x047623816f024edfull;
    constexpr std::uint64_t kProbeMeshGuidLo = 0x9fd7a557f6501f7eull;

    // The suite data project, baked by the build (DESERT_TEST_DATA_DIR).
    std::filesystem::path DataRoot()
    {
        return Desert::TestSupport::TestDataDir();
    }

    Desert::Assets::Serialization::SkeletonAssetData
    ProbeSkeletonData( const Common::Content::AssetGuid& guid = kProbeSkeletonGuid )
    {
        Desert::Animation::BoneInfo root;
        root.Name               = kProbeBoneName;
        root.OffsetMatrix       = glm::mat4( 1.0f );
        root.LocalBindTransform = glm::mat4( 1.0f );
        root.ParentBoneID       = std::nullopt;

        Desert::Assets::Serialization::SkeletonAssetData data;
        data.Header    = Common::Content::MakeTextHeader( Common::Content::ContentKind::Skeleton, guid,
                                                          Desert::Assets::Serialization::SkeletonTextSubsystems() );
        data.Bones     = { root };
        data.Signature = Desert::Animation::Skeleton::ComputeSignature( data.Bones );
        return data;
    }

    // A single triangle bound entirely to bone 0. The geometry is irrelevant to the relation; what matters
    // is that the file names its skeleton by GUID, because that field is the dependency.
    Desert::Assets::Serialization::MeshAssetData ProbeMeshData( const Common::Content::AssetGuid& skeleton )
    {
        Desert::Assets::Serialization::MeshAssetData data;
        data.IsSkinned = true;
        data.Skeleton  = skeleton;

        for ( int i = 0; i < 3; ++i )
        {
            Desert::Assets::Serialization::SkinnedVertexData v;
            v.Position    = glm::vec3( static_cast<float>( i ), 0.0f, 0.0f );
            v.Normal      = glm::vec3( 0.0f, 1.0f, 0.0f );
            v.Tangent     = glm::vec3( 1.0f, 0.0f, 0.0f );
            v.Bitangent   = glm::vec3( 0.0f, 0.0f, 1.0f );
            v.TexCoord    = glm::vec2( 0.0f, 0.0f );
            v.BoneIDs     = { 0u, 0u, 0u, 0u };
            v.BoneWeights = { 1.0f, 0.0f, 0.0f, 0.0f };
            data.SkinnedVertices.push_back( v );
        }

        data.Indices.push_back( { 0u, 1u, 2u } );

        Desert::Assets::Serialization::SubmeshData submesh;
        submesh.Name            = "SkinProbe";
        submesh.VertexOffset    = 0;
        submesh.VertexCount     = 3;
        submesh.IndexOffset     = 0;
        submesh.IndexCount      = 3;
        submesh.Transform       = glm::mat4( 1.0f );
        submesh.BoundingBox.Min = glm::vec3( 0.0f );
        submesh.BoundingBox.Max = glm::vec3( 2.0f, 0.0f, 0.0f );
        data.Submeshes.push_back( submesh );

        return data;
    }

    void WriteText( const std::filesystem::path& path, const std::string& text )
    {
        std::filesystem::create_directories( path.parent_path() );
        std::ofstream out( path, std::ios::binary | std::ios::trunc );
        out << text;
    }

    // One scratch directory per test, removed with it, so a test that leaves a file behind cannot make the
    // next one pass.
    class ProbeFiles
    {
    public:
        explicit ProbeFiles( const std::string& name )
             : m_Dir( std::filesystem::temp_directory_path() / ( "DesertSkinProbe_" + name ) )
        {
            std::error_code ec;
            std::filesystem::remove_all( m_Dir, ec );
            std::filesystem::create_directories( m_Dir, ec );
        }

        ~ProbeFiles()
        {
            std::error_code ec;
            std::filesystem::remove_all( m_Dir, ec );
        }

        ProbeFiles( const ProbeFiles& )            = delete;
        ProbeFiles& operator=( const ProbeFiles& ) = delete;

        std::string WriteSkeleton( const char*                       stem = "SkinProbe",
                                   const Common::Content::AssetGuid& guid = kProbeSkeletonGuid ) const
        {
            const auto path = m_Dir / ( std::string( stem ) + ".skeleton" );
            WriteText( path, Desert::Assets::Serialization::WriteSkeletonJson( ProbeSkeletonData( guid ) ) );
            return path.generic_string();
        }

        std::string WriteMesh( const Common::Content::AssetGuid& skeleton, const char* stem = "SkinProbe" ) const
        {
            const auto path = m_Dir / ( std::string( stem ) + ".skmesh" );
            // The mesh is a binary container; the skeleton beside it is still JSON, and that asymmetry
            // is the point of the two lines rather than an oversight — only the MESH form moved.
            WriteText( path, Desert::Assets::Serialization::EncodeMeshBinary( ProbeMeshData( skeleton ) ) );
            return path.generic_string();
        }

    private:
        std::filesystem::path m_Dir;
    };
} // namespace

// THE RELATION ITSELF. A dependency that only becomes nameable after the file is parsed must be bound once
// the file is parsed — through the deferred route, which is the one the whole project actually uses.
TEST( SkinnedMeshDependency, AShellBindsItsSkeletonOnceTheDeferredLoadRevealsTheReference )
{
    const ProbeFiles files( "deferred" );
    AssetManager     manager;

    const auto skeletonPath = files.WriteSkeleton();
    const auto meshPath     = files.WriteMesh( kProbeSkeletonGuid );

    auto skeleton = manager.CreateAsset<SkeletonAsset>( skeletonPath );
    ASSERT_TRUE( skeleton );
    ASSERT_EQ( skeleton->GetMetadata().Handle, Common::AssetHandle( static_cast<uint64_t>(
                                                    Common::Content::HandleForGuid( kProbeSkeletonGuid ) ) ) )
         << "the rig's handle is not HandleForGuid of its header GUID, so no reference by GUID can find it";

    // Exactly what AssetPreloader does: register the mesh WITHOUT parsing it.
    auto mesh = manager.CreateAsset<SkinnedMeshAsset>( meshPath, /*loadAfterCreate=*/false );
    ASSERT_TRUE( mesh );

    // Before the parse the mesh cannot know which rig it wants, so it must be bound to none. This half of
    // the assertion is what makes the other half mean something: an implementation that bound every mesh to
    // the first skeleton it saw would pass the "after" check alone.
    EXPECT_TRUE( mesh->GetSkeleton().IsNull() );
    EXPECT_FALSE( mesh->GetSkeletonDependency().IsValid() );

    ASSERT_TRUE( mesh->EnsureLoaded( manager ).IsSuccess() );

    EXPECT_EQ( mesh->GetSkeleton(), kProbeSkeletonGuid );
    ASSERT_TRUE( mesh->GetSkeletonDependency().IsValid() );
    EXPECT_EQ( mesh->GetSkeletonDependency().Get(), skeleton.get() );
}

// THE TWO ROUTES MUST AGREE. Eager and deferred are two implementations of "this mesh is loaded", and the
// defect was that only one of them produced a usable asset. Comparing them to each other rather than each
// to a hand-written expectation is what catches the next divergence.
TEST( SkinnedMeshDependency, TheEagerAndDeferredRoutesReachTheSameBinding )
{
    const ProbeFiles files( "routes" );
    AssetManager     manager;

    auto skeleton = manager.CreateAsset<SkeletonAsset>( files.WriteSkeleton() );
    ASSERT_TRUE( skeleton );

    // Two copies of one mesh at two paths, so the manager keeps them as two records.
    const auto eagerPath    = files.WriteMesh( kProbeSkeletonGuid, "Eager" );
    const auto deferredPath = files.WriteMesh( kProbeSkeletonGuid, "Deferred" );

    auto eager = manager.CreateAsset<SkinnedMeshAsset>( eagerPath, /*loadAfterCreate=*/true );
    ASSERT_TRUE( eager );

    auto deferred = manager.CreateAsset<SkinnedMeshAsset>( deferredPath, /*loadAfterCreate=*/false );
    ASSERT_TRUE( deferred );
    ASSERT_TRUE( deferred->EnsureLoaded( manager ).IsSuccess() );

    EXPECT_EQ( eager->IsReadyForUse(), deferred->IsReadyForUse() );
    EXPECT_EQ( eager->GetSkeleton(), deferred->GetSkeleton() );
    EXPECT_EQ( eager->GetSkeletonDependency().IsValid(), deferred->GetSkeletonDependency().IsValid() );
    EXPECT_EQ( eager->GetSkeletonDependency().Get(), deferred->GetSkeletonDependency().Get() );
    EXPECT_EQ( eager->GetSkeletonDependency().Get(), skeleton.get() );
}

// A NULL REFERENCE IS "NOT KNOWN YET", NEVER "MATCHES ANYTHING". An unparsed shell names no skeleton; with a
// rig registered, resolving it must bind nothing rather than the one rig in sight.
TEST( SkinnedMeshDependency, AnUnparsedShellBindsNoRigEvenWithOneRegistered )
{
    const ProbeFiles files( "zero" );
    AssetManager     manager;
    ASSERT_TRUE( manager.CreateAsset<SkeletonAsset>( files.WriteSkeleton() ) );

    auto shell = manager.CreateAsset<SkinnedMeshAsset>( files.WriteMesh( kProbeSkeletonGuid ),
                                                        /*loadAfterCreate=*/false );
    ASSERT_TRUE( shell );
    ASSERT_TRUE( shell->GetSkeleton().IsNull() );

    shell->ResolveDependencies( manager );
    EXPECT_FALSE( shell->GetSkeletonDependency().IsValid() );
}

// THE BONES ARE NOT THE IDENTITY (SKEL-TREE). A rig with EXACTLY the probe's bones (the same signature) under
// another GUID is another skeleton: a mesh naming the probe rig must not bind to it, and a mesh naming it
// binds to it. Under the signature rule both meshes bound to whichever rig was found first.
TEST( SkinnedMeshDependency, ARigWithTheSameBonesUnderAnotherGuidIsAnotherSkeleton )
{
    const ProbeFiles                 files( "twin" );
    AssetManager                     manager;
    const Common::Content::AssetGuid twinGuid{ 0x7717717717717717ull, 0x0000000000000001ull };

    auto twin = manager.CreateAsset<SkeletonAsset>( files.WriteSkeleton( "Twin", twinGuid ) );
    ASSERT_TRUE( twin );
    ASSERT_TRUE( twin->EnsureLoaded( manager ).IsSuccess() );
    ASSERT_EQ( twin->GetSignature(), kProbeSignature ) << "the twin must share the probe's bones exactly";

    // Only the twin is registered. The mesh naming the probe rig binds nothing.
    auto probeMesh = manager.CreateAsset<SkinnedMeshAsset>( files.WriteMesh( kProbeSkeletonGuid, "NamesProbe" ),
                                                            /*loadAfterCreate=*/false );
    ASSERT_TRUE( probeMesh );
    ASSERT_TRUE( probeMesh->EnsureLoaded( manager ).IsSuccess() );
    EXPECT_FALSE( probeMesh->GetSkeletonDependency().IsValid() )
         << "a mesh bound to a skeleton it does not name, because the bones matched";

    auto twinMesh = manager.CreateAsset<SkinnedMeshAsset>( files.WriteMesh( twinGuid, "NamesTwin" ),
                                                           /*loadAfterCreate=*/false );
    ASSERT_TRUE( twinMesh );
    ASSERT_TRUE( twinMesh->EnsureLoaded( manager ).IsSuccess() );
    ASSERT_TRUE( twinMesh->GetSkeletonDependency().IsValid() );
    EXPECT_EQ( twinMesh->GetSkeletonDependency().Get(), twin.get() );
}

// RE-RESOLVING IS NOT ADDITIVE. The second call must state the whole answer, including "no rig", or a
// binding survives the disappearance of the thing it points at.
TEST( SkinnedMeshDependency, ResolvingAgainstAManagerWithoutTheRigDropsTheBinding )
{
    const ProbeFiles files( "drop" );

    AssetManager withRig;
    ASSERT_TRUE( withRig.CreateAsset<SkeletonAsset>( files.WriteSkeleton() ) );

    auto mesh = withRig.CreateAsset<SkinnedMeshAsset>( files.WriteMesh( kProbeSkeletonGuid ),
                                                       /*loadAfterCreate=*/false );
    ASSERT_TRUE( mesh );
    ASSERT_TRUE( mesh->EnsureLoaded( withRig ).IsSuccess() );
    ASSERT_TRUE( mesh->GetSkeletonDependency().IsValid() );

    AssetManager withoutRig;
    mesh->ResolveDependencies( withoutRig );
    EXPECT_FALSE( mesh->GetSkeletonDependency().IsValid() );
}

// LOADING HAPPENS ONCE. IsReadyForUse is what every deferred path asks before deciding to parse, and this
// asset never set it — so MeshService::GetAsset re-read and re-parsed the whole .skmesh on every call, i.e.
// per frame. Deleting the file after the first load is how the second load makes itself visible.
TEST( SkinnedMeshDependency, TheDeferredLoadIsNotRepeatedOnEveryAsk )
{
    const ProbeFiles files( "once" );
    AssetManager     manager;

    ASSERT_TRUE( manager.CreateAsset<SkeletonAsset>( files.WriteSkeleton() ) );

    const auto meshPath = files.WriteMesh( kProbeSkeletonGuid );
    auto       mesh     = manager.CreateAsset<SkinnedMeshAsset>( meshPath, /*loadAfterCreate=*/false );
    ASSERT_TRUE( mesh );
    EXPECT_FALSE( mesh->IsReadyForUse() );

    ASSERT_TRUE( mesh->EnsureLoaded( manager ).IsSuccess() );
    EXPECT_TRUE( mesh->IsReadyForUse() );

    std::error_code ec;
    std::filesystem::remove( meshPath, ec );
    ASSERT_FALSE( ec );

    // A second ask must not touch the file. If it does, the parse fails and the vertices vanish.
    EXPECT_TRUE( mesh->EnsureLoaded( manager ).IsSuccess() );
    EXPECT_EQ( mesh->GetVertices().size(), 3u );
    EXPECT_TRUE( mesh->GetSkeletonDependency().IsValid() );
}

// A RIG'S RESIDENCY IS NOT A RIG'S IDENTITY — the relation asset eviction broke.
//
// MEASURED IN THE EDITOR, 2026-09-16. Open ANIM_RigWitness.desce as the FIRST scene: zero errors, the
// character is there. Open any other scene first and ANIM_RigWitness second: 410 x "MeshFactory: Skeleton
// dependency invalid" in a twelve-second session (A13 counted 3 059 in a longer one), one
// "skeleton sig ... not found among 4 skeletons", and no character. Nothing about the scene changed
// between the two runs — only whether a sweep had run first.
//
// The mechanism, and it is ONE LINE: the first scene's eviction sweep releases every skeleton, because no
// skinned mesh is reachable from a world that has none. `SkeletonAsset::GetSignature()` answered
// `m_Skeleton ? m_Skeleton->GetSignature() : 0`, so a released rig answered 0 — the same 0 an unread one
// answers — and the loop below matched no rig at all. The binding then stayed empty for ever: the evictor
// reaches a rig only through `GetSkeletonDependency().Handle`, which is exactly the thing that failed to
// be filled in, so no later sweep could bring it back either.
//
// The relation this test states is therefore NOT "that scene stops logging". It is that a rig is found by
// WHICH RIG IT IS and not by whether its bones happen to be in memory right now.
TEST( SkinnedMeshDependency, AMeshBindsItsRigWhetherOrNotTheRigsPayloadIsResident )
{
    const ProbeFiles files( "evicted" );
    AssetManager     manager;

    auto skeleton = manager.CreateAsset<SkeletonAsset>( files.WriteSkeleton() );
    ASSERT_TRUE( skeleton );

    auto mesh = manager.CreateAsset<SkinnedMeshAsset>( files.WriteMesh( kProbeSkeletonGuid ),
                                                       /*loadAfterCreate=*/false );
    ASSERT_TRUE( mesh );
    ASSERT_TRUE( mesh->EnsureLoaded( manager ).IsSuccess() );
    ASSERT_TRUE( mesh->GetSkeletonDependency().IsValid() ) << "the precondition failed: the binding was "
                                                              "never made, so this test cannot be about "
                                                              "losing it";

    // WHAT A SCENE CHANGE DOES, spelled as the two calls the sweep makes. Records, handles and registry
    // entries all stay — AssetBase::Unload's contract — so this is exactly the state the editor is in at
    // the moment the witness scene is opened second.
    ASSERT_TRUE( mesh->Unload().IsSuccess() );
    ASSERT_TRUE( skeleton->Unload().IsSuccess() );
    ASSERT_EQ( skeleton->GetSkeleton(), nullptr ) << "the rig's payload was not released, so the state "
                                                     "under test was never reached";

    // The scene comes back and the mesh is asked for again — MeshService::Get's build-on-miss path.
    ASSERT_TRUE( mesh->EnsureLoaded( manager ).IsSuccess() );

    EXPECT_TRUE( mesh->GetSkeletonDependency().IsValid() )
         << "the mesh could not find a rig that is registered, unchanged and named by the very GUID "
            "it carries — only its bones were not resident. This is the defect: after one scene change "
            "every skinned mesh in the project is unbuildable for the rest of the session.";

    ASSERT_NE( mesh->GetSkeletonDependency().Get(), nullptr );
    EXPECT_NE( mesh->GetSkeletonDependency().Get()->GetSkeleton(), nullptr )
         << "the binding is back but the bones are not. This is the line MeshFactory::CreateSkinned reads "
            "to build a SkinnedMesh, and a null here is 'Skeleton runtime object is null' once per frame "
            "— the same defect wearing the next message along.";
}

// THE SHIPPED PROBE'S IDENTITY. Two numbers live in files no compiler reads — the rig signature inside
// SkinProbe.skeleton/.skmesh, and the mesh handle inside the scene that places it. Both are derived, so
// both can be re-derived here and compared against what was written down.
TEST( SkinnedMeshDependency, TheShippedProbeKeepsTheIdentityTheSceneWasSavedWith )
{
    const auto data = ProbeSkeletonData();
    EXPECT_EQ( data.Signature, kProbeSignature )
         << "The one-bone 'Root' rig no longer hashes to the signature SkinProbe.skmesh stores; the shipped "
            "probe mesh would find no skeleton and the scene that places it would render nothing.";

    const std::filesystem::path root = DataRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the checkout";
    std::ifstream in( root / kProbeMeshPath, std::ios::binary );
    std::string   prefix( Common::Content::kMeshBinaryPrefixSize, '\0' );
    in.read( prefix.data(), static_cast<std::streamsize>( prefix.size() ) );
    prefix.resize( static_cast<std::size_t>( in.gcount() ) );

    const auto stated = Common::Content::ReadMeshHeaderGuid( prefix );
    ASSERT_TRUE( stated.has_value() ) << kProbeMeshPath << " states no GUID in its header";
    const Common::Content::AssetGuid expected{ kProbeMeshGuidHi, kProbeMeshGuidLo };
    EXPECT_EQ( stated, expected ) << "SkinProbe.skmesh's GUID changed; MESH_SkinnedProbe.desce stores the old "
                                     "MeshGuid and would resolve to no mesh at all.";
    EXPECT_FALSE( Common::Content::HandleForGuid( expected ) == Common::AssetHandle::Null() )
         << "the probe mesh's GUID folds to the null handle, so the scene's reference would name nothing";

    // THE RELATION: the skeleton the shipped mesh names IS the shipped skeleton's header GUID.
    const auto meshRig = Common::Content::ReadMeshHeaderSkeleton( prefix );
    if ( !meshRig.has_value() )
        FAIL() << kProbeMeshPath << " is not a version 5 mesh";
    EXPECT_EQ( *meshRig, kProbeSkeletonGuid ) << kProbeMeshPath << " names another skeleton";

    const std::ifstream rigIn( root / kProbeSkeletonPath, std::ios::binary );
    std::stringstream   rigText;
    rigText << rigIn.rdbuf();
    const auto rig = Desert::Assets::Serialization::ReadSkeletonJson( rigText.str() );
    ASSERT_TRUE( rig ) << rig.GetError();
    const auto& rigHeader = rig.GetValue().Header;
    if ( !rigHeader.has_value() )
        FAIL() << kProbeSkeletonPath << " states no header";
    const auto rigGuid = Common::Content::AssetGuidFromText( rigHeader->Guid );
    ASSERT_TRUE( rigGuid ) << rigGuid.GetError();
    EXPECT_EQ( rigGuid.GetValue(), kProbeSkeletonGuid )
         << kProbeSkeletonPath << "'s GUID changed; SkinProbe.skmesh names the old one and binds no rig";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
