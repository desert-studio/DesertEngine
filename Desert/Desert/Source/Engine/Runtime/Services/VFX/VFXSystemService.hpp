#pragma once

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Assets/VFXEffectTypeAsset.hpp>
#include <Engine/Assets/VFXSystemAsset.hpp>

#include <Common/Core/ResultStr.hpp>

#include <memory>
#include <string>
#include <unordered_map>

namespace Desert::Runtime
{
    /**
     * @brief The `.dfx` systems VFX components name (VFX-HOST).
     *
     * WaterWavesService's shape: the first ask creates the asset from its registry row and requests the read
     * through the AsyncAssetLoader; until it lands the system is pending (null). What is handed out is the
     * asset's own immutable snapshot (VFXSystemAsset::Snapshot), so the asset eviction sweep may drop the asset
     * while every component naming the system shares the snapshot.
     *
     * THE ASSET IS THE ONE COPY (VFX-PRESETS2, variant B): the `.dfx` window edits the asset in place
     * (VFXSystemAsset::Edit), a re-read replaces it the same way, and Get hands out the asset's current snapshot
     * - a new pointer, which consumers key what they built by (VFXWorld decides whether the new data only moved
     * values or changed the structure). There is no second, published copy.
     */
    class VFXSystemService
    {
    public:
        using System = std::shared_ptr<const Assets::Serialization::VFXSystemData>;

        void BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets );

        /// The system @p handle names: null while it is being read; an error once it cannot be read.
        Common::ResultStr<System> Get( const Assets::AssetHandle& handle );

        /// The effect type NAMED @p name (VFXSystemData::EffectType: a `.dfxtype` file stem, anywhere under the
        /// VFX content root), read on the first ask. The asset is the manager's own (IsProjectScopedAsset), so
        /// AssetHotReload's re-read is what the caller sees through it. An error names why: no file of that
        /// name, two of them, or a first read that failed.
        Common::ResultStr<Assets::Asset<Assets::VFXEffectTypeAsset>> EffectType( const std::string& name );

        void Clear();

    private:
        void Request( const Assets::AssetHandle& handle );
        void Adopt( const Assets::AssetHandle& handle, const Assets::Asset<Assets::VFXSystemAsset>& asset );
        /// A `.dfxmod` written since the last look (the module document's save, a re-import): every ready system
        /// whose header Dependencies name it is handed out as a NEW pointer, which is what makes the particle world
        /// recompile the emitter (ParticleWorldGpu compares the pointer). One serial compare while nothing was
        /// written.
        void ReissueOnModuleWrites();
        /// An asset whose data was replaced since the last look (an edit in the `.dfx` window, a re-read): its
        /// new snapshot is what Get hands out from now on. One compare while nothing was replaced.
        void FollowAssetEdits();

        // One ready system: the asset snapshot it was taken from, and what is handed out - the same pointer, or
        // a copy of it re-issued after a module it uses was written (the new pointer makes consumers rebuild).
        struct Ready
        {
            System Adopted;
            System Issued;
        };

        std::weak_ptr<Assets::AssetManager>                          m_Assets;
        std::unordered_map<Assets::AssetHandle, Assets::LoadRequest> m_Requests;
        std::unordered_map<Assets::AssetHandle, Ready>               m_Ready;
        std::unordered_map<Assets::AssetHandle, std::string>         m_Failed;
        uint64_t                                                     m_ModuleSerial = 0;
        uint64_t                                                     m_EditSerial   = 0;
    };
} // namespace Desert::Runtime
