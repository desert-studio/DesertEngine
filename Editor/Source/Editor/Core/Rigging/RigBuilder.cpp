#include "RigBuilder.hpp"

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/Animation/BoneInfo.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/Geometry/AutoRig.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>

#include <Common/Core/Logger.hpp>

#include <glm/gtc/matrix_transform.hpp>

namespace Desert::Editor
{
    namespace
    {
        bool                          s_Active = false;
        Common::UUID                  s_Target{ 0 };
        std::vector<RigBuilder::Bone> s_Bones;
        int                           s_Selected         = -1;
        bool                          s_ConvertRequested = false;
    } // namespace

    bool RigBuilder::IsActive()
    {
        return s_Active;
    }

    const Common::UUID& RigBuilder::Target()
    {
        return s_Target;
    }

    const std::vector<RigBuilder::Bone>& RigBuilder::Bones()
    {
        return s_Bones;
    }

    int RigBuilder::SelectedBone()
    {
        return s_Selected;
    }

    void RigBuilder::SelectBone( int index )
    {
        s_Selected = ( index >= 0 && index < static_cast<int>( s_Bones.size() ) ) ? index : -1;
    }

    void RigBuilder::Begin( const Common::UUID& entity, const glm::vec3& seedHead )
    {
        s_Active           = true;
        s_Target           = entity;
        s_ConvertRequested = false;
        s_Bones.clear();
        s_Bones.push_back( { "Root", seedHead, -1 } );
        s_Selected = 0;
    }

    int RigBuilder::AddBone( int parent, const glm::vec3& head )
    {
        if ( parent < -1 || parent >= static_cast<int>( s_Bones.size() ) )
            parent = -1;
        s_Bones.push_back( { "Bone_" + std::to_string( s_Bones.size() ), head, parent } );
        s_Selected = static_cast<int>( s_Bones.size() ) - 1;
        return s_Selected;
    }

    void RigBuilder::DeleteBone( int index )
    {
        if ( index <= 0 || index >= static_cast<int>( s_Bones.size() ) )
            return; // never delete the root (index 0) — a rig needs at least one bone

        // Collect the sub-tree rooted at `index` (children reference parents by index).
        std::vector<bool> remove( s_Bones.size(), false );
        remove[index] = true;
        bool changed  = true;
        while ( changed )
        {
            changed = false;
            for ( size_t i = 0; i < s_Bones.size(); ++i )
            {
                const int p = s_Bones[i].Parent;
                if ( !remove[i] && p >= 0 && remove[p] )
                {
                    remove[i] = true;
                    changed   = true;
                }
            }
        }

        // Compact, remapping surviving parent indices.
        std::vector<int>  oldToNew( s_Bones.size(), -1 );
        std::vector<Bone> kept;
        for ( size_t i = 0; i < s_Bones.size(); ++i )
        {
            if ( remove[i] )
                continue;
            oldToNew[i] = static_cast<int>( kept.size() );
            kept.push_back( s_Bones[i] );
        }
        for ( auto& b : kept )
            if ( b.Parent >= 0 )
                b.Parent = oldToNew[b.Parent];

        s_Bones    = std::move( kept );
        s_Selected = s_Bones.empty() ? -1 : 0;
    }

    void RigBuilder::SetHead( int index, const glm::vec3& head )
    {
        if ( index >= 0 && index < static_cast<int>( s_Bones.size() ) )
            s_Bones[index].Head = head;
    }

    void RigBuilder::Cancel()
    {
        s_Active           = false;
        s_ConvertRequested = false;
        s_Target           = Common::UUID{ 0 };
        s_Bones.clear();
        s_Selected = -1;
    }

    void RigBuilder::RequestConvert()
    {
        if ( s_Active && !s_Bones.empty() )
            s_ConvertRequested = true;
    }

