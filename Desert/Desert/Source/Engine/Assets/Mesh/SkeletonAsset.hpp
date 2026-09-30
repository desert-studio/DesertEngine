#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AssetEvents.hpp>

#include <Engine/Animation/Skeleton.hpp>

#include <Common/Content/AssetEnvelope.hpp>

#include <span>

namespace Desert::Assets
{
    class SkeletonAsset : public AssetBase
    {
    public:
        SkeletonAsset( const Common::Filepath& filepath );

        Common::BoolResultStr LoadFromFile() override;
        Common::BoolResultStr Unload() override;

        // WAS A HARDCODED `return true`, WHICH MADE THIS TYPE UNLOADABLE AND UNLOADED AT ONCE.
        //
        // `AssetBase::EnsureLoaded` opens with `if ( IsReadyForUse() ) return BOOLSUCCESS;`, so a constant
        // true meant a skeleton shell registered with `loadAfterCreate = false` could NEVER be parsed:
        // `m_Skeleton` stayed null, `GetSkeleton()` answered nullptr and `GetSignature()` answered 0. That
        // zero is the number `SkinnedMeshAsset::ResolveDependencies` matches rigs on, so an unloaded
        // skeleton silently failed to match the mesh that names it — the same never-recovers shape that
        // file's own comment warns about, from the other side.
        //
        // The skeleton IS the readiness, so there is no flag to keep in step with it.
        virtual bool IsReadyForUse() const override
        {
            return m_Skeleton != nullptr;
        }

        const Animation::Skeleton* GetSkeleton() const
        {
            return m_Skeleton.get();
        }

        // WHICH RIG THIS IS — AND THAT OUTLIVES THE BONES, which is the whole of the fix that put this
        // field here.
        //
        // It used to read `m_Skeleton ? m_Skeleton->GetSignature() : 0`, so a rig whose payload asset
        // eviction had released answered 0 — indistinguishable from a rig whose file has never been read.
        // `SkinnedMeshAsset::ResolveDependencies` matches on this number, so after the first eviction
        // sweep of a session NO skinned mesh in the project could find its rig again, and because the
        // sweep reaches a rig only through the dependency handle that failed to be filled in, nothing
        // could ever bring it back. Measured 2026-09-16: ANIM_RigWitness.desce opened SECOND logged 410 x
        // "MeshFactory: Skeleton dependency invalid" in twelve seconds and drew no character; opened
        // FIRST, none.
        //
        // A signature is a hash of the bone structure, so it is derived from the payload — but what it is
        // USED as is an identity, exactly like the path-derived handle that `AssetBase::Unload` is
        // required to keep (contract point 3). Keeping it is the same statement as keeping the handle: an
        // evicted asset keeps its identity or the reload is a different asset. The value can only ever
        // START a lookup — the one caller re-checks it against the payload it just loaded, so a `.skeleton`
        // edited while it was cold cannot bind a mesh to a rig it no longer matches.
        //
        // Zero still means "never read", and still never matches: this field is written only by `Load`.
        uint64_t GetSignature() const
        {
            return m_Signature;
        }

        /// The skinned mesh the Skeleton Editor previews this rig on (UE USkeleton::PreviewSkeletalMesh). Null =
        /// bones only. Contract: Engine/Animation/SkeletonReference.hpp.
        [[nodiscard]] Common::Content::AssetGuid GetPreviewMesh() const;

        /// Skeletons whose clips play on meshes of THIS skeleton (UE USkeleton::CompatibleSkeletons): one
        /// direction, not transitive. The third argument of Animation::ClipPlaysOnMesh.
        [[nodiscard]] std::span<const Common::Content::AssetGuid> GetCompatibleSkeletons() const;

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::Skeleton;
        }

    private:
        std::unique_ptr<Animation::Skeleton> m_Skeleton;

        // The payload's identity, kept across `Unload`. See GetSignature.
        uint64_t m_Signature = 0U;
    };

} // namespace Desert::Assets