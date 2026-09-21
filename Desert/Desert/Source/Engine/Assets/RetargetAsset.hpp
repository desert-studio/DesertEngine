#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Mesh/SkeletonAsset.hpp>
#include <Engine/Assets/Serialization/Retarget.hpp>

namespace Desert::Assets
{
    /**
     * @brief A retarget on disk (`.retarget`), in the engine's ONE asset system.
     *
     * The same `AssetBase` / `AssetManager` as a texture, a material or a control rig, so it gets the
     * Content Browser, the drag-and-drop payload, the scene reference and the hot reload for free — and,
     * the part this tier actually needed, an `AssetHandle` a component can hold.
     *
     * A PATH STRING WOULD HAVE BEEN CHEAPER AND IS THE WRONG ANSWER, for `ControlRigAsset`'s reason: this
     * engine has exactly one way for a component to name content, and a second addressing scheme would
     * mean the eviction walk cannot see a retarget (`SceneAssetRoots`), the Details page cannot offer a
     * picker, and a moved file breaks silently instead of being re-resolved.
     *
     * ── IT OWNS THE SOURCE HALF OF THE PAIR, AND THAT IS WHY IT HAS A DEPENDENCY ─────────────────────
     *
     * `.derig` has no dependency: a control rig is a statement about ONE rig, and that rig is whichever
     * entity the component sits on. A retarget is a statement about TWO, and the source one is named by
     * nothing else in the scene — the entity carries only its own. So the file names it and this class
     * resolves it, by RELATIVE PATH, the way `CloudTypeAsset` resolves the noise volume its `.decloudtype`
     * names. NOT by signature, although `SkinnedMeshAsset` names its rig that way: see
     * Serialization/Retarget.hpp — a signature cannot tell two proportion variants of one character apart,
     * and those are precisely the pair a retarget exists to bridge.
     *
     * WHAT IT DOES NOT DO: build a `Retargeter`. The caches inside one are resolved against a particular
     * pair of skeletons, and the same retarget is legally used by two entities whose meshes are different
     * exports of the target character. `AnimationECSSystem` builds one per entity, against THAT entity's
     * skeleton — exactly as it builds a `ControlRigStage` per entity.
     */
    class RetargetAsset final : public AssetBase
    {
    public:
        RetargetAsset( AssetPriority priority, const Common::Filepath& filepath );

        /// Reads and parses the file. A file that is missing, malformed, from an unknown format version or
        /// describing a retarget the loader cannot honour is an ERROR carrying the reason — never a
        /// quietly substituted empty retarget, which would attach as a pipeline that cannot change a pose.
        Common::BoolResultStr LoadFromFile() override;
        Common::BoolResultStr Unload() override;

        /// Binds `SourceSkeletonSignature` to the `SkeletonAsset` that answers it. Re-runnable by
        /// construction: the previous answer is dropped first, so a second call after the file is parsed
        /// cannot leave a stale binding behind and cannot be mistaken for the first.
        void ResolveDependencies( AssetManager& manager ) override;

        [[nodiscard]] bool IsReadyForUse() const override
        {
            return m_Ready;
        }

        [[nodiscard]] const Serialization::RetargetAssetData& GetData() const
        {
            return m_Data;
        }

        /// The rig the clips are authored on, or null while the dependency is unresolved. NON-OWNING and
        /// NOT CACHED BY THE CALLER: asset eviction can release the skeleton under it, which is precisely
        /// why `Animation::Retarget::RetargetSource` takes a COPY of the bones rather than this pointer.
        [[nodiscard]] const Animation::Skeleton* GetSourceSkeleton() const
        {
            const auto* asset = m_SourceSkeleton.Get();
            return asset != nullptr ? asset->GetSkeleton() : nullptr;
        }

        /// The source rig's dependency, by HANDLE — which is what the eviction closure marks. Read
        /// through the handle and not through the signature it was matched by, for the reason
        /// `SkinnedMeshAsset`'s does: the signature names a shape, the handle names the file.
        [[nodiscard]] const AssetDependency<SkeletonAsset>& GetSourceSkeletonDependency() const
        {
            return m_SourceSkeleton;
        }

        /// What to show in a slot. The file's `Name` when it has one, the file's stem when it does not.
        [[nodiscard]] const std::string& GetDisplayName() const
        {
            return m_DisplayName;
        }

        /// Bumped by every successful Load. The ECS system holds the revision it last built a retargeter
        /// from, so a hot-reloaded retarget is rebuilt and an unchanged one is not — which is what stops
        /// the per-frame sync from re-resolving every name sixty times a second.
        [[nodiscard]] uint32_t GetRevision() const
        {
            return m_Revision;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::Retarget;
        }

        /// Writes a retarget to disk, creating the directory if needed. Static because saving is what
        /// CREATES an asset: writing through an instance would mean an instance had to exist for a file
        /// that does not.
        static Common::BoolResultStr Save( const Common::Filepath&                 filepath,
                                           const Serialization::RetargetAssetData& data );

    private:
        Serialization::RetargetAssetData m_Data;
        std::string                      m_DisplayName;
        AssetDependency<SkeletonAsset>   m_SourceSkeleton;
        bool                             m_Ready    = false;
        uint32_t                         m_Revision = 0;
    };
} // namespace Desert::Assets
