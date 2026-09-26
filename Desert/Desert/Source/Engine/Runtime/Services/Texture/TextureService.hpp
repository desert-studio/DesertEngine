#pragma once

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AssetRef.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Assets/TextureAsset.hpp>
#include <Engine/Graphic/Texture.hpp>
#include <Engine/Runtime/Services/Texture/TextureWaiters.hpp>

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Desert::Runtime
{
    /**
     * Textures ON DEMAND (AL1-4). Nothing registers every texture at boot any more: a handle nobody
     * announced is discovered from its content-registry row the first time it is asked for, its `.detex`
     * metadata is read through `AsyncAssetLoader`, and the GPU texture is built in the completion, on the
     * main thread. Until then the answer is Pending, and a material that bound a slot's default meanwhile
     * is rebuilt when the texture lands (`RebuildWhenReady`).
     */
    class TextureService
    {
    public:
        /// The manager discovery creates shells in. Bound by ResourceRegistry::BindOnDemandAssets.
        void BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets );

        // Eager: build the GPU texture now, for an asset the caller has just made (import, UI render target).
        void Register( const std::shared_ptr<Assets::TextureAsset>& texture );

        // Seed a shell the caller already holds (a freshly imported texture that is not in the registry yet).
        void RegisterAsset( const std::shared_ptr<Assets::TextureAsset>& texture );

        /// Ready, Pending (announced or discovered and being read) or Null (not in the project, or failed).
        /// Starts the read on the first ask; never blocks on the file.
        Assets::AssetRef<Graphic::Texture2D> Require( const Assets::AssetHandle& handle ) const;

        /// `Require` reduced to a pointer: null while Pending as well as when absent.
        Graphic::Texture2D* Get( const Assets::AssetHandle& handle ) const;

        /// A material that bound a slot default because @p texture was Pending; it is invalidated (rebuilt
        /// on its next draw) when the texture lands.
        void RebuildWhenReady( const Assets::AssetHandle& texture, const Assets::AssetHandle& material ) const;

        // Original source file of a texture (e.g. ".../foo.gif"), empty when the handle is unknown. The one
        // synchronous door: an unread shell is finished NOW through AsyncAssetLoader::FlushOne, which the
        // SyncLoadLedger counts.
        std::string GetSourcePath( const Assets::AssetHandle& handle ) const;

        void Clear();

    private:
        struct Entry
        {
            std::shared_ptr<Assets::TextureAsset> Source;
            std::shared_ptr<Graphic::Texture2D>   Built;
            Assets::LoadRequest                   Request;
            /// Latched so a corrupt file is not re-read every frame.
            bool Failed = false;
        };

        Entry* FindOrDiscover( const Assets::AssetHandle& handle ) const;
        void   BeginRead( const Assets::AssetHandle& handle, Entry& entry ) const;
        void   Build( const Assets::AssetHandle& handle, Entry& entry ) const;

        // Mutable: `Get` is const for its 25 callers and discovery on a miss is a cache fill, not a change
        // of what the service answers.
        mutable std::unordered_map<Assets::AssetHandle, Entry> m_Entries;
        mutable std::unordered_set<Assets::AssetHandle>        m_ReportedMissing;
        std::weak_ptr<Assets::AssetManager>                    m_Assets;
        /// Materials that drew a slot default while a texture was Pending.
        mutable TextureWaiters m_Waiters;
    };
} // namespace Desert::Runtime
