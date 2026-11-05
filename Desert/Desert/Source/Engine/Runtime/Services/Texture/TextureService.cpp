#include "TextureService.hpp"

#include <Engine/Assets/RegistryDiscovery.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Core/RenderTask.hpp>
#include <Engine/Graphic/TextureFactory.hpp>
#include <Engine/Graphic/TextureStreamingDemand.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/JobSystem.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/PathText.hpp>
#include <Common/Settings/Scalability.hpp>

#include <algorithm>
#include <chrono>

namespace Desert::Runtime
{
    namespace
    {
        // SAY WHOSE THE IMAGE IS, AS SOON AS IT EXISTS. A `Texture2D` is a thin wrapper around an
        // ImageHandle, so the row that costs device memory is the Image's, not the texture's — and until
        // somebody names the asset behind it the ledger correctly reports it as unclaimed. This is the one
        // place that knows both halves. See Engine/Graphic/ResourceLedger.hpp.
        void ClaimTextureImage( const std::shared_ptr<Graphic::Texture2D>& texture,
                                const Assets::AssetHandle&                 asset )
        {
            if ( !texture )
                return;
            if ( auto* image = ResourceRegistry::GetImageService()->Resolve( texture->GetImageHandle() ) )
                image->ClaimOwnership( Graphic::ResourceOwner::AssetService, asset );
        }
    } // namespace

