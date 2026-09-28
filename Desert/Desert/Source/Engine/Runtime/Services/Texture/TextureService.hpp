#pragma once

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AssetRef.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Assets/FrameRetireQueue.hpp>
#include <Engine/Assets/TextureAsset.hpp>
#include <Engine/Graphic/Texture.hpp>
#include <Engine/Runtime/Services/Texture/TextureUploadQueue.hpp>
#include <Engine/Runtime/Services/Texture/TextureWaiters.hpp>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Desert::Runtime
{
    /// How much texture data one frame may copy to the GPU (AM2). A code default like WorldPartition's
    /// ResidencySettings, read by `TextureService::PumpUploads` every frame.
    struct TextureUploadSettings
    {
        /// 8 MiB: one 1k RGBA8 texture with its mips (5.6 MB) or one 2k BC7 (5.6 MB) per frame, or a dozen
        /// 512 BC textures. A frame always uploads at least one waiting texture, whatever its size.
        uint64_t BytesPerFrame = 8ull * 1024ull * 1024ull;
    };

    /// What a cook worker hands the frame loop: the decoded texture, or why there is none.
    struct TextureCookOutcome
    {
        std::optional<Graphic::CookedTexture2D> Cooked;
        std::string                             Error;
    };

    using TextureUploads = TextureUploadQueue<Assets::AssetHandle, TextureCookOutcome>;

    /**
     * Textures ON DEMAND (AL1-4). Nothing registers every texture at boot any more: a handle nobody
     * announced is discovered from its content-registry row the first time it is asked for, its `.detex`
     * metadata is read through `AsyncAssetLoader`, and the GPU texture is built in the completion, on the
     * main thread. Until then the answer is Pending, and a material that bound a slot's default meanwhile
     * is rebuilt when the texture lands (`RebuildWhenReady`).
     *
     * THE COOK AND THE UPLOAD ARE NOT ON THE READ'S COMPLETION (AM2). The completion hands the platform-data
     * read -- a DDC hit, or on a miss the cook that fills the DDC (1.2 s for a 1k noise texture in Debug) --
     * to a JobSystem worker. The worker's decoded result waits in `TextureUploads`, and `PumpUploads` creates
     * GPU images from it once per frame within `TextureUploadSettings::BytesPerFrame`. The texture stays
     * Pending, drawing the slot default, until its upload runs.
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

        /// ONE ROW OF A CLOSURE (AL1-8b): `Require`'s read start, with the loader handle to wait on appended
        /// to @p awaited while the read is in flight. Never reads on this thread.
        void StartRead( const Assets::AssetHandle& handle, std::vector<Assets::AssetHandle>& awaited ) const;

        /// A material that bound a slot default because @p texture was Pending; it is invalidated (rebuilt
        /// on its next draw) when the texture lands.
        void RebuildWhenReady( const Assets::AssetHandle& texture, const Assets::AssetHandle& material ) const;

        // Original source file of a texture (e.g. ".../foo.gif"), empty when the handle is unknown. The one
        // synchronous door: an unread shell is finished NOW through AsyncAssetLoader::FlushOne, which the
        // SyncLoadLedger counts.
        std::string GetSourcePath( const Assets::AssetHandle& handle ) const;

        /// Eviction (WP14b): forget the GPU texture built for @p handle, keeping the shell, so the next `Require`
        /// reads the file and builds it again. False when nothing is built for it. The image is not freed here:
        /// frames recorded before the sweep may still sample it, so it is parked (Assets::FrameRetireQueue).
        bool EvictBuilt( const Assets::AssetHandle& handle );

        /// Free the parked textures no frame in flight can still sample. Every frame, from the frame loop.
        std::size_t RetireEvicted();

        /// Create the GPU textures whose cook finished, within the per-frame upload budget. Every frame, from
        /// the frame loop, main thread. Returns how many results were taken (built, failed or stale).
        std::size_t PumpUploads();

        /// Cooked results still waiting for their upload (tests, diagnostics).
        std::size_t PendingUploads() const;

        void Clear();

    private:
        struct Entry
        {
            std::shared_ptr<Assets::TextureAsset> Source;
            std::shared_ptr<Graphic::Texture2D>   Built;
            Assets::LoadRequest                   Request;
            /// Which cook the entry waits for; a result carrying another ticket is stale and dropped.
            uint64_t CookTicket = 0;
            /// The platform data is being read or cooked on a worker, or waits for its upload.
            bool Cooking = false;
            /// Latched so a corrupt file is not re-read every frame.
            bool Failed = false;
        };

        Entry*      FindOrDiscover( const Assets::AssetHandle& handle ) const;
        void        BeginRead( const Assets::AssetHandle& handle, Entry& entry ) const;
        void        BeginCook( const Assets::AssetHandle& handle, Entry& entry ) const;
        void        FinishCook( const Assets::AssetHandle& handle, Entry& entry, TextureCookOutcome&& outcome );

        // Mutable: `Get` is const for its 25 callers and discovery on a miss is a cache fill, not a change
        // of what the service answers.
        mutable std::unordered_map<Assets::AssetHandle, Entry> m_Entries;
        mutable std::unordered_set<Assets::AssetHandle>        m_ReportedMissing;
        std::weak_ptr<Assets::AssetManager>                    m_Assets;
        /// Materials that drew a slot default while a texture was Pending.
        mutable TextureWaiters m_Waiters;
        /// Textures eviction dropped, alive until the frames that could sample them have retired.
        Assets::FrameRetireQueue<std::shared_ptr<Graphic::Texture2D>> m_Retiring;
        /// Shared with the cook jobs, which may still finish after this service is cleared or destroyed.
        std::shared_ptr<TextureUploads> m_Uploads        = std::make_shared<TextureUploads>();
        mutable uint64_t                m_NextCookTicket = 0;
        TextureUploadSettings           m_UploadSettings;
    };
} // namespace Desert::Runtime
