#pragma once
#include <Engine/Assets/AsyncAssetLoader.hpp>

#include <Engine/Graphic/Materials/Skybox/MaterialSkybox.hpp> //TODO: maybe fix?

namespace Desert::Runtime
{
    class SkyboxService
    {
    public:
        /// DECLARES, NEVER BAKES (AL1-3). The cache read runs on an `AsyncAssetLoader` worker; the upload
        /// (or, on a cache miss, the bake) runs in the completion on the main thread. `Get` answers null
        /// until then, and `IsPending` says that the null is "not here yet" rather than "none". A scene
        /// being opened waits for it through `ContentGate`, which counts the loader's outstanding reads.
        /// A file that is not there is an ERROR naming path and GUID, and nothing is requested.
        void               Request( const std::shared_ptr<Assets::SkyboxAsset>& skyboxAsset );
        [[nodiscard]] bool IsPending( const Assets::AssetHandle& handle ) const;
        std::shared_ptr<Graphic::MaterialSkybox> Get( const Assets::AssetHandle& handle ) const;
        void                                     Clear();

    private:
        std::unordered_map<Assets::AssetHandle, std::shared_ptr<Graphic::MaterialSkybox>> m_Skyboxes;
        std::unordered_map<Assets::AssetHandle, Assets::LoadRequest>                      m_Pending;
    };
} // namespace Desert::Runtime