    bool RigBuilder::ProcessPending( ::Desert::Core::Scene& scene, const ::Desert::Assets::AssetManager& assets )
    {
        if ( !s_ConvertRequested )
            return false;
        s_ConvertRequested = false;

        const auto entOpt = scene.FindEntityByID( s_Target );
        if ( !entOpt )
        {
            LOG_WARN( "[RigBuilder] Convert aborted: target entity no longer exists." );
            Cancel();
            return false;
        }
        const ECS::Entity entity = entOpt->get();
        if ( !entity.HasComponent<ECS::StaticMeshComponent>() )
        {
            LOG_WARN( "[RigBuilder] Convert aborted: entity has no StaticMeshComponent." );
            Cancel();
            return false;
        }

        const auto& smc       = entity.GetComponent<ECS::StaticMeshComponent>();
        auto        meshAsset = assets.FindByHandle<Assets::StaticMeshAsset>( smc.MeshHandle );
        if ( !meshAsset )
        {
            LOG_WARN( "[RigBuilder] Convert aborted: only asset-backed static meshes can be rigged (no "
                      "primitive/procedural)." );
            return false; // keep the session so the user can pick an asset mesh
        }

        const std::vector<Vertex>&  vertices  = meshAsset->GetVertices();
        const std::vector<Index>&   indices   = meshAsset->GetIndices();
        const std::vector<Submesh>& submeshes = meshAsset->GetSubmeshes();
        if ( vertices.empty() )
        {
            LOG_WARN( "[RigBuilder] Convert aborted: static mesh has no CPU vertices." );
            return false;
        }

        // Bones for weighting (mesh-local heads) — index-aligned with the built Skeleton below.
        std::vector<Geometry::RigBone> rigBones;
        rigBones.reserve( s_Bones.size() );
        for ( const auto& b : s_Bones )
            rigBones.push_back( { b.Head, b.Parent } );

        // Skeleton: LocalBindTransform = translation from the parent's head to this head, so the parent-chain
        // global equals translate(head). OffsetMatrix (recomputed) = inverse(global) => the bind pose is the
        // identity per bone, i.e. the skinned mesh renders IDENTICALLY to the source static mesh until a bone
        // is posed. Matches the bind convention MeshECSSystem/LightGizmoRenderer expect.
        std::vector<Animation::BoneInfo> boneInfos;
        boneInfos.reserve( s_Bones.size() );
        for ( size_t i = 0; i < s_Bones.size(); ++i )
        {
            const auto&     b          = s_Bones[i];
            const glm::vec3 parentHead = ( b.Parent >= 0 ) ? s_Bones[b.Parent].Head : glm::vec3( 0.0f );

            Animation::BoneInfo info;
            info.Name               = b.Name;
            info.OffsetMatrix       = glm::mat4( 1.0f ); // filled by RecomputeOffsetMatrices()
            info.LocalBindTransform = glm::translate( glm::mat4( 1.0f ), b.Head - parentHead );
            if ( b.Parent >= 0 )
                info.ParentBoneID = static_cast<uint32_t>( b.Parent ); // default-constructed optional = root
            boneInfos.push_back( std::move( info ) );
        }

        auto skeleton = std::make_shared<Animation::Skeleton>( std::move( boneInfos ) );
        skeleton->RecomputeOffsetMatrices();

        const std::vector<SkinnedVertex> skinned = Geometry::AutoSkinVertices( vertices, rigBones );

        auto skinnedMesh = std::make_shared<SkinnedMesh>( skinned, indices, submeshes, skeleton.get(),
                                                          meshAsset->GetVertexStreams() );

        // Swap the component. Materials are left to the default skinned PBR material (a static PBR instance is
        // bound to the wrong vertex/pipeline layout) — the user re-assigns skinned materials afterward.
        entity.RemoveComponent<ECS::StaticMeshComponent>();
        auto& out           = entity.AddComponent<ECS::SkinnedMeshComponent>();
        out.RuntimeSkeleton = skeleton;
        out.RuntimeMesh     = skinnedMesh;

        LOG_INFO( "[RigBuilder] Converted static mesh to skinned: {} bones, {} vertices.", s_Bones.size(),
                  skinned.size() );

        Cancel();
        return true;
    }
} // namespace Desert::Editor
