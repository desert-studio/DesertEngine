#pragma once
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Assets/Skybox/SkyboxAsset.hpp>

#include <Engine/Graphic/Environment/EnvironmentBake.hpp>       // EnvironmentCacheWriter, owned here
#include <Engine/Graphic/Materials/Skybox/MaterialSkybox.hpp> // the type this service caches by handle

namespace Desert::Runtime
{
    class SkyboxService
    {
    public:
        /// The manager a skybox named only by its handle is created in (`Require`).
        void BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets );

        /// A SKYBOX NAMED BY ITS HANDLE, CREATED FROM ITS REGISTRY ROW (AL1-9). No boot stage creates skybox
        /// shells any more: the shell is created unread from `ContentRegistry::RowOf` the first time something
        /// names the handle (a scene, a picker, a material slot), exactly as textures, meshes and the cloud
        /// kinds are, and then `Request`ed. Null when there is no such row or its file is gone; the error names
        /// the handle, and the path and GUID when the row has them.
        [[nodiscard]] Assets::Asset<Assets::SkyboxAsset> Require( const Assets::AssetHandle& handle );

        /// DECLARES, NEVER BAKES (AL1-3). The cache read runs on an `AsyncAssetLoader` worker; the upload
        /// (or, on a cache miss, the bake) runs in the completion on the main thread. `Get` answers null
        /// until then, and `IsPending` says that the null is "not here yet" rather than "none". A scene
        /// being opened waits for it through `ContentGate`, which counts the loader's outstanding reads.
        /// A file that is not there is an ERROR naming path and GUID, and nothing is requested.
        void               Request( const std::shared_ptr<Assets::SkyboxAsset>& skyboxAsset );
        [[nodiscard]] bool IsPending( const Assets::AssetHandle& handle ) const;
        std::shared_ptr<Graphic::MaterialSkybox> Get( const Assets::AssetHandle& handle ) const;
        void                                     Clear();

        /// The skybox conversions' cache writes (EnvironmentManager::WriteCache begins them). Owned by this
        /// service, so it lives exactly as long as the skyboxes it caches; Clear does not touch it. Pumped after
        /// the frame slot and drained first at shutdown by the engine's renderer client (Core/EngineRendererClient).
        [[nodiscard]] Graphic::EnvironmentCacheWriter& EnvironmentCache()
        {
            return m_EnvironmentCache;
        }

    private:
        Graphic::EnvironmentCacheWriter                                                   m_EnvironmentCache;
        std::weak_ptr<Assets::AssetManager>                                               m_Assets;
        std::unordered_map<Assets::AssetHandle, std::shared_ptr<Graphic::MaterialSkybox>> m_Skyboxes;
        std::unordered_map<Assets::AssetHandle, Assets::LoadRequest>                      m_Pending;
    };
} // namespace Desert::Runtime