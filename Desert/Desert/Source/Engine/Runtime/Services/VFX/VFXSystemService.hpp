#pragma once

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
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
     * through the AsyncAssetLoader; until it lands the system is pending (null). The data is copied out once, so
     * the asset eviction sweep may drop the asset while every component naming the system shares the copy. A new
     * copy (a reload) is a new pointer: consumers key what they built from it by that pointer.
     */
    class VFXSystemService
    {
    public:
        using System = std::shared_ptr<const Assets::Serialization::VFXSystemData>;

        void BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets );

        /// The system @p handle names: null while it is being read; an error once it cannot be read.
        Common::ResultStr<System> Get( const Assets::AssetHandle& handle );

        void Clear();

        /// AN EDITOR'S WORKING COPY (UE: the Niagara System editor's transient system, played by its preview
        /// component): @p system becomes what @p handle names - a generated handle that names no file - until
        /// ReleaseWorkingCopy. Every publish is a new pointer, so whatever plays it starts over with the edit.
        /// Refused when @p handle already names a system read from a file. Clear() keeps working copies: their
        /// owner (the open document) releases them.
        Common::BoolResultStr PublishWorkingCopy( const Assets::AssetHandle& handle, System system );
        void                  ReleaseWorkingCopy( const Assets::AssetHandle& handle );

    private:
        void Request( const Assets::AssetHandle& handle );
        void Adopt( const Assets::AssetHandle& handle, const Assets::Asset<Assets::VFXSystemAsset>& asset );
        /// A `.dfxmod` written since the last look (the module document's save, a re-import): every ready system
        /// whose header Dependencies name it is handed out as a NEW pointer, which is what makes the particle
        /// world recompile the emitter (ParticleWorldGpu compares the pointer). One serial compare while nothing
        /// was written.
        void ReissueOnModuleWrites();

        std::weak_ptr<Assets::AssetManager>                          m_Assets;
        std::unordered_map<Assets::AssetHandle, Assets::LoadRequest> m_Requests;
        std::unordered_map<Assets::AssetHandle, System>              m_Ready;
        std::unordered_map<Assets::AssetHandle, std::string>         m_Failed;
        uint64_t                                                     m_ModuleSerial = 0;
        std::unordered_map<Assets::AssetHandle, System>              m_WorkingCopies;
    };
} // namespace Desert::Runtime
