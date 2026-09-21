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
// skeleton signature is still 0 — the signature is a field inside the .skmesh. Nothing asked a second time
// except the editor's drag-and-drop, so every skinned mesh loaded FROM A SCENE had a null skeleton,
// MeshFactory refused to build it, and the frame contained an invisible character plus one
// "MeshFactory: Skeleton dependency invalid" per frame forever. MeshService::Get even carried a comment
// about not caching the failed build so the mesh "can never recover once the dependency is in place" —
// the recovery was designed and the "once" never arrived.
//
// Why the zero test is here too. SkeletonAsset::GetSignature() returns 0 for a rig whose own file has not
// been read, and an unparsed mesh shell also reports 0. An equality match between those two binds a mesh to
// an arbitrary skeleton and calls it resolved, which is strictly worse than the unresolved state because
// nothing downstream can tell the difference. The guard belongs in the same suite as the relation it
// protects.
//
// Why the identity constants are pinned. The repository ships a hand-authored probe rig + mesh
// (Editor/Cooked/Meshes/SkinProbe.*) and a scene that places it, because before that there was NOT ONE
// skinned mesh in any scene in this project and the whole skinned path was therefore unobservable. The
// scene stores the mesh's handle as a number; the rig is matched by a signature. Both are derived, and
// both are written into files that no compiler reads. Pinning them here is what makes a rename or a
// re-cook fail loudly instead of silently emptying the one scene that covers this path.

#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <gtest/gtest.h>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>

#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Mesh/SkeletonAsset.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Assets/Serialization/Mesh.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>

#include <rflcpp/rfl.hpp>
#include <rflcpp/rfl/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using Desert::Assets::AssetManager;
using Desert::Assets::AssetPriority;
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

    // The handle the shipped scene stores for the shipped probe mesh. Derived from the project-relative
    // cooked path, so it is the same number on every machine — that is the whole reason the derivation is
    // relative (see AssetHandle::StableKeyForPath).
    constexpr const char*   kProbeCookedMeshPath = "Cooked/Meshes/SkinProbe.skmesh";
    constexpr std::uint64_t kProbeMeshHandle     = 16266463617133760712ull;

    Desert::Assets::Serialization::SkeletonAssetData ProbeSkeletonData()
    {
        Desert::Animation::BoneInfo root;
        root.Name               = kProbeBoneName;
        root.OffsetMatrix       = glm::mat4( 1.0f );
        root.LocalBindTransform = glm::mat4( 1.0f );
        root.ParentBoneID       = std::nullopt;

        Desert::Assets::Serialization::SkeletonAssetData data;
        data.Bones     = { root };
        data.Signature = Desert::Animation::Skeleton::ComputeSignature( data.Bones );
        return data;
    }

    // A single triangle bound entirely to bone 0. The geometry is irrelevant to the relation; what matters
    // is that the file carries a SkeletonSignature, because that field is the dependency.
    Desert::Assets::Serialization::MeshAssetData ProbeMeshData( std::uint64_t signature )
    {
        Desert::Assets::Serialization::MeshAssetData data;
        data.IsSkinned         = true;
        data.SkeletonSignature = signature;

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

        std::string WriteSkeleton( const char* stem = "SkinProbe" ) const
        {
            const auto path = m_Dir / ( std::string( stem ) + ".skeleton" );
            WriteText( path, rfl::json::write( ProbeSkeletonData() ) );
            return path.generic_string();
        }

        std::string WriteMesh( std::uint64_t signature, const char* stem = "SkinProbe" ) const
        {
            const auto path = m_Dir / ( std::string( stem ) + ".skmesh" );
            // The mesh is a binary container; the skeleton beside it is still JSON, and that asymmetry
            // is the point of the two lines rather than an oversight — only the MESH form moved.
            WriteText( path, Desert::Assets::Serialization::EncodeMeshBinary( ProbeMeshData( signature ) ) );
            return path.generic_string();
        }

    private:
        std::filesystem::path m_Dir;
    };
} // namespace

// THE RELATION ITSELF. A dependency that only becomes nameable after the file is parsed must be bound once
// the file is parsed — through the deferred route, which is the one the whole project actually uses.
TEST( SkinnedMeshDependency, AShellBindsItsSkeletonOnceTheDeferredLoadRevealsTheSignature )
{
    const ProbeFiles files( "deferred" );
    AssetManager     manager;

    const auto skeletonPath = files.WriteSkeleton();
    const auto meshPath     = files.WriteMesh( kProbeSignature );

    auto skeleton = manager.CreateAsset<SkeletonAsset>( AssetPriority::Low, skeletonPath );
    ASSERT_TRUE( skeleton );
    ASSERT_EQ( skeleton->GetSignature(), kProbeSignature );

    // Exactly what AssetPreloader does: register the mesh WITHOUT parsing it.
    auto mesh = manager.CreateAsset<SkinnedMeshAsset>( AssetPriority::Low, meshPath, /*loadAfterCreate=*/false );
    ASSERT_TRUE( mesh );

    // Before the parse the mesh cannot know which rig it wants, so it must be bound to none. This half of
    // the assertion is what makes the other half mean something: an implementation that bound every mesh to
    // the first skeleton it saw would pass the "after" check alone.
    EXPECT_EQ( mesh->GetSkeletonSignature(), 0ull );
    EXPECT_FALSE( mesh->GetSkeletonDependency().IsValid() );

    ASSERT_TRUE( mesh->EnsureLoaded( manager ).IsSuccess() );

    EXPECT_EQ( mesh->GetSkeletonSignature(), kProbeSignature );
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

    auto skeleton = manager.CreateAsset<SkeletonAsset>( AssetPriority::Low, files.WriteSkeleton() );
    ASSERT_TRUE( skeleton );

    // Two copies of one mesh at two paths, so the manager keeps them as two records.
    const auto eagerPath    = files.WriteMesh( kProbeSignature, "Eager" );
    const auto deferredPath = files.WriteMesh( kProbeSignature, "Deferred" );

    auto eager = manager.CreateAsset<SkinnedMeshAsset>( AssetPriority::Low, eagerPath, /*loadAfterCreate=*/true );
    ASSERT_TRUE( eager );

    auto deferred =
         manager.CreateAsset<SkinnedMeshAsset>( AssetPriority::Low, deferredPath, /*loadAfterCreate=*/false );
    ASSERT_TRUE( deferred );
    ASSERT_TRUE( deferred->EnsureLoaded( manager ).IsSuccess() );

    EXPECT_EQ( eager->IsReadyForUse(), deferred->IsReadyForUse() );
    EXPECT_EQ( eager->GetSkeletonSignature(), deferred->GetSkeletonSignature() );
    EXPECT_EQ( eager->GetSkeletonDependency().IsValid(), deferred->GetSkeletonDependency().IsValid() );
    EXPECT_EQ( eager->GetSkeletonDependency().Get(), deferred->GetSkeletonDependency().Get() );
    EXPECT_EQ( eager->GetSkeletonDependency().Get(), skeleton.get() );
}

// A SIGNATURE OF ZERO IS "NOT KNOWN YET", NEVER "MATCHES ANYTHING". Both sides of the comparison report 0
// before their file is read, so an equality match binds an arbitrary rig and reports success.
TEST( SkinnedMeshDependency, AnUnknownSignatureNeverMatchesAnUnreadSkeleton )
{
    const ProbeFiles files( "zero" );
    AssetManager     manager;

    // A skeleton record that exists but has not been read: its signature is 0, exactly like an unparsed
    // mesh shell's.
    SkeletonAsset unreadSkeleton( AssetPriority::Low, Common::Filepath( files.WriteSkeleton() ) );
    ASSERT_EQ( unreadSkeleton.GetSignature(), 0ull );

    auto shell = manager.CreateAsset<SkinnedMeshAsset>( AssetPriority::Low, files.WriteMesh( kProbeSignature ),
                                                        /*loadAfterCreate=*/false );
    ASSERT_TRUE( shell );
    ASSERT_EQ( shell->GetSkeletonSignature(), 0ull );

    shell->ResolveDependencies( manager );
    EXPECT_FALSE( shell->GetSkeletonDependency().IsValid() );
}

// RE-RESOLVING IS NOT ADDITIVE. The second call must state the whole answer, including "no rig", or a
// binding survives the disappearance of the thing it points at.
TEST( SkinnedMeshDependency, ResolvingAgainstAManagerWithoutTheRigDropsTheBinding )
{
    const ProbeFiles files( "drop" );

    AssetManager withRig;
    ASSERT_TRUE( withRig.CreateAsset<SkeletonAsset>( AssetPriority::Low, files.WriteSkeleton() ) );

    auto mesh = withRig.CreateAsset<SkinnedMeshAsset>( AssetPriority::Low, files.WriteMesh( kProbeSignature ),
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

    ASSERT_TRUE( manager.CreateAsset<SkeletonAsset>( AssetPriority::Low, files.WriteSkeleton() ) );

    const auto meshPath = files.WriteMesh( kProbeSignature );
    auto mesh = manager.CreateAsset<SkinnedMeshAsset>( AssetPriority::Low, meshPath, /*loadAfterCreate=*/false );
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

    auto skeleton = manager.CreateAsset<SkeletonAsset>( AssetPriority::Low, files.WriteSkeleton() );
    ASSERT_TRUE( skeleton );

    auto mesh = manager.CreateAsset<SkinnedMeshAsset>( AssetPriority::Low, files.WriteMesh( kProbeSignature ),
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
         << "the mesh could not find a rig that is registered, unchanged and named by the very signature "
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

    EXPECT_EQ( static_cast<std::uint64_t>( Common::AssetHandle::FromCookedPath( kProbeCookedMeshPath ) ),
               kProbeMeshHandle )
         << "The probe mesh's path-derived handle changed; MESH_SkinnedProbe.desce stores the old number and "
            "would resolve to no mesh at all.";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