    void TextureService::BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets )
    {
        m_Assets = assets;
    }

    Common::ResultStr<Assets::AssetHandle> TextureService::HandleForPath( const std::filesystem::path& path )
    {
        const auto assets = m_Assets.lock();
        if ( !assets )
            return Common::MakeError<Assets::AssetHandle>( std::string( "no asset manager is bound" ) );
        const std::filesystem::path full =
             path.is_absolute() ? path : ( Common::Constants::Path::ASSETS_PATH / path ).lexically_normal();
        std::error_code ec;
        if ( !std::filesystem::exists( full, ec ) )
            return Common::MakeFormattedError<Assets::AssetHandle>( "no texture at '{}'", full.string() );
        auto asset = assets->FindByPath<Assets::TextureAsset>( full );
        if ( !asset )
            asset = assets->CreateAsset<Assets::TextureAsset>( full, /*loadAfterCreate=*/false );
        if ( !asset )
            return Common::MakeFormattedError<Assets::AssetHandle>( "'{}' cannot be registered as a texture",
                                                                    full.string() );
        return Common::MakeSuccess<Assets::AssetHandle>( Assets::AssetHandle( asset->GetMetadata().Handle ) );
    }

    void TextureService::Register( const std::shared_ptr<Assets::TextureAsset>& texture )
    {
        Entry& entry = m_Entries[texture->GetHandle()];
        entry.Source = texture;
        entry.Built  = Graphic::TextureFactory::Create2D( texture );
        entry.Failed = false;
        ClaimTextureImage( entry.Built, texture->GetHandle() );
    }

    void TextureService::RegisterAsset( const std::shared_ptr<Assets::TextureAsset>& texture )
    {
        if ( texture )
            m_Entries[texture->GetHandle()].Source = texture; // read and built on the first Require
    }

    TextureService::Entry* TextureService::FindOrDiscover( const Assets::AssetHandle& handle ) const
    {
        if ( static_cast<uint64_t>( handle ) == 0 )
            return nullptr;
        if ( const auto it = m_Entries.find( handle ); it != m_Entries.end() )
            return &it->second;
        if ( m_ReportedMissing.contains( handle ) )
            return nullptr;

        auto created = Assets::CreateFromRegistryRow<Assets::TextureAsset>(
             m_Assets, handle, Common::Content::ContentKind::Texture );
        if ( !created )
        {
            // Once per handle, not once per frame: the caller falls back to the slot default and says so.
            m_ReportedMissing.insert( handle );
            LOG_ERROR( "[TextureService] {}", created.GetError() );
            return nullptr;
        }
        Entry& entry = m_Entries[handle];
        entry.Source = created.GetValue();
        return &entry;
    }

    void TextureService::BeginCook( const Assets::AssetHandle& handle, Entry& entry ) const
    {
        // OFF THE MAIN THREAD. The worker does the DDC read and, on a miss, the cook that fills it -- the
        // 1.2 s THUMB3 measured on the frame that first touched 1k_Dissolve_Noise_Texture. The worker only
        // decodes; the GPU image is created by PumpUploads, under the per-frame budget.
        entry.Cooking    = true;
        entry.CookTicket = ++m_NextCookTicket;
        // Still Pending either way; a cook past the admission capacity starts when PumpUploads frees a slot.
        if ( m_CookAdmission.TryAdmit( handle ) )
            SubmitCook( handle, entry );
    }

    void TextureService::SubmitCook( const Assets::AssetHandle& handle, const Entry& entry ) const
    {
        struct CookJob
        {
            Assets::AssetHandle             Handle;
            uint64_t                        Ticket = 0;
            std::filesystem::path           Asset;
            std::shared_ptr<TextureUploads> Uploads;
            uint32_t                        FirstMip = 0;
            uint32_t                        TailSize = 0;
        };
        auto job = std::make_shared<CookJob>( CookJob{ handle, entry.CookTicket,
                                                       entry.Source->GetMetadata().Filepath, m_Uploads,
                                                       entry.RequestedFirstMip, m_StreamingSettings.TailSize } );
        Common::JobSystem::Get().Submit(
             [job]() noexcept
             {
                 TextureCookOutcome outcome;
                 const auto         began  = std::chrono::steady_clock::now();
                 auto cooked = Graphic::Texture2D::ReadCookedMips( job->Asset, job->FirstMip, job->TailSize );
                 LOG_DEBUG( "[TextureService] '{}' platform data read on a worker in {:.1f} ms",
                            Common::PathUtf8( job->Asset.filename() ),
                            std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - began )
                                 .count() );
                 uint64_t bytes = 0;
                 if ( cooked.IsSuccess() )
                 {
                     // CPU-STREAM-UPLOAD-RING: the device image, its views and the staging write happen HERE, on
                     // the worker; the main thread only records the copies (PumpUploads).
                     auto prepared = Graphic::Texture2D::PrepareCooked( cooked.ExtractValue() );
                     if ( prepared.IsSuccess() )
                     {
                         outcome.Prepared = prepared.ExtractValue();
                         bytes            = outcome.Prepared->Bytes;
                     }
                     else
                         outcome.Error = prepared.GetError();
                 }
                 else
                     outcome.Error = cooked.GetError();
                 job->Uploads->Push( { job->Handle, job->Ticket, bytes, std::move( outcome ) } );
             } );
    }

    void TextureService::FinishCook( const Assets::AssetHandle& handle, Entry& entry, TextureCookOutcome outcome,
                                     const double recordMs )
    {
        entry.Cooking = false;
        // A STREAMING READ (TEX-STREAMING) finishes over a texture that is already drawn: its new levels go INTO
        // the same texture (Texture2D::ReplaceLevels -- the Image2D every material slot and override points at
        // keeps its address and bindless slot, UE's RHIUpdateTextureReference), so no holder is rebuilt or left
        // with a released image. A read that fails leaves the resident levels drawing.
        const bool streamed = entry.Built != nullptr;
        bool       replaced = false;
        if ( outcome.Prepared )
        {
            const auto     began    = std::chrono::steady_clock::now();
            const uint64_t bytes    = outcome.Prepared->Bytes;
            const uint32_t firstMip = outcome.Prepared->FirstMip;
            entry.FullWidth         = outcome.Prepared->FullWidth;
            entry.FullHeight        = outcome.Prepared->FullHeight;
            entry.LevelBytes        = outcome.Prepared->FullLevelBytes;
            if ( streamed )
            {
                // THE UPLOAD NOW, THE SWAP AT THE HAND-OFF (OVERLAP-ON-7; UE FRenderAssetUpdate: the RHI swap is a
                // render command, applied between frames). The new levels go into a NEW image nothing reads; the
                // live Image2D every material slot names (its views, layout and bindless row) takes them only
                // once the render work in flight has finished: RenderTask::AfterJoin runs it at once when nothing
                // is in flight, else at the next Join (Scene::OnUpdate's, before this tick's views).
                auto staged = entry.Built->StagePrepared( *outcome.Prepared );
                replaced    = staged.IsSuccess();
                if ( replaced )
                    AdoptAtHandOff( entry.Built, staged.ExtractValue() );
                else
                    outcome.Error = staged.GetError();
            }
            else
            {
                auto built = Graphic::Texture2D::CreateFromPrepared( *outcome.Prepared );
                if ( built.IsSuccess() )
                {
                    entry.Built = built.ExtractValue();
                    replaced    = true;
                }
                else
                    outcome.Error = built.GetError();
            }
            if ( replaced )
                entry.ResidentFirstMip = firstMip;
            LOG_DEBUG(
                 "[TextureService] '{}' uploaded on the main thread in {:.1f} ms ({} bytes, from mip {})",
                 entry.Source->GetMetadata().Filepath.filename().string(),
                 std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - began ).count() +
                      recordMs,
                 bytes, firstMip );
        }
        if ( streamed )
        {
            if ( !replaced )
            {
                LOG_WARN( "[TextureStreaming] '{}' could not move to mip {}: {}. Its resident mips stay drawn.",
                          entry.Source->GetMetadata().Filepath.filename().string(), entry.RequestedFirstMip,
                          outcome.Error );
                entry.RequestedFirstMip = entry.ResidentFirstMip;
            }
            return;
        }
        if ( !entry.Built )
        {
            entry.Failed = true;
            LOG_ERROR( "[TextureService] '{}' was read but no GPU texture could be built from it: {}. Materials "
                       "naming it sample the slot's schema default; it is not retried.",
                       entry.Source->GetMetadata().Filepath.string(), outcome.Error );
        }
        else
            ClaimTextureImage( entry.Built, handle );

        // At the hand-off (MAT-CELL-LOAD): the render job builds cells on its workers (OVERLAP-ON-7), adding
        // waiters (RebuildWhenReady) and asking them (AwaitsTextures) under the material service's lock; the
        // settle and the invalidations it raises wait for that work, as the texture's own swap does.
        Engine::RenderTask::Get().AfterJoin(
             [this, handle]()
             {
                 m_Waiters.Settle( handle, []( const Assets::AssetHandle& material )
                                   { ResourceRegistry::GetMaterialService()->Invalidate( material ); } );
             } );
    }

    void TextureService::BeginRead( const Assets::AssetHandle& handle, Entry& entry ) const
    {
        // BOTH DELEGATES (T2.3): a Clear() that drops the request must not leave the entry answering
        // Pending at a worker that is never coming back.
        entry.Request = Assets::AsyncAssetLoader::Get().Request(
             entry.Source,
             [this, handle]( const Assets::Asset<Assets::AssetBase>&, const Assets::LoadOutcome outcome,
                             const std::string& error )
             {
                 const auto it = m_Entries.find( handle );
                 if ( it == m_Entries.end() )
                     return; // cleared while the read ran
                 // Released first, so nothing answers Pending for a texture whose read has settled.
                 it->second.Request.Release();
                 if ( outcome == Assets::LoadOutcome::Loaded )
                 {
                     // Still Pending: the waiters settle when the upload runs (FinishCook).
                     BeginCook( handle, it->second );
                     return;
                 }
                 it->second.Failed = true;
                 LOG_ERROR( "[TextureService] '{}' could not be read: {}. Materials naming it sample the "
                            "slot's schema default; it is not retried.",
                            it->second.Source->GetMetadata().Filepath.string(), error );
                 // At the hand-off, as a landed texture settles (MAT-CELL-LOAD).
                 Engine::RenderTask::Get().AfterJoin(
                      [this, handle]()
                      {
                          m_Waiters.Settle( handle, []( const Assets::AssetHandle& material )
                                            { ResourceRegistry::GetMaterialService()->Invalidate( material ); } );
                      } );
             },
             [this, handle]
             {
                 // Back to "not asked for", not to "failed": the next Require starts a fresh read.
                 if ( const auto it = m_Entries.find( handle ); it != m_Entries.end() )
                     it->second.Request.Release();
             } );
    }

    Assets::AssetRef<Graphic::Texture2D> TextureService::Require( const Assets::AssetHandle& handle ) const
    {
        Entry* entry = FindOrDiscover( handle );
        if ( entry == nullptr )
            return Assets::AssetRef<Graphic::Texture2D>::Null();
        if ( entry->Built )
            return Assets::AssetRef<Graphic::Texture2D>::Ready( handle, entry->Built );
        if ( entry->Failed || !entry->Source )
            return Assets::AssetRef<Graphic::Texture2D>::Null();
        if ( entry->Request.IsValid() || entry->Cooking )
            return Assets::AssetRef<Graphic::Texture2D>::Pending( handle );

        // Already read (an importer handed over a loaded asset): no request needed, straight to the cook.
        if ( entry->Source->IsReadyForUse() )
        {
            BeginCook( handle, *entry );
            return Assets::AssetRef<Graphic::Texture2D>::Pending( handle );
        }

        BeginRead( handle, *entry );
        return Assets::AssetRef<Graphic::Texture2D>::Pending( handle );
    }

    Graphic::Texture2D* TextureService::Get( const Assets::AssetHandle& handle ) const
    {
        const auto ref = Require( handle );
        return ref.Get();
    }

    void TextureService::StartRead( const Assets::AssetHandle&        handle,
                                    std::vector<Assets::AssetHandle>& awaited ) const
    {
        (void)Require( handle );
        const auto it = m_Entries.find( handle );
        if ( it != m_Entries.end() && it->second.Request.IsValid() && it->second.Source )
            awaited.emplace_back( it->second.Source->GetMetadata().Handle );
    }

    void TextureService::RebuildWhenReady( const Assets::AssetHandle& texture,
                                           const Assets::AssetHandle& material ) const
    {
        if ( const auto it = m_Entries.find( texture );
             it != m_Entries.end() && ( it->second.Request.IsValid() || it->second.Cooking ) )
            m_Waiters.Add( texture, material );
    }

    std::string TextureService::GetSourcePath( const Assets::AssetHandle& handle ) const
    {
        (void)Require( handle );
        const auto it = m_Entries.find( handle );
        if ( it == m_Entries.end() || !it->second.Source )
            return {};
        if ( !it->second.Source->IsReadyForUse() && it->second.Request.IsValid() )
            Assets::AsyncAssetLoader::Get().FlushOne( handle );
        const auto again = m_Entries.find( handle );
        return again != m_Entries.end() && again->second.Source && again->second.Source->IsReadyForUse()
                    ? again->second.Source->GetSourcePath()
                    : std::string{};
    }

    bool TextureService::EvictBuilt( const Assets::AssetHandle& handle )
    {
        const auto it = m_Entries.find( handle );
        if ( it == m_Entries.end() || !it->second.Built )
            return false;
        // No shell means no recipe: a texture handed over by Register with nothing to read it back from. Only
        // the image holds it, so it is kept, and said - a silent "nothing to do" reads as "already released".
        if ( !it->second.Source )
        {
            LOG_WARN( "[TextureService] eviction asked for texture {} and it has no asset shell, so nothing could "
                      "rebuild it. Kept.",
                      static_cast<uint64_t>( handle ) );
            return false;
        }
        m_Retiring.Park( std::move( it->second.Built ),
                         Engine::FrameManager::GetInstance().GetAbsoluteFrameCount() );
        it->second.Built.reset();
        // Read back from its tail, like any texture asked for the first time.
        it->second.RequestedFirstMip = Graphic::Texture2D::kStreamingTail;
        return true;
    }

    std::size_t TextureService::RetireEvicted()
    {
        const auto& frames = Engine::FrameManager::GetInstance();
        return m_Retiring.Collect( frames.GetAbsoluteFrameCount(), frames.GetMaxFramesInFlight() );
    }

    void TextureService::AdoptAtHandOff( const std::shared_ptr<Graphic::Texture2D>& texture,
                                         Graphic::Texture2D::StagedLevels           staged )
    {
        Engine::RenderTask::Get().AfterJoin(
             [texture, staged = std::move( staged )]()
             {
                 if ( const auto taken = texture->AdoptStagedLevels( staged ); !taken.IsSuccess() )
                     LOG_ERROR( "[TextureStreaming] a streamed read's levels were not swapped in at the hand-off: "
                                "{}. Its resident mips stay drawn; the entry counts the new ones.",
                                taken.GetError() );
             } );
    }

    void TextureService::StreamOutAtHandOff( const std::shared_ptr<Graphic::Texture2D>& texture,
                                             const uint32_t                             droppedLevels )
    {
        Engine::RenderTask::Get().AfterJoin(
             [texture, droppedLevels]()
             {
                 if ( const auto dropped = texture->StreamOutLevels( droppedLevels ); !dropped.IsSuccess() )
                     LOG_ERROR( "[TextureStreaming] a stream-out of {} level(s) failed at the hand-off: {}. Its "
                                "resident mips stay drawn; the entry counts the kept ones.",
                                droppedLevels, dropped.GetError() );
             } );
    }

    std::size_t TextureService::PumpUploads()
    {
        // THE UPLOAD BATCHES THE GPU HAS FINISHED (their upload-timeline value is reached): dropping them retires
        // the upload-ring slices they copied out of. Never waits: an unfinished batch stays for a later pump.
        std::erase_if( m_UploadBatches,
                       []( const std::unique_ptr<Graphic::GpuBatch>& batch ) { return batch->IsComplete(); } );

        // New results only while what is already recording leaves room in this frame's budget.
        uint64_t owed = 0;
        for ( const RecordingUpload& recording : m_Recording )
            owed += recording.Remaining;
        std::size_t takenCount = 0;
        if ( owed < m_UploadSettings.BytesPerFrame )
        {
            auto taken = m_Uploads->TakeWithinBudget( m_UploadSettings.BytesPerFrame - owed );
            takenCount = taken.size();
            for ( auto& item : taken )
            {
                const auto it = m_Entries.find( item.Handle );
                // Stale: the entry was cleared, or a later cook replaced the one this result belongs to.
                if ( it == m_Entries.end() || !it->second.Cooking || it->second.CookTicket != item.Ticket )
                    continue;
                // Read before the move: argument evaluation order is unspecified (clang L->R, MSVC R->L).
                const Assets::AssetHandle handle = item.Handle;
                if ( !item.Data.Prepared )
                {
                    FinishCook( handle, it->second, std::move( item.Data ) ); // the read failed: settles now
                    continue;
                }
                const uint64_t bytes = item.Data.Prepared->Bytes;
                m_Recording.push_back( RecordingUpload{ handle, item.Ticket, bytes, 0.0,
                                                        std::move( *item.Data.Prepared ) } );
            }
            // Every taken result, stale ones included, was one admitted cook: its decoded pixels are gone now
            // (in the upload ring, or held by the image on the full path).
            for ( std::size_t released = 0; released < takenCount; ++released )
                m_CookAdmission.Release();
            StartAdmittedCooks();
        }
        if ( m_Recording.empty() )
            return takenCount;

        // ONE COMMAND BUFFER FOR EVERY COPY OF THIS PUMP (CPU-STREAM-UPLOAD-RING; UE FRHIResourceUpdateBatcher), queued
        // on the submit thread in order with the frames (GpuBatch::Submit -> RT_SubmitCommandBufferGraphic), so a
        // texture finished below is copied before the first frame that samples it.
        auto begun = Graphic::GpuBatch::Begin();
        if ( !begun.IsSuccess() )
        {
            LOG_ERROR( "[TextureService] no upload batch this frame: {}. {} texture(s) wait for the next one.",
                       begun.GetError(), m_Recording.size() );
            return takenCount;
        }
        std::unique_ptr<Graphic::GpuBatch> batch = begun.ExtractValue();
        std::vector<RecordingUpload>       finished;
        uint64_t                           spent = 0;
        for ( auto it = m_Recording.begin(); it != m_Recording.end(); )
        {
            const auto entryIt = m_Entries.find( it->Handle );
            if ( entryIt == m_Entries.end() || !entryIt->second.Cooking || entryIt->second.CookTicket != it->Ticket )
            {
                it = m_Recording.erase( it ); // stale: its image and its slice go with it
                continue;
            }
            if ( spent >= m_UploadSettings.BytesPerFrame )
                break;
            const uint64_t budget = m_UploadSettings.BytesPerFrame - spent;
            const auto     began  = std::chrono::steady_clock::now();
            auto           step   = it->Prepared.Image->RecordUploadStep( *batch, budget );
            const auto recorded = std::chrono::steady_clock::now() - began;
            it->RecordMs += std::chrono::duration<double, std::milli>( recorded ).count();
            if ( !step.IsSuccess() )
            {
                TextureCookOutcome failed;
                failed.Error                     = step.GetError();
                const Assets::AssetHandle handle = it->Handle;
                it                               = m_Recording.erase( it );
                FinishCook( handle, entryIt->second, std::move( failed ) );
                continue;
            }
            const Graphic::Image2D::RecordedUploadStep done = step.GetValue();
            if ( done.Bytes == 0 && !done.Complete )
                break; // the ring is full: the order is kept and the next pump retries
            spent += done.Bytes;
            it->Remaining = it->Remaining > done.Bytes ? it->Remaining - done.Bytes : 0;
            if ( !done.Complete )
                break; // the budget is spent: its next levels are the next frame's
            finished.push_back( std::move( *it ) );
            it = m_Recording.erase( it );
        }
        if ( const auto submitted = batch->Submit(); !submitted.IsSuccess() )
        {
            LOG_ERROR( "[TextureService] the upload batch was not submitted: {}", submitted.GetError() );
            return takenCount;
        }
        m_UploadBatches.push_back( std::move( batch ) );

        // THE TEXTURES WHOSE LAST LEVEL IS IN THE BATCH JUST SUBMITTED: they are drawn from here on.
        for ( RecordingUpload& done : finished )
        {
            const auto entryIt = m_Entries.find( done.Handle );
            if ( entryIt == m_Entries.end() )
                continue;
            TextureCookOutcome outcome;
            outcome.Prepared = std::move( done.Prepared );
            FinishCook( done.Handle, entryIt->second, std::move( outcome ), done.RecordMs );
        }
        return takenCount;
    }

    void TextureService::StartAdmittedCooks()
    {
        while ( const std::optional<Assets::AssetHandle> next = m_CookAdmission.NextToStart() )
        {
            const auto it = m_Entries.find( *next );
            // Cleared or settled while it waited: no job will ever push for it, so the slot goes straight back.
            if ( it == m_Entries.end() || !it->second.Cooking )
            {
                m_CookAdmission.Release();
                continue;
            }
            SubmitCook( *next, it->second );
        }
    }

    namespace
    {
        uint64_t BytesFrom( const std::vector<uint64_t>& levels, const uint32_t firstMip )
        {
            uint64_t bytes = 0;
            for ( std::size_t level = firstMip; level < levels.size(); ++level )
                bytes += levels[level];
            return bytes;
        }
    } // namespace

    void TextureService::UpdateStreaming()
    {
        ++m_StreamingFrame;
        const TextureStreamingSettings& settings = m_StreamingSettings;
        for ( const auto& [asset, pixels] : Graphic::TextureStreamingDemand::Get().TakeFrame() )
        {
            if ( const auto it = m_Entries.find( asset ); it != m_Entries.end() )
            {
                it->second.LastAskedFrame = m_StreamingFrame;
                it->second.AskedPixels    = pixels;
            }
        }

        // WANTED MIPS (UE FRenderAssetStreamingManager / FStreamingRenderAsset::GetWantedMips): the smallest level
        // that still gives the screen the texels it covers; the tail for a texture no draw asked for lately.
        struct Want
        {
            Assets::AssetHandle Handle;
            Entry*              Row   = nullptr;
            uint32_t            First = 0;
            uint32_t            Tail  = 0;
        };
        std::vector<Want> wants;
        uint64_t          resident = 0, full = 0, wanted = 0;
        uint32_t          streaming = 0;
        for ( auto& [handle, entry] : m_Entries )
        {
            if ( !entry.Built || !entry.Source || entry.LevelBytes.empty() )
                continue;
            if ( entry.Cooking )
                ++streaming;
            const auto levels = static_cast<uint32_t>( entry.LevelBytes.size() );
            uint32_t   tail   = 0;
            while ( std::max( entry.FullWidth >> tail, entry.FullHeight >> tail ) > settings.TailSize &&
                    tail + 1 < levels )
                ++tail;
            uint32_t first = tail;
            if ( entry.AskedPixels > 0.0f && m_StreamingFrame - entry.LastAskedFrame <= settings.UnseenFrames )
            {
                const float    needed = entry.AskedPixels * settings.TexelsPerPixel;
                const uint32_t top    = std::max( entry.FullWidth, entry.FullHeight );
                first                 = 0;
                while ( first < tail && static_cast<float>( top >> ( first + 1 ) ) >= needed )
                    ++first;
            }
            resident += BytesFrom( entry.LevelBytes, entry.ResidentFirstMip );
            full += BytesFrom( entry.LevelBytes, 0 );
            wanted += BytesFrom( entry.LevelBytes, first );
            wants.push_back( { handle, &entry, first, tail } );
        }

        // THE POOL (r.Streaming.PoolSize): over budget, the least asked-for textures give up a level first, one
        // level per texture per pass, until the wanted set fits (UE drops the lowest-priority mips first).
        const uint64_t budget = static_cast<uint64_t>( Common::Scalability::QualityState::Resolved().As<uint32_t>(
                                     Common::Scalability::Parameter::TextureStreamingPoolMiB ) ) *
                                1024ull * 1024ull;
        const bool overBudget = wanted > budget;
        if ( overBudget )
        {
            std::sort( wants.begin(), wants.end(),
                       []( const Want& a, const Want& b ) { return a.Row->AskedPixels < b.Row->AskedPixels; } );
            for ( bool dropped = true; wanted > budget && dropped; )
            {
                dropped = false;
                for ( Want& want : wants )
                {
                    if ( wanted <= budget )
                        break;
                    if ( want.First >= want.Tail )
                        continue;
                    wanted -= want.Row->LevelBytes[want.First];
                    ++want.First;
                    dropped = true;
                }
            }
        }

        // START THE MOVES, nearest first: a texture short of its wanted level reads it now; one holding more than
        // it wants gives it back when it is two levels over (one level of hysteresis keeps a texture at the edge
        // of two sizes from being read every frame) or when the pool is over budget.
        std::sort( wants.begin(), wants.end(),
                   []( const Want& a, const Want& b ) { return a.Row->AskedPixels > b.Row->AskedPixels; } );
        for ( Want& want : wants )
        {
            Entry& entry = *want.Row;
            if ( entry.Cooking || want.First == entry.ResidentFirstMip )
                continue;
            const bool up = want.First < entry.ResidentFirstMip;
            const bool down =
                 want.First >= entry.ResidentFirstMip + 2 || ( overBudget && want.First > entry.ResidentFirstMip );
            if ( !up && !down )
                continue;
            const std::string name = entry.Source->GetMetadata().Filepath.filename().string();
            if ( down )
            {
                // A DROP READS NOTHING (UE FTexture2DStreamOut): the kept levels are copied on the GPU into a
                // smaller image the same Image2D takes; the .detex is read only to raise a texture again.
                // It moves the live image (a layout transition and the swap), so it runs at the hand-off like
                // a raise's swap (OVERLAP-ON-7): at once when nothing is in flight, else after the next Join.
                const uint32_t from = entry.ResidentFirstMip;
                StreamOutAtHandOff( entry.Built, want.First - from );
                entry.ResidentFirstMip = entry.RequestedFirstMip = want.First;
                resident -= BytesFrom( entry.LevelBytes, from ) - BytesFrom( entry.LevelBytes, want.First );
                LOG_INFO( "[TextureStreaming] '{}' streamed out mip {} -> {} by a GPU copy (no read)", name, from,
                          want.First );
                continue;
            }
            LOG_INFO( "[TextureStreaming] '{}' reads mip {} -> {} from its asset", name, entry.ResidentFirstMip,
                      want.First );
            entry.RequestedFirstMip = want.First;
            BeginCook( want.Handle, entry );
            ++streaming;
        }

        auto& pool = Graphic::TextureStreamingDemand::Get().Pool();
        pool.ResidentBytes.store( resident, std::memory_order_relaxed );
        pool.WantedBytes.store( wanted, std::memory_order_relaxed );
        pool.BudgetBytes.store( budget, std::memory_order_relaxed );
        pool.FullChainBytes.store( full, std::memory_order_relaxed );
        pool.Textures.store( static_cast<uint32_t>( wants.size() ), std::memory_order_relaxed );
        pool.Streaming.store( streaming, std::memory_order_relaxed );
        if ( m_StreamingFrame % settings.LogEveryFrames == 0 &&
             ( resident != m_LoggedResident || wanted != m_LoggedWanted ) )
        {
            m_LoggedResident    = resident;
            m_LoggedWanted      = wanted;
            constexpr double MB = 1024.0 * 1024.0;
            LOG_INFO(
                 "[TextureStreaming] pool resident {:.1f} MB / budget {:.1f} MB (wanted {:.1f} MB, full chains "
                 "{:.1f} MB, {} textures, {} streaming)",
                 static_cast<double>( resident ) / MB, static_cast<double>( budget ) / MB,
                 static_cast<double>( wanted ) / MB, static_cast<double>( full ) / MB, wants.size(), streaming );
        }
    }

    std::size_t TextureService::PendingUploads() const
    {
        return m_Uploads->Pending();
    }

    std::size_t TextureService::InFlight() const
    {
        return static_cast<std::size_t>(
             std::count_if( m_Entries.begin(), m_Entries.end(), []( const auto& row )
                            { return row.second.Request.IsValid() || row.second.Cooking; } ) );
    }

    void TextureService::Clear()
    {
        m_Recording.clear();
        m_UploadBatches.clear();
        // The cooks already on a worker keep their slots until their (now stale) results are taken; the results
        // already queued are dropped here, and so are their slots.
        for ( std::size_t released = m_Uploads->TakeWithinBudget( UINT64_MAX ).size(); released > 0; --released )
            m_CookAdmission.Release();
        m_CookAdmission.DropWaiting();
        m_Retiring.Clear();
        m_Entries.clear();
        m_ReportedMissing.clear();
        m_Waiters.Clear();
    }
} // namespace Desert::Runtime
