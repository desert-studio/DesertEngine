// THE TWO RELATIONS ASSET EVICTION IS, AND THE CONTRACT EVERY `Unload()` NOW OBEYS.
//
// The first relation is the whole feature in one sentence: AN ASSET NOTHING REFERENCES IS RELEASED, AND AN
// ASSET SOMETHING REFERENCES IS NOT. Both halves, always, in one test — a sweep that releases everything
// passes "it releases" and a sweep that releases nothing passes "it keeps", and each of those is a real
// defect that the other half is the only thing to catch.
//
// The second is §1.4 applied to the reload: AFTER AN ASSET IS RELEASED, ASKING FOR IT MUST NOT PRODUCE AN
// EMPTY SUCCESSFUL ANSWER. In the asset layer that reduces to a property `AssetBase::EnsureLoaded` rests
// on entirely: a successful `Unload()` must leave `IsReadyForUse()` FALSE. Five of the thirteen
// implementations left it TRUE — TextureAsset, ShaderAsset and SkyboxAsset by omitting the line,
// SkeletonAsset and AnimationAsset by returning a hardcoded `true` from the predicate itself — and the
// consequence is not "the eviction did not work". It is that EnsureLoaded short-circuits on that flag, so
// the asset is emptied AND unreloadable, and every consumer downstream gets a valid object holding
// nothing, for ever.
//
// WHY THE SWEEP IS TESTABLE WITHOUT A DEVICE. The decision is pure — a registry, a root set and a graph
// walk — and the four calls that reach `Runtime::*Service` sit behind `IEvictionSink`. This suite drives
// the real `AssetEviction::Run` against a recording double and asserts the exact set of handles it
// touched. See Engine/Assets/AssetEviction.hpp.

#include <gtest/gtest.h>

#include <Engine/Assets/AssetEviction.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/CloudLayoutAsset.hpp>
#include <Engine/Assets/CloudModellingVolumeAsset.hpp>
#include <Engine/Assets/CloudNoiseVolumeAsset.hpp>
#include <Engine/Assets/CloudTypeAsset.hpp>
#include <Engine/Assets/Mesh/AnimationAsset.hpp>
#include <Engine/Assets/Mesh/SkeletonAsset.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Assets/Shader/ShaderAsset.hpp>
#include <Engine/Assets/Skybox/SkyboxAsset.hpp>
#include <Engine/Assets/TextureAsset.hpp>

#include <Engine/Assets/Serialization/Mesh.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>
#include <Engine/Animation/Skeleton.hpp>

#include <Common/Core/Serialization/GlmReflection.hpp>

#include <Engine/Graphic/ResourceLedger.hpp>

#include <rflcpp/rfl.hpp>
#include <rflcpp/rfl/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace Desert;
using namespace Desert::Assets;

namespace
{
    // ── A RECORDING SINK ─────────────────────────────────────────────────────────────────────────────
    //
    // It answers what a service would and writes down what it was asked to drop. `HasBuiltMaterial` is
    // driven from a set the test fills, so "there is a built material for this asset" is a fact the test
    // states rather than one the double invents.
    class RecordingSink final : public IEvictionSink
    {
    public:
        std::set<uint64_t> BuiltMaterials;
        std::set<uint64_t> BuiltMeshes;

        std::vector<uint64_t> DroppedMaterials;
        std::vector<uint64_t> DroppedMeshes;
        int                   CollectGarbageCalls = 0;

        bool DropBuiltMesh( const Common::AssetHandle& handle ) override
        {
            const uint64_t value = static_cast<uint64_t>( handle );
            if ( BuiltMeshes.erase( value ) == 0 )
                return false;
            DroppedMeshes.push_back( value );
            return true;
        }

        bool HasBuiltMaterial( const Common::AssetHandle& handle ) const override
        {
            return BuiltMaterials.count( static_cast<uint64_t>( handle ) ) != 0;
        }

        void DropBuiltMaterial( const Common::AssetHandle& handle ) override
        {
            const uint64_t value = static_cast<uint64_t>( handle );
            BuiltMaterials.erase( value );
            DroppedMaterials.push_back( value );
        }

        void CollectGarbage() override
        {
            ++CollectGarbageCalls;
        }
    };

    // ── AN ASSET WHOSE LOAD AND UNLOAD ARE OBSERVABLE ────────────────────────────────────────────────
    //
    // A double rather than a real type, because the sweep's DECISION is what is under test here and a
    // real type would drag a file format in with it. The contract it implements is the one every real
    // implementation now obeys, and the per-type suite below holds the real ones to it.
    class ProbeAsset final : public AssetBase
    {
    public:
        ProbeAsset( const AssetPriority priority, const Common::Filepath& filepath )
             : AssetBase( priority, filepath, GetTypeID() )
        {
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::Prefab;
        }

        Common::BoolResultStr Load() override
        {
            m_Ready = true;
            ++LoadCount;
            return BOOLSUCCESS;
        }

        Common::BoolResultStr Unload() override
        {
            if ( !IsReloadableFromFile() )
                return Common::MakeError<bool>( "the probe holds an in-memory payload" );
            m_Ready = false;
            ++UnloadCount;
            return BOOLSUCCESS;
        }

        bool IsReadyForUse() const override
        {
            return m_Ready;
        }

        bool IsReloadableFromFile() const override
        {
            return Reloadable;
        }

        bool Reloadable  = true;
        int  LoadCount   = 0;
        int  UnloadCount = 0;

    private:
        bool m_Ready = false;
    };

    std::shared_ptr<ProbeAsset> Register( AssetManager& manager, const char* path, const bool load )
    {
        auto asset = manager.CreateAsset<ProbeAsset>( AssetPriority::Medium, Common::Filepath( path ), load );
        EXPECT_TRUE( asset ) << "the registry refused " << path;
        return asset;
    }

    // ── A RIG AND A MESH CUT AGAINST IT, ON DISK ─────────────────────────────────────────────────────
    //
    // Real files, for the reason the probe `.demat` above is a real file: the edge under test is the one
    // the LOADER produces. A one-bone rig and a one-triangle mesh, because the relation has nothing to do
    // with either of their sizes.
    std::string WriteProbeRig( const std::filesystem::path& path )
    {
        Desert::Animation::BoneInfo root;
        root.Name               = "Root";
        root.OffsetMatrix       = glm::mat4( 1.0f );
        root.LocalBindTransform = glm::mat4( 1.0f );
        root.ParentBoneID       = std::nullopt;

        Desert::Assets::Serialization::SkeletonAssetData data;
        data.Bones     = { root };
        data.Signature = Desert::Animation::Skeleton::ComputeSignature( data.Bones );

        std::ofstream out( path, std::ios::binary | std::ios::trunc );
        out << rfl::json::write( data );
        return path.generic_string();
    }

    std::string WriteProbeSkinnedMesh( const std::filesystem::path& path, const std::uint64_t signature )
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
        submesh.Name            = "RigProbe";
        submesh.VertexOffset    = 0;
        submesh.VertexCount     = 3;
        submesh.IndexOffset     = 0;
        submesh.IndexCount      = 3;
        submesh.Transform       = glm::mat4( 1.0f );
        submesh.BoundingBox.Min = glm::vec3( 0.0f );
        submesh.BoundingBox.Max = glm::vec3( 2.0f, 0.0f, 0.0f );
        data.Submeshes.push_back( submesh );

        std::ofstream out( path, std::ios::binary | std::ios::trunc );
        out << rfl::json::write( data );
        return path.generic_string();
    }
} // namespace

// ─────────────────────────────────────────────────────────────────────────────────────────────────────
// RELATION 1 — referenced stays, unreferenced goes. BOTH HALVES, ONE TEST.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────

TEST( AssetEviction, AnAssetNothingReferencesIsReleasedAndAReferencedOneIsNot )
{
    AssetManager manager;

    const auto kept    = Register( manager, "probe/kept.deprefab", true );
    const auto dropped = Register( manager, "probe/dropped.deprefab", true );

    ASSERT_TRUE( kept->IsReadyForUse() );
    ASSERT_TRUE( dropped->IsReadyForUse() );

    AssetRootSet roots;
    roots.Mark( kept->GetMetadata().Handle, "the test says something draws it" );

    RecordingSink sink;
    const auto    outcome = AssetEviction::Run( manager, roots, sink );

    // The half that fails if the sweep keeps everything.
    EXPECT_FALSE( dropped->IsReadyForUse() )
         << "an asset no root reaches was not released; the sweep is doing nothing";
    EXPECT_EQ( dropped->UnloadCount, 1 );

    // The half that fails if the sweep releases everything — and that half is the one an over-eager
    // trace breaks, which is the failure mode a person notices as "my mesh disappeared".
    EXPECT_TRUE( kept->IsReadyForUse() ) << "an asset a root names was released; the trace lost its root";
    EXPECT_EQ( kept->UnloadCount, 0 );

    EXPECT_EQ( outcome.Released, 1u );
    EXPECT_EQ( outcome.Reachable, 1u );
    EXPECT_EQ( outcome.Refused, 0u );
    EXPECT_EQ( sink.CollectGarbageCalls, 1 )
         << "the graveyard was never collected, so nothing the sweep dropped was actually destroyed";
}

TEST( AssetEviction, ANotYetLoadedAssetIsCountedColdRatherThanReleased )
{
    // "Nothing to do" and "did something" must be different numbers. A sweep that reports Released for
    // assets that were never loaded reads as working while doing nothing at all — §1.4 applied to a
    // count rather than to a value.
    AssetManager manager;

    const auto shell = Register( manager, "probe/shell.deprefab", false );
    ASSERT_FALSE( shell->IsReadyForUse() );

    RecordingSink sink;
    const auto    outcome = AssetEviction::Run( manager, AssetRootSet{}, sink );

    EXPECT_EQ( outcome.Released, 0u );
    EXPECT_EQ( outcome.AlreadyCold, 1u );
    EXPECT_EQ( shell->UnloadCount, 0 );
}

TEST( AssetEviction, AnInMemoryPayloadIsRefusedByNameRatherThanReleased )
{
    // The third answer, and the one a silent implementation would turn into data loss: an asset that was
    // filled in memory has no file to reload from, so releasing it destroys the only copy. The refusal
    // is COUNTED and CARRIED, not swallowed.
    AssetManager manager;

    const auto captured  = Register( manager, "probe/captured.deprefab", true );
    captured->Reloadable = false;

    RecordingSink sink;
    const auto    outcome = AssetEviction::Run( manager, AssetRootSet{}, sink );

    EXPECT_TRUE( captured->IsReadyForUse() ) << "an unreloadable payload was released";
    EXPECT_EQ( outcome.Refused, 1u );
    EXPECT_EQ( outcome.Released, 0u );
    ASSERT_EQ( outcome.Refusals.size(), 1u );
    EXPECT_FALSE( outcome.Refusals.front().empty() ) << "a refusal with no reason is not a refusal";
}

