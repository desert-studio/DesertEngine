#pragma once

#include "MeshAsset.hpp"

#include <Engine/Geometry/MeshTypes.hpp>
#include <Engine/Assets/Common.hpp>
#include <Engine/Assets/Mesh/SkeletonAsset.hpp>

#include <optional>
#include <vector>

namespace Desert::Assets
{
    class SkinnedMeshAsset final : public MeshAsset
    {
    public:
        SkinnedMeshAsset( const AssetPriority priority, const Common::Filepath& filepath );

        // -------------------------------------------------
        // Asset lifecycle
        // -------------------------------------------------
        Common::BoolResultStr LoadFromFile() override;
        Common::BoolResultStr Unload() override;

        bool IsSkinned() const override
        {
            return true;
        }

        // -------------------------------------------------
        // Accessors (CPU data only)
        // -------------------------------------------------
        const std::vector<SkinnedVertex>& GetVertices() const
        {
            return m_Vertices;
        }

        const std::vector<Index>& GetIndices() const
        {
            return m_Indices;
        }

        virtual const std::vector<Common::UUID>& GetMaterialHandles() const override
        {
            return m_MaterialAssetHandles;
        }

        const std::vector<Submesh>& GetSubmeshes() const override
        {
            return m_Submeshes;
        }

        // Blendshapes (empty when the mesh has none). Deltas are index-aligned with GetVertices().
        const std::vector<MorphTarget>& GetMorphTargets() const override
        {
            return m_MorphTargets;
        }

        const auto& GetSkeletonDependency() const
        {
            return m_SkeletonDependency;
        }

        virtual void ResolveDependencies( AssetManager& manager ) override
        {
            // Re-runnable by construction: the previous answer is dropped first, so a second call after the
            // file is parsed cannot leave a stale binding behind and cannot be mistaken for the first.
            m_SkeletonDependency.Handle = Common::AssetHandle::Null();
            m_SkeletonDependency.Cached.reset();

            // A SIGNATURE OF ZERO MEANS "NOT KNOWN YET", NEVER "MATCHES ANYTHING". The signature is a field
            // inside the .skmesh, so an unparsed shell carries 0 — and SkeletonAsset::GetSignature() also
            // returns 0 for a skeleton whose own file has not been read. Comparing the two would bind this
            // mesh to the first unloaded skeleton in the project and report the dependency as resolved,
            // which is a worse failure than the unresolved one because nothing downstream can detect it.
            // AssetBase::EnsureLoaded runs this again the moment the parse fills the signature in.
            if ( m_SkeletonSignature == 0 )
            {
                return;
            }

            const auto& allSkeletons = manager.FindAllByType<Assets::SkeletonAsset>();
            for ( const auto& [handle, skeleton] : allSkeletons )
            {
                if ( skeleton->GetSignature() != m_SkeletonSignature )
                    continue;

                // THE RIG'S BONES MUST BE RESIDENT BEFORE THIS COUNTS AS RESOLVED, and that is not a
                // nicety: the only thing anyone does with this dependency is
                // `MeshFactory::CreateSkinned`, which reads `GetSkeleton()` and refuses the whole mesh
                // when it is null. Asset eviction releases a rig whenever a scene without a skinned mesh
                // is open (it is reachable only through this very dependency), so "registered but cold"
                // is the ORDINARY state here rather than an edge case — it is what every scene opened
                // after the first one finds.
                if ( const auto loaded = skeleton->EnsureLoaded( manager ); !loaded )
                {
                    LOG_ERROR( "SkinnedMeshAsset '{}': rig sig {} is registered as '{}' but could not be "
                               "read back: {}",
                               m_Metadata.Filepath.string(), m_SkeletonSignature,
                               skeleton->GetMetadata().Filepath.string(), loaded.GetError() );
                    continue;
                }

                // AND THE REMEMBERED NUMBER IS RE-CHECKED AGAINST THE BONES JUST READ. A cold rig answers
                // with the signature of the last payload it held, which is what makes it findable at all;
                // that value may be stale if the `.skeleton` was re-cooked while it was cold. Verifying
                // here is what keeps the remembered signature a HINT THAT STARTS A LOOKUP rather than a
                // fact that completes one — binding a mesh to a rig it no longer matches is the failure
                // this file's zero-guard was written to prevent, arriving from the other direction.
                if ( skeleton->GetSignature() != m_SkeletonSignature )
                {
                    LOG_WARN( "SkinnedMeshAsset '{}': rig '{}' was remembered as sig {} and reads back as "
                              "{} — it has been re-cooked. Not bound.",
                              m_Metadata.Filepath.string(), skeleton->GetMetadata().Filepath.string(),
                              m_SkeletonSignature, skeleton->GetSignature() );
                    continue;
                }

                m_SkeletonDependency.Handle = handle;
                m_SkeletonDependency.Cached = skeleton;
                break;
            }

            if ( !m_SkeletonDependency.IsValid() )
            {
                LOG_WARN( "SkinnedMeshAsset '{}': skeleton sig {} not found among {} skeletons.",
                          m_Metadata.Filepath.string(), m_SkeletonSignature, allSkeletons.size() );
            }
        }

        // The rig this mesh was cooked against, as stored in the .skmesh. Zero until the file is parsed.
        uint64_t GetSkeletonSignature() const
        {
            return m_SkeletonSignature;
        }

        bool IsReadyForUse() const override
        {
            return m_IsReadyForUse;
        }

    private:
        // Skinning geometry
        std::vector<SkinnedVertex> m_Vertices;
        std::vector<Index>         m_Indices;
        std::vector<Submesh>       m_Submeshes;
        std::vector<MorphTarget>   m_MorphTargets;
        std::vector<Common::UUID>  m_MaterialAssetHandles;

        uint64_t m_SkeletonSignature = 0U;

        // Dependency
        AssetDependency<SkeletonAsset> m_SkeletonDependency;

        bool m_IsReadyForUse = false;
    };
} // namespace Desert::Assets