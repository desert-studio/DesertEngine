#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AssetEvents.hpp>

#include <Engine/Animation/Skeleton.hpp>

#include <Common/Content/AssetEnvelope.hpp>

#include <span>
#include <vector>

namespace Desert::Assets
{
    class SkeletonAsset : public AssetBase
    {
        struct ReloadTwinTag; // see the private section

    public:
        SkeletonAsset( const Common::Filepath& filepath );
        // THE RELOAD TWIN (MakeReloadTarget): this asset's identity, no header read - the twin's Load reads
        // the file. Public for make_shared; its tag is private, so no one else can name it.
        SkeletonAsset( const AssetMetadata& identity, ReloadTwinTag );

        Common::BoolResultStr LoadFromFile() override;
        Common::BoolResultStr Unload() override;

        // THE OFF-FRAME RELOAD (AssetBase::MakeReloadTarget / AdoptReloaded): the rig read on a worker into a
        // twin, written here on the main thread AT THIS ADDRESS with the new signature and bind revision -
        // what an in-place Load writes, so the meshes and Animators holding `const Skeleton*` stay valid.
        [[nodiscard]] std::shared_ptr<AssetBase> MakeReloadTarget() const override;
        [[nodiscard]] Common::BoolResultStr      AdoptReloaded( AssetBase& twin ) override;

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

        /// Authoring (Skeleton Editor Details). In memory only; Serialization::SaveSkeletonAsset writes the file.
        void SetPreviewMesh( Common::Content::AssetGuid mesh );
        void SetCompatibleSkeletons( std::vector<Common::Content::AssetGuid> skeletons );

        /// Reference Pose authoring (Skeleton Editor, UE's Skeleton Tree bone transform): one bone's
        /// LocalBindTransform on the loaded rig, in memory - Serialization::SaveSkeletonAsset writes it to the
        /// `.skeleton`. The structure does not move, so GetSignature (the identity a mesh and a clip match on)
        /// stays. False when the rig is not loaded or @p bone is out of range.
        bool SetLocalBindTransform( uint32_t bone, const glm::mat4& localBind );

        /// RENAME BONE (Skeleton Editor; UE Skeleton Editing's Rename Bone): @p bone answers to @p name on the
        /// loaded rig, in memory - Serialization::SaveSkeletonAsset writes it and carries it into every asset of
        /// this skeleton that names the bone (Assets::RenameBonesInSkeletonAssets). Indices do not move, so a skin
        /// and every resolved index stay valid; the rig is rebuilt at the same address (its name lookup and its
        /// signature are the names'), the signature moves (AnimationECSSystem re-resolves on it) and so does the
        /// bind revision. Refused, by reason: the rig is not loaded, @p bone is out of range, @p name is empty or
        /// already another bone's. The same name is a success that changes nothing.
        [[nodiscard]] Common::BoolResultStr RenameBone( uint32_t bone, const std::string& name );

        /// Moves on every write of the rest pose - an authoring edit or a (re)load from the file - so a reader
        /// that decomposed it once (an Animator's bind pose) knows to read it again (Animator::RebindRestPose).
        [[nodiscard]] uint64_t GetBindRevision() const
        {
            return m_BindRevision;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::Skeleton;
        }

    private:
        // Names the twin's constructor; private, so only MakeReloadTarget can call it.
        struct ReloadTwinTag
        {
        };

        std::unique_ptr<Animation::Skeleton> m_Skeleton;

        // The payload's identity, kept across `Unload`. See GetSignature.
        uint64_t m_Signature = 0U;
        // See GetBindRevision.
        uint64_t m_BindRevision = 0U;

        // References, not payload: kept across `Unload` like the signature (the .skeleton states them).
        Common::Content::AssetGuid              m_PreviewMesh;
        std::vector<Common::Content::AssetGuid> m_CompatibleSkeletons;
    };

} // namespace Desert::Assets