TEST( AssetEviction, TheBuiltGpuObjectsOfAnUnreachableAssetAreDroppedAndAReachableOnesAreNot )
{
    AssetManager manager;

    const auto kept    = Register( manager, "probe/kept.deprefab", true );
    const auto dropped = Register( manager, "probe/dropped.deprefab", true );

    RecordingSink sink;
    sink.BuiltMaterials = { static_cast<uint64_t>( kept->GetMetadata().Handle ),
                            static_cast<uint64_t>( dropped->GetMetadata().Handle ) };
    sink.BuiltMeshes    = { static_cast<uint64_t>( kept->GetMetadata().Handle ),
                            static_cast<uint64_t>( dropped->GetMetadata().Handle ) };

    AssetRootSet roots;
    roots.Mark( kept->GetMetadata().Handle, "the test says something draws it" );

    const auto outcome = AssetEviction::Run( manager, roots, sink );

    EXPECT_EQ( outcome.MaterialsDropped, 1u );
    EXPECT_EQ( outcome.MeshesDropped, 1u );
    ASSERT_EQ( sink.DroppedMaterials.size(), 1u );
    ASSERT_EQ( sink.DroppedMeshes.size(), 1u );
    EXPECT_EQ( sink.DroppedMaterials.front(), static_cast<uint64_t>( dropped->GetMetadata().Handle ) );
    EXPECT_EQ( sink.DroppedMeshes.front(), static_cast<uint64_t>( dropped->GetMetadata().Handle ) );
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────
// RELATION 2 — a released asset must be RELOADABLE, i.e. it must stop reporting itself ready.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────

TEST( AssetEviction, EveryUnloadLeavesTheAssetNotReadyOrRefusesWithAReason )
{
    // THE CONTRACT, HELD OVER ALL THIRTEEN. Each type is constructed, driven into whatever "ready" state
    // it can reach without a file, then unloaded; afterwards it must report itself NOT ready. That is
    // what `AssetBase::EnsureLoaded` consults, so this is precisely "can the asset come back".
    //
    // A type that cannot become ready without a file still contributes: it must be NOT ready as a fresh
    // shell. Two of the thirteen failed that — SkeletonAsset and AnimationAsset returned a hardcoded
    // `true` — which made them unloadable AND unloadED at the same time: EnsureLoaded never parsed them,
    // so the shell handed out an empty skeleton and an uninitialised rig signature.
    const Common::Filepath path( "probe/asset.bin" );

    const auto check = []( const char* name, AssetBase& asset )
    {
        EXPECT_FALSE( asset.IsReadyForUse() )
             << name
             << " reports itself ready as a freshly constructed shell that has never been loaded. "
                "EnsureLoaded short-circuits on that, so this asset can never be parsed at all.";

        const auto unloaded = asset.Unload();
        if ( !unloaded )
        {
            EXPECT_FALSE( unloaded.GetError().empty() ) << name << " refused Unload with no reason";
            return;
        }

        EXPECT_FALSE( asset.IsReadyForUse() )
             << name
             << " reports itself READY after a successful Unload. The payload is gone and "
                "EnsureLoaded will never re-read it: the asset is emptied and unreloadable.";
    };

    StaticMeshAsset staticMesh( AssetPriority::Medium, path );
    check( "StaticMeshAsset", staticMesh );

    SkinnedMeshAsset skinnedMesh( AssetPriority::Medium, path );
    check( "SkinnedMeshAsset", skinnedMesh );

    SkeletonAsset skeleton( AssetPriority::Medium, path );
    check( "SkeletonAsset", skeleton );

    AnimationAsset animation( AssetPriority::Medium, path );
    check( "AnimationAsset", animation );

    SurfaceMaterialAsset material( AssetPriority::Medium, path );
    check( "SurfaceMaterialAsset", material );

    TextureAsset texture( AssetPriority::Medium, path );
    check( "TextureAsset", texture );

    SkyboxAsset skybox( AssetPriority::Medium, path );
    check( "SkyboxAsset", skybox );

    ShaderAsset shader( AssetPriority::Medium, path );
    check( "ShaderAsset", shader );

    // PrefabAsset is held to the same contract textually, in Desert/Tests/Engine/AssetRoots: constructing
    // one here would link the whole ECS and scene-serializer layer into a suite that exists to run without
    // a world.

    CloudNoiseVolumeAsset noise( AssetPriority::Medium, path );
    check( "CloudNoiseVolumeAsset", noise );

    CloudTypeAsset cloudType( AssetPriority::Medium, path );
    check( "CloudTypeAsset", cloudType );

    CloudModellingVolumeAsset modelling( AssetPriority::Medium, path );
    check( "CloudModellingVolumeAsset", modelling );

    CloudLayoutAsset layout( AssetPriority::Medium, path );
    check( "CloudLayoutAsset", layout );
}

TEST( AssetEviction, AnUnloadedAssetStopsAnsweringWithItsPayload )
{
    // The other half of "no empty successful answer": a released asset must not go on reporting the
    // things it read out of its file. Six of the thirteen bodies only flipped a flag, so every getter
    // kept answering with post-load values on an asset that reported itself not ready — a contradiction
    // nothing downstream is written to expect.
    const Common::Filepath path( "probe/asset.bin" );

    SkinnedMeshAsset skinned( AssetPriority::Medium, path );
    ASSERT_TRUE( skinned.Unload() );
    EXPECT_EQ( skinned.GetSkeletonSignature(), 0U )
         << "an unloaded skinned mesh still claims a rig signature; ResolveDependencies matches rigs on "
            "that number";

    AnimationAsset animation( AssetPriority::Medium, path );
    ASSERT_TRUE( animation.Unload() );
    EXPECT_EQ( animation.GetSkeletonSignature(), 0U );
    EXPECT_TRUE( animation.GetClip().Tracks.empty() );

    CloudNoiseVolumeAsset noise( AssetPriority::Medium, path );
    ASSERT_TRUE( noise.Unload() );
    EXPECT_TRUE( noise.GetVolume().Voxels.empty() );
    EXPECT_FALSE( noise.IsReadyForUse() );
    // `Params` is deliberately NOT asserted to be zero: it is the RECIPE — the seed, periods and
    // resolution a volume would be generated from — not a description of the buffer, and a released
    // asset holding its recipe's defaults is the same state a never-loaded one holds. `Voxels.empty()`
    // and `IsReadyForUse()` are the two facts a caller may act on, and both are now true.

    CloudLayoutAsset layout( AssetPriority::Medium, path );
    ASSERT_TRUE( layout.Unload() );
    EXPECT_EQ( layout.GetLayout().Resolution, 0U );
    EXPECT_EQ( layout.GetLayout().ContentHash, 0U );

    SkeletonAsset skeleton( AssetPriority::Medium, path );
    EXPECT_EQ( skeleton.GetSignature(), 0U )
         << "a rig that has never been read claims a signature. Zero is what 'not known yet' is spelled "
            "as, and SkinnedMeshAsset::ResolveDependencies refuses to match it for that reason.";
    ASSERT_TRUE( skeleton.Unload() );
    EXPECT_EQ( skeleton.GetSkeleton(), nullptr )
         << "SkeletonAsset::Unload used to be `return BOOLSUCCESS` with no semicolon, leaking the "
            "unique_ptr it exclusively owns";
    // THE RIG'S SIGNATURE IS THE CARVE-OUT, and it is stated here rather than inferred, beside the fields
    // that do go. It is what the mesh MATCHES a rig by, so releasing it is releasing the asset's identity
    // — the same thing `AssetBase::Unload`'s contract point 3 forbids for the handle — and the test below
    // (ASkinnedMeshRebindsItsRigAfterASweepHasReleasedBoth) is what that costs when it is not kept. It is
    // asserted over a rig that was actually LOADED, there, because a fresh shell's zero proves nothing.
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────
// THE EDGES — the trace has to reach an asset that only another ASSET names.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────

TEST( AssetEviction, AMaterialsTextureSurvivesBecauseTheMaterialNamesIt )
{
    // A root names the MATERIAL; nothing names the TEXTURE except the material's own file. If Expand
    // drops that edge the texture is released, and the surface it dresses comes back untextured - with no
    // missing file and nothing in the log, because a `.demat` names its assets by number and by nothing
    // else. That is the defect shape AssetReferenceCensus exists for, reached from the eviction side.
    //
    // A REAL `.demat` ON DISK, not a hand-set member: the edge this asserts is the one the LOADER
    // produces, and a test that filled MaterialData directly would pass over a Load() that had stopped
    // reading the Textures array at all.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "desert_asset_eviction";
    std::filesystem::create_directories( dir );

    const std::filesystem::path texturePath  = dir / "probe_texture.tex";
    const std::filesystem::path materialPath = dir / "probe_material.demat";

    AssetManager manager;

    auto texture =
         manager.CreateAsset<TextureAsset>( AssetPriority::Medium, Common::Filepath( texturePath ), false );
    ASSERT_TRUE( texture );
    const uint64_t textureHandle = static_cast<uint64_t>( texture->GetMetadata().Handle );

    {
        std::ofstream out( materialPath, std::ios::binary | std::ios::trunc );
        ASSERT_TRUE( out.is_open() );
        out << R"({"Params":[],"Textures":[{"Name":"u_AlbedoTexture","TextureHandle":)" << textureHandle
            << R"(}]})";
    }

    auto material = manager.CreateAsset<SurfaceMaterialAsset>( AssetPriority::Medium,
                                                               Common::Filepath( materialPath ), true );
    ASSERT_TRUE( material );
    ASSERT_TRUE( material->IsReadyForUse() ) << "the probe material did not load; the edge cannot be tested";
    ASSERT_EQ( material->Data().Textures.size(), 1u );

    AssetRootSet roots;
    roots.Mark( material->GetMetadata().Handle, "the test says a mesh slot names it" );

    RecordingSink sink;
    const auto    outcome = AssetEviction::Run( manager, roots, sink );

    EXPECT_EQ( outcome.Reachable, 2u ) << "the material -> texture edge was not followed: the trace reached "
                                       << outcome.Reachable << " asset(s) where the material alone names one more";
    EXPECT_FALSE( roots.Contains( texture->GetMetadata().Handle ) )
         << "the ROOT set was mutated; the closure must be a copy so the caller's roots stay its own";

    std::filesystem::remove( materialPath );
}

// THE SWEEP MUST BE SURVIVABLE, NOT MERELY CORRECT — the half this suite did not state.
//
// Every other test here asks what the sweep RELEASES. This one asks what happens NEXT, which is the half
// the whole design rests on: "eviction drops the payload and keeps the shell, so the next Get rebuilds it"
// (AssetEviction.hpp). For a skinned mesh that promise was false, and it was false for a whole class of
// scene rather than for one file — measured in the editor 2026-09-16, ANIM_RigWitness.desce opened second
// logged 410 x "MeshFactory: Skeleton dependency invalid" in twelve seconds and drew no character, while
// the same scene opened FIRST logged none.
//
// The rebuild went through `SkinnedMeshAsset::ResolveDependencies`, which matched a rig by
// `SkeletonAsset::GetSignature()` — and that answered 0 for a rig whose bones this sweep had just
// released, which is the same 0 that means "never read". So the mesh bound no rig, and because the
// evictor reaches a rig ONLY through `GetSkeletonDependency().Handle`, no later sweep could mark it
// either: the graph edge stayed cut for the rest of the session.
TEST( AssetEviction, ASkinnedMeshRebindsItsRigAfterASweepHasReleasedBoth )
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "desert_asset_eviction_rig";
    std::filesystem::create_directories( dir );

    AssetManager manager;

    auto skeleton = manager.CreateAsset<SkeletonAsset>(
         AssetPriority::Medium, Common::Filepath( WriteProbeRig( dir / "probe.skeleton" ) ) );
    ASSERT_TRUE( skeleton );
    const std::uint64_t signature = skeleton->GetSignature();
    ASSERT_NE( signature, 0U ) << "the probe rig did not load; the relation cannot be tested";

    auto mesh = manager.CreateAsset<SkinnedMeshAsset>(
         AssetPriority::Medium, Common::Filepath( WriteProbeSkinnedMesh( dir / "probe.skmesh", signature ) ),
         /*loadAfterCreate=*/false );
    ASSERT_TRUE( mesh );
    ASSERT_TRUE( mesh->EnsureLoaded( manager ).IsSuccess() );
    ASSERT_TRUE( mesh->GetSkeletonDependency().IsValid() ) << "the precondition failed: nothing was bound "
                                                              "before the sweep, so nothing could be lost";

    // A SCENE THAT NAMES NEITHER — the ordinary case, since most scenes hold no skinned mesh at all.
    RecordingSink sink;
    const auto    swept = AssetEviction::Run( manager, AssetRootSet{}, sink );
    ASSERT_EQ( swept.Released, 2U ) << "the sweep did not release both; the state under test was not reached";
    ASSERT_EQ( skeleton->GetSkeleton(), nullptr );
    EXPECT_EQ( skeleton->GetSignature(), signature )
         << "the released rig forgot WHICH rig it is. Its bones are gone and must be; its identity is what "
            "every mesh cooked against it names it by, and a registry record that cannot say what it is "
            "cannot be found again by anything.";

    // THE SCENE COMES BACK. This is MeshService::Get's build-on-miss, and everything it needs is in the
    // registry: the same two files, the same two handles, the same signature inside the mesh.
    ASSERT_TRUE( mesh->EnsureLoaded( manager ).IsSuccess() );

    EXPECT_TRUE( mesh->GetSkeletonDependency().IsValid() )
         << "a rig that the sweep RELEASED could not be found again, so the sweep is not survivable: the "
            "mesh is unbuildable for the rest of the session and every frame says so once.";
    ASSERT_NE( mesh->GetSkeletonDependency().Get(), nullptr );
    EXPECT_NE( mesh->GetSkeletonDependency().Get()->GetSkeleton(), nullptr )
         << "the binding came back without the bones behind it — MeshFactory::CreateSkinned reads exactly "
            "this pointer.";

    // AND THE EDGE IS WHOLE AGAIN, which is the half that makes the recovery stick. `Expand` reaches a rig
    // only through the mesh's dependency HANDLE, so a mesh that rebound to a rig it cannot name would be
    // swept into the same hole by the very next scene change.
    AssetRootSet roots;
    roots.Mark( mesh->GetMetadata().Handle, "the test says a SkinnedMeshComponent draws it" );
    const auto second = AssetEviction::Run( manager, roots, sink );
    EXPECT_EQ( second.Reachable, 2U )
         << "the mesh -> rig edge was not followed after the rebind: the trace reached " << second.Reachable
         << " asset(s) where the mesh alone names one more";
    EXPECT_NE( skeleton->GetSkeleton(), nullptr )
         << "the rig was released again while the mesh that is drawing with it is reachable";

    std::error_code ec;
    std::filesystem::remove_all( dir, ec );
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────
// THE LEDGER — the ownership registry the sweep reports against.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────

TEST( ResourceLedger, ARowLivesExactlyAsLongAsItsToken )
{
    using namespace Desert::Graphic;

    const uint32_t before = ResourceLedger::Take().Live;
    {
        ResourceOwnership row = ResourceOwnership::Take( ResourceKind::Image2D, 1024 );
        EXPECT_TRUE( row.IsAccounted() );
        EXPECT_EQ( ResourceLedger::Take().Live, before + 1 );
    }
    EXPECT_EQ( ResourceLedger::Take().Live, before )
         << "a row outlived the object it accounts for: the ledger now reports resources that are gone, "
            "and nothing can tell that it does";
}

TEST( ResourceLedger, AMovedTokenLeavesExactlyOneRow )
{
    using namespace Desert::Graphic;

    const uint32_t before = ResourceLedger::Take().Live;
    {
        ResourceOwnership first  = ResourceOwnership::Take( ResourceKind::VertexBuffer );
        ResourceOwnership second = std::move( first );

        EXPECT_FALSE( first.IsAccounted() ) << "the moved-FROM token still holds a row; it will close it "
                                               "when it dies and the survivor's row will be gone";
        EXPECT_TRUE( second.IsAccounted() );
        EXPECT_EQ( ResourceLedger::Take().Live, before + 1 );
    }
    EXPECT_EQ( ResourceLedger::Take().Live, before );
}

TEST( ResourceLedger, AnUnclaimedRowIsReportedAsUnclaimedRatherThanAsSomebodysDefault )
{
    // §1.4 applied to a census. "Nobody owns this" is the measurement Г7-A plans against, and a default
    // bucket would make the ledger read complete while covering half the device.
    using namespace Desert::Graphic;

    const ResourceCensus before = ResourceLedger::Take();
    {
        ResourceOwnership anonymous = ResourceOwnership::Take( ResourceKind::Framebuffer );
        ResourceOwnership owned     = ResourceOwnership::Take( ResourceKind::Framebuffer );
        owned.Claim( ResourceOwner::SceneRenderer );

        const ResourceCensus during = ResourceLedger::Take();
        EXPECT_EQ( during.Unclaimed, before.Unclaimed + 1 );
        EXPECT_EQ( during.PerOwner[static_cast<std::size_t>( ResourceOwner::SceneRenderer )],
                   before.PerOwner[static_cast<std::size_t>( ResourceOwner::SceneRenderer )] + 1 );
    }
}

TEST( ResourceLedger, AnAmbientScopeAttributesWhatIsBuiltInsideItAndAnExplicitClaimStillWins )
{
    using namespace Desert::Graphic;

    const ResourceCensus before = ResourceLedger::Take();
    {
        const ResourceAttributionScope scope( ResourceOwner::SceneRenderer );

        ResourceOwnership ambient = ResourceOwnership::Take( ResourceKind::GraphicsPipeline );
        EXPECT_EQ( ambient.GetOwner(), ResourceOwner::SceneRenderer );

        ResourceOwnership explicitly = ResourceOwnership::Take( ResourceKind::GraphicsPipeline );
        explicitly.Claim( ResourceOwner::AssetService, Common::AssetHandle( 42 ) );
        EXPECT_EQ( explicitly.GetOwner(), ResourceOwner::AssetService )
             << "the ambient scope overrode a claim; naming the asset behind an object is the more "
                "specific truth and must win";
        EXPECT_EQ( static_cast<uint64_t>( explicitly.GetAsset() ), 42U );
    }

    ResourceOwnership afterScope = ResourceOwnership::Take( ResourceKind::GraphicsPipeline );
    EXPECT_EQ( afterScope.GetOwner(), ResourceOwner::Unclaimed )
         << "the scope did not restore the previous attribution when it went out of scope";

    EXPECT_EQ( ResourceLedger::Take().Live, before.Live + 1 );
}

TEST( ResourceLedger, EveryKindAndEveryOwnerHasAName )
{
    // The same guard AssetTypeName carries: a name table that falls behind its enum puts a wrong word in
    // the one message whose job is to be trusted.
    using namespace Desert::Graphic;

    for ( std::size_t kind = 0; kind < static_cast<std::size_t>( ResourceKind::Count ); ++kind )
    {
        const char* name = ResourceKindName( static_cast<ResourceKind>( kind ) );
        EXPECT_STRNE( name, "Unknown" ) << "resource kind " << kind << " has no name";
    }

    for ( std::size_t owner = 0; owner < static_cast<std::size_t>( ResourceOwner::Count ); ++owner )
    {
        const char* name = ResourceOwnerName( static_cast<ResourceOwner>( owner ) );
        EXPECT_STRNE( name, "Unknown" ) << "resource owner " << owner << " has no name";
    }
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
