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
#include <utility>

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
        // The GPU texture is created before the lock and the one it replaces is freed after it.
        const Assets::AssetHandle           handle = texture->GetHandle();
        std::shared_ptr<Graphic::Texture2D> built  = Graphic::TextureFactory::Create2D( texture );
        std::shared_ptr<Graphic::Texture2D> replaced;
        {
            const std::unique_lock table( m_TableLock );
            Entry&                 entry = m_Entries[handle];
            entry.Source                 = texture;
            replaced                     = std::exchange( entry.Built, built );
            entry.Failed                 = false;
        }
        // A render worker of a frame in flight may still hold Get's raw pointer to the one replaced: it is parked
        // as an eviction is (CPU-IMAGE-RACE-2), never freed here.
        if ( replaced )
            m_Retiring.Park( std::move( replaced ), Engine::FrameManager::GetInstance().GetAbsoluteFrameCount() );
        ClaimTextureImage( built, handle );
    }

    void TextureService::RegisterAsset( const std::shared_ptr<Assets::TextureAsset>& texture )
    {
        if ( !texture )
            return;
        const std::unique_lock table( m_TableLock );
        m_Entries[texture->GetHandle()].Source = texture; // read and built on the first Require
    }

    std::optional<Assets::AssetRef<Graphic::Texture2D>> TextureService::Answer( const Assets::AssetHandle& handle,
                                                                                const Entry&               entry )
    {
        if ( entry.Built )
            return Assets::AssetRef<Graphic::Texture2D>::Ready( handle, entry.Built );
        if ( entry.Failed || !entry.Source )
            return Assets::AssetRef<Graphic::Texture2D>::Null();
        if ( entry.Reading || entry.Cooking )
            return Assets::AssetRef<Graphic::Texture2D>::Pending( handle );
        return std::nullopt;
    }

    bool TextureService::Discover( const Assets::AssetHandle& handle ) const
    {
        // ONE TEXTURE DISCOVERY AT A TIME, AND NOT UNDER THE TABLE LOCK: the registry row is checked on disk and
        // the shell is created in the AssetManager while the table's readers keep answering. The AssetManager has
        // no lock of its own and this one serializes only texture discovery (other services reach it: R19).
        const std::lock_guard discovering( m_DiscoverLock );
        {
            const std::shared_lock table( m_TableLock );
            if ( m_Entries.contains( handle ) )
                return true;
            if ( m_ReportedMissing.contains( handle ) )
                return false;
        }
        auto created = Assets::CreateFromRegistryRow<Assets::TextureAsset>(
             m_Assets, handle, Common::Content::ContentKind::Texture );
        bool firstReport = false;
        {
            const std::unique_lock table( m_TableLock );
            if ( created )
            {
                // A Register or RegisterAsset on another thread may have seeded the row meanwhile: its shell
                // stays.
                Entry& entry = m_Entries[handle];
                if ( !entry.Source )
                    entry.Source = created.GetValue();
                return true;
            }
            firstReport = m_ReportedMissing.insert( handle ).second;
        }
        // Once per handle, not once per frame: the caller falls back to the slot default and says so.
        if ( firstReport )
            LOG_ERROR( "[TextureService] {}", created.GetError() );
        return false;
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

    void TextureService::FinishCook( const Assets::AssetHandle& handle, const uint64_t ticket,
                                     TextureCookOutcome outcome, const double recordMs )
    {
        // THE RESULT IS CHECKED SHARED. Only the frame loop moves a cooking entry (a worker's Require starts
        // nothing on it), so what is read here still holds when the commit below takes the lock again.
        std::shared_ptr<Graphic::Texture2D>   built;
        std::shared_ptr<Assets::TextureAsset> source;
        uint32_t                              requestedFirstMip = 0;
        {
            const std::shared_lock table( m_TableLock );
            const auto             it = m_Entries.find( handle );
            // Stale: the entry was cleared, or a later cook replaced the one this result belongs to.
            if ( it == m_Entries.end() || !it->second.Cooking || it->second.CookTicket != ticket )
                return;
            built             = it->second.Built;
            source            = it->second.Source;
            requestedFirstMip = it->second.RequestedFirstMip;
        }
        std::string path, name;
        if ( source )
        {
            path = source->GetMetadata().Filepath.string();
            name = source->GetMetadata().Filepath.filename().string();
        }

        // A STREAMING READ (TEX-STREAMING) finishes over a texture that is already drawn: its new levels go INTO
        // the same texture (Texture2D::ReplaceLevels -- the Image2D every material slot and override points at
        // keeps its address and bindless slot, UE's RHIUpdateTextureReference), so no holder is rebuilt or left
        // with a released image. A read that fails leaves the resident levels drawing.
        // THE GPU WORK RUNS UNLOCKED: a render worker's Require or AwaitedBy does not wait behind an upload. The
        // prepared image's copies were recorded by RecordUploads, with the table unlocked too.
        const bool                          streamed = built != nullptr;
        const bool                          prepared = outcome.Prepared.has_value();
        bool                                replaced = false;
        std::shared_ptr<Graphic::Texture2D> created;
        uint32_t                            firstMip = 0, fullWidth = 0, fullHeight = 0;
        std::vector<uint64_t>               levelBytes;
        if ( prepared )
        {
            const auto     began = std::chrono::steady_clock::now();
            const uint64_t bytes = outcome.Prepared->Bytes;
            firstMip             = outcome.Prepared->FirstMip;
            fullWidth            = outcome.Prepared->FullWidth;
            fullHeight           = outcome.Prepared->FullHeight;
            levelBytes           = outcome.Prepared->FullLevelBytes;
            if ( streamed )
            {
                // THE UPLOAD NOW, THE SWAP AT THE HAND-OFF (OVERLAP-ON-7; UE FRenderAssetUpdate: the RHI swap is a
                // render command, applied between frames). The new levels go into a NEW image nothing reads; the
                // live Image2D every material slot names (its views, layout and bindless row) takes them only
                // once the render work in flight has finished: RenderTask::AfterJoin runs it at once when nothing
                // is in flight, else at the next Join (Scene::OnUpdate's, before this tick's views).
                auto staged = built->StagePrepared( *outcome.Prepared );
                replaced    = staged.IsSuccess();
                if ( replaced )
                    AdoptAtHandOff( built, staged.ExtractValue() );
                else
                    outcome.Error = staged.GetError();
            }
            else
            {
                auto made = Graphic::Texture2D::CreateFromPrepared( *outcome.Prepared );
                if ( made.IsSuccess() )
                {
                    created  = made.ExtractValue();
                    replaced = true;
                }
                else
                    outcome.Error = made.GetError();
            }
            const double settledMs =
                 std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - began ).count();
            LOG_DEBUG( "[TextureService] '{}' uploaded on the main thread in {:.1f} ms ({} bytes, from mip {})",
                       name, settledMs + recordMs, bytes, firstMip );
        }

        // THE COMMIT, EXCLUSIVE.
        {
            const std::unique_lock table( m_TableLock );
            const auto             it = m_Entries.find( handle );
            // Not reachable today (see above); a created image is then freed unused, after the lock.
            if ( it == m_Entries.end() || !it->second.Cooking || it->second.CookTicket != ticket )
                return;
            Entry& entry  = it->second;
            entry.Cooking = false;
            if ( prepared )
            {
                entry.FullWidth  = fullWidth;
                entry.FullHeight = fullHeight;
                entry.LevelBytes = std::move( levelBytes );
            }
            if ( replaced )
                entry.ResidentFirstMip = firstMip;
            if ( streamed )
            {
                if ( !replaced )
                    entry.RequestedFirstMip = entry.ResidentFirstMip;
            }
            else
            {
                entry.Built        = created;
                entry.Failed       = created == nullptr;
                entry.SettleQueued = true;
            }
        }

        if ( streamed )
        {
            if ( !replaced )
                LOG_WARN( "[TextureStreaming] '{}' could not move to mip {}: {}. Its resident mips stay drawn.",
                          name, requestedFirstMip, outcome.Error );
            return;
        }
        if ( !created )
        {
            LOG_ERROR( "[TextureService] '{}' was read but no GPU texture could be built from it: {}. Materials "
                       "naming it sample the slot's schema default; it is not retried.",
                       path, outcome.Error );
        }
        else
        {
            ClaimTextureImage( created, handle );
        }
        SettleAtHandOff( handle );
    }

    void TextureService::SettleAtHandOff( const Assets::AssetHandle& texture ) const
    {
        // At the hand-off (MAT-CELL-LOAD): the render job builds cells on its workers (OVERLAP-ON-7), adding
        // waiters (RebuildWhenReady) and asking them (AwaitedBy); the settle and the invalidations it raises wait
        // for that work, as the texture's own swap does. AfterJoin may run the command at once, so this is called
        // with the table lock released, and the materials are invalidated with it released again: the material
        // service's lock comes BEFORE this one in the one order, never under it. The full fence stays (not
        // AfterViews, CPU-GT-OVERLAP): MaterialService::Invalidate's writer guard joins the whole queue anyway,
        // and this runs once per texture creation, not per frame.
        Engine::RenderTask::Get().AfterJoin(
             [this, texture]() noexcept
             {
                 std::vector<Assets::AssetHandle> waited;
                 {
                     const std::unique_lock table( m_TableLock );
                     if ( const auto it = m_Entries.find( texture ); it != m_Entries.end() )
                         it->second.SettleQueued = false;
                     m_Waiters.Settle( texture, [&waited]( const Assets::AssetHandle& material )
                                       { waited.push_back( material ); } );
                     m_AnyWaiting.store( !m_Waiters.Empty(), std::memory_order_release );
                 }
                 for ( const auto& material : waited )
                     ResourceRegistry::GetMaterialService()->Invalidate( material );
             } );
    }

    void TextureService::BeginRead( const Assets::AssetHandle& handle, Entry& entry ) const
    {
        // BOTH DELEGATES (T2.3): a Clear() that drops the request must not leave the entry answering
        // Pending at a worker that is never coming back. Asked under the caller's exclusive lock: Request never
        // calls a delegate before it returns, and the delegates (main thread) take the table lock themselves.
        entry.Request = Assets::AsyncAssetLoader::Get().Request(
             entry.Source,
             [this, handle]( const Assets::Asset<Assets::AssetBase>&, const Assets::LoadOutcome outcome,
                             const std::string& error ) noexcept
             {
                 std::string failedPath;
                 {
                     const std::unique_lock table( m_TableLock );
                     const auto             it = m_Entries.find( handle );
                     if ( it == m_Entries.end() )
                         return; // cleared while the read ran
                     // The read ends and the cook (or the failure) begins in ONE exclusive section: a worker sees
                     // Reading, Cooking or SettleQueued, never a gap between them.
                     it->second.Request.Release();
                     it->second.Reading = false;
                     if ( outcome == Assets::LoadOutcome::Loaded )
                     {
                         // Still Pending: the waiters settle when the upload runs (FinishCook).
                         BeginCook( handle, it->second );
                         return;
                     }
                     it->second.Failed       = true;
                     it->second.SettleQueued = true;
                     failedPath              = it->second.Source->GetMetadata().Filepath.string();
                 }
                 LOG_ERROR( "[TextureService] '{}' could not be read: {}. Materials naming it sample the "
                            "slot's schema default; it is not retried.",
                            failedPath, error );
                 // At the hand-off, as a landed texture settles (MAT-CELL-LOAD).
                 SettleAtHandOff( handle );
             },
             [this, handle]() noexcept
             {
                 // Back to "not asked for", not to "failed": the next Require starts a fresh read.
                 const std::unique_lock table( m_TableLock );
                 if ( const auto it = m_Entries.find( handle ); it != m_Entries.end() )
                 {
                     it->second.Request.Release();
                     it->second.Reading = false;
                 }
             } );
        // Request refuses only a null shell or a null delegate, neither possible here, so one of the two
        // delegates above is owed and clears this under the lock the caller holds now.
        entry.Reading = true;
    }

    Assets::AssetRef<Graphic::Texture2D> TextureService::Require( const Assets::AssetHandle& handle ) const
    {
        if ( static_cast<uint64_t>( handle ) == 0 )
            return Assets::AssetRef<Graphic::Texture2D>::Null();
        // THE HIT, SHARED: a texture built, failed or in flight is answered by every worker at once.
        bool known = false;
        {
            const std::shared_lock table( m_TableLock );
            if ( const auto it = m_Entries.find( handle ); it != m_Entries.end() )
            {
                if ( auto answer = Answer( handle, it->second ) )
                    return std::move( *answer );
                known = true;
            }
            else if ( m_ReportedMissing.contains( handle ) )
                return Assets::AssetRef<Graphic::Texture2D>::Null();
        }
        if ( !known && !Discover( handle ) )
            return Assets::AssetRef<Graphic::Texture2D>::Null();

        // THE MISS, EXCLUSIVE AND RE-CHECKED: another thread may have started the read between the two locks.
        const std::unique_lock table( m_TableLock );
        const auto             it = m_Entries.find( handle );
        if ( it == m_Entries.end() )
            return Assets::AssetRef<Graphic::Texture2D>::Null(); // cleared between the two locks
        if ( auto answer = Answer( handle, it->second ) )
            return std::move( *answer );
        // Already read (an importer handed over a loaded asset): no request needed, straight to the cook.
        if ( it->second.Source->IsReadyForUse() )
            BeginCook( handle, it->second );
        else
            BeginRead( handle, it->second );
        return Assets::AssetRef<Graphic::Texture2D>::Pending( handle );
    }

    bool TextureService::AwaitedBy( const Assets::AssetHandle& material ) const
    {
        // THE STEADY FRAME TAKES NO LOCK: asked per draw by every render worker, and once the scene's textures
        // have landed nothing waits. The flag is raised under the exclusive lock inside the build that added
        // the waiter (under the material lock), so a worker that sees that build's cell sees the flag.
        if ( !m_AnyWaiting.load( std::memory_order_acquire ) )
            return false;
        const std::shared_lock table( m_TableLock );
        return m_Waiters.IsWaiting( material );
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
        const std::shared_lock table( m_TableLock );
        const auto             it = m_Entries.find( handle );
        if ( it != m_Entries.end() && it->second.Request.IsValid() && it->second.Source )
            awaited.emplace_back( it->second.Source->GetMetadata().Handle );
    }

    void TextureService::RebuildWhenReady( const Assets::AssetHandle& texture,
                                           const Assets::AssetHandle& material ) const
    {
        // UNTIL THE SETTLE HAS RUN, not until the upload: a texture that landed after this material's Require
        // answered Pending, its settle still queued behind the render work in flight, settles this waiter too.
        const std::unique_lock table( m_TableLock );
        const auto             it = m_Entries.find( texture );
        if ( it == m_Entries.end() )
            return;
        if ( !it->second.Reading && !it->second.Cooking && !it->second.SettleQueued )
            return;
        m_Waiters.Add( texture, material );
        m_AnyWaiting.store( true, std::memory_order_release );
    }

    std::string TextureService::GetSourcePath( const Assets::AssetHandle& handle ) const
    {
        (void)Require( handle );
        std::shared_ptr<Assets::TextureAsset> source;
        bool                                  reading = false;
        {
            const std::shared_lock table( m_TableLock );
            const auto             it = m_Entries.find( handle );
            if ( it == m_Entries.end() || !it->second.Source )
                return {};
            source  = it->second.Source;
            reading = it->second.Request.IsValid();
        }
        // UNLOCKED: the flush reads the file and runs the read's completion, which takes the table lock.
        if ( !source->IsReadyForUse() && reading )
            Assets::AsyncAssetLoader::Get().FlushOne( handle );
        if ( !source->IsReadyForUse() )
            return {};
        return source->GetSourcePath();
    }

    bool TextureService::EvictBuilt( const Assets::AssetHandle& handle )
    {
        std::shared_ptr<Graphic::Texture2D> evicted;
        {
            const std::unique_lock table( m_TableLock );
            const auto             it = m_Entries.find( handle );
            if ( it == m_Entries.end() || !it->second.Built )
                return false;
            if ( it->second.Source )
            {
                evicted = std::move( it->second.Built );
                // Read back from its tail, like any texture asked for the first time.
                it->second.RequestedFirstMip = Graphic::Texture2D::kStreamingTail;
            }
        }
        // No shell means no recipe: a texture handed over by Register with nothing to read it back from. Only
        // the image holds it, so it is kept, and said - a silent "nothing to do" reads as "already released".
        if ( !evicted )
        {
            LOG_WARN( "[TextureService] eviction asked for texture {} and it has no asset shell, so nothing could "
                      "rebuild it. Kept.",
                      static_cast<uint64_t>( handle ) );
            return false;
        }
        m_Retiring.Park( std::move( evicted ), Engine::FrameManager::GetInstance().GetAbsoluteFrameCount() );
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
        // THE FULL FENCE STAYS (AfterJoin, not AfterViews; CPU-GT-OVERLAP), here and in StreamOutAtHandOff: the
        // swap moves the live Image2D (its views, layout and bindless row), and the interface draw of frame N-1 -
        // still in flight when a view bracket's JoinViews returns - samples that same image wherever a panel shows
        // it (UICacheTextureImGui caches an ImTextureID per Image2D). Only a streaming frame defers these.
        Engine::RenderTask::Get().AfterJoin(
             [texture, staged = std::move( staged )]() noexcept
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
             [texture, droppedLevels]() noexcept
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
        // Every recording holds an admission, so CooksInFlight bounds both lists: reserved once, never grown here.
        if ( m_Recording.capacity() < m_UploadSettings.CooksInFlight )
        {
            m_Recording.reserve( m_UploadSettings.CooksInFlight );
            m_Finished.reserve( m_UploadSettings.CooksInFlight );
        }

        // New results only while what is already recording leaves room in this frame's budget. A recording the
        // ring refused (Starved) is not owed: the slices it waits for are held by the results still queued and by
        // the recordings behind it, which retire them only once their own copies are recorded -- counting it
        // kept them queued, and the ring full, for good.
        uint64_t owed = 0;
        for ( const RecordingUpload& recording : m_Recording )
            if ( !recording.Starved )
                owed += recording.Remaining;
        std::size_t takenCount = 0;
        // The cooks whose results leave this pump: each gives its admission back once, in the commit below.
        std::size_t released = 0;
        if ( owed < m_UploadSettings.BytesPerFrame )
        {
            auto taken = m_Uploads->TakeWithinBudget( m_UploadSettings.BytesPerFrame - owed );
            takenCount = taken.size();
            for ( auto& item : taken )
            {
                // Read before the move: argument evaluation order is unspecified (clang L->R, MSVC R->L).
                const Assets::AssetHandle handle = item.Handle;
                const uint64_t            ticket = item.Ticket;
                if ( !item.Data.Prepared )
                {
                    // The read failed: it settles now (FinishCook drops a stale one) and its admission goes back.
                    FinishCook( handle, ticket, std::move( item.Data ), 0.0 );
                    ++released;
                    continue;
                }
                // A prepared one keeps its admission while it records (RecordingUpload); when it is stale,
                // RecordUploads' shared check drops it in this same pump.
                RecordingUpload recording;
                recording.Handle    = handle;
                recording.Ticket    = ticket;
                recording.Remaining = item.Data.Prepared->Bytes;
                recording.Prepared  = std::move( *item.Data.Prepared );
                m_Recording.push_back( std::move( recording ) );
            }
        }
        released += RecordUploads();

        // THE ADMISSIONS GO BACK EXCLUSIVE, in one section that also starts the cooks they make room for.
        const std::unique_lock table( m_TableLock );
        for ( ; released > 0; --released )
            m_CookAdmission.Release();
        StartAdmittedCooks();
        return takenCount;
    }

    void TextureService::FailRecording( const RecordingUpload& recording, const std::string& error )
    {
        // Settled by name rather than drawn with unwritten levels; FinishCook checks the ticket SHARED (a stale
        // recording is dropped there) and commits the failure EXCLUSIVE. The caller frees its image, unlocked.
        TextureCookOutcome failed;
        failed.Error = error;
        FinishCook( recording.Handle, recording.Ticket, std::move( failed ), recording.RecordMs );
    }

    std::size_t TextureService::RecordUploads()
    {
        if ( m_Recording.empty() )
            return 0;
        // WHICH RECORDINGS STILL BELONG TO THEIR ENTRY'S COOK, asked SHARED in one section. Only the frame loop
        // moves a cooking entry, so the answer holds while the copies are recorded below with the table unlocked.
        {
            const std::shared_lock table( m_TableLock );
            for ( RecordingUpload& recording : m_Recording )
            {
                const auto it = m_Entries.find( recording.Handle );
                recording.Stale =
                     it == m_Entries.end() || !it->second.Cooking || it->second.CookTicket != recording.Ticket;
            }
        }
        // A stale one goes with its image and its ring slice, freed here with the table unlocked.
        std::size_t left =
             std::erase_if( m_Recording, []( const RecordingUpload& recording ) { return recording.Stale; } );
        if ( m_Recording.empty() )
            return left;

        // ONE COMMAND BUFFER FOR EVERY COPY OF THIS PUMP (CPU-STREAM-UPLOAD-RING; UE FRHIResourceUpdateBatcher),
        // queued on the submit thread in order with the frames (GpuBatch::Submit ->
        // RT_SubmitCommandBufferGraphic), so a texture finished below is copied before the first frame that
        // samples it.
        auto begun = Graphic::GpuBatch::Begin();
        if ( !begun.IsSuccess() )
        {
            LOG_ERROR( "[TextureService] no upload batch this frame: {}. {} texture(s) wait for the next one.",
                       begun.GetError(), m_Recording.size() );
            return left;
        }
        std::unique_ptr<Graphic::GpuBatch> batch = begun.ExtractValue();
        uint64_t                           spent = 0;
        m_Finished.clear();
        for ( RecordingUpload& recording : m_Recording )
            recording.InThisBatch = false;
        for ( auto it = m_Recording.begin(); it != m_Recording.end(); )
        {
            if ( spent >= m_UploadSettings.BytesPerFrame )
                break;
            const uint64_t budget   = m_UploadSettings.BytesPerFrame - spent;
            const auto     began    = std::chrono::steady_clock::now();
            auto           step     = it->Prepared.Image->RecordUploadStep( *batch, budget );
            const auto     recorded = std::chrono::steady_clock::now() - began;
            it->RecordMs += std::chrono::duration<double, std::milli>( recorded ).count();
            if ( !step.IsSuccess() )
            {
                RecordingUpload failed = std::move( *it );
                it                     = m_Recording.erase( it );
                FailRecording( failed, step.GetError() );
                ++left;
                continue;
            }
            const Graphic::Image2D::RecordedUploadStep done = step.GetValue();
            if ( done.Bytes == 0 && !done.Complete )
            {
                // THE RING HAS NO ROOM FOR THIS ONE'S LEVELS (they were never staged): it waits, the others go on.
                // Stopping here kept the order and deadlocked the pump, since the recordings behind it hold slices
                // that retire only once their own copies are recorded.
                it->Starved = true;
                ++it;
                continue;
            }
            it->Starved     = false;
            it->InThisBatch = true;
            spent += done.Bytes;
            it->Remaining = it->Remaining > done.Bytes ? it->Remaining - done.Bytes : 0;
            if ( !done.Complete )
                break; // the budget is spent: its next levels are the next frame's
            m_Finished.push_back( std::move( *it ) );
            it = m_Recording.erase( it );
        }
        if ( const auto submitted = batch->Submit(); !submitted.IsSuccess() )
        {
            // NOTHING RECORDED ABOVE WILL RUN, and every image with a step in it now counts levels as copied that
            // never were: each such texture fails by name rather than draw unwritten levels (the device is most
            // likely lost). The recordings this batch did not reach are untouched and stay.
            std::string error = "the upload batch was not submitted: ";
            error += submitted.GetError();
            LOG_ERROR( "[TextureService] {}", error );
            for ( const RecordingUpload& done : m_Finished )
                FailRecording( done, error );
            left += m_Finished.size();
            m_Finished.clear();
            for ( auto it = m_Recording.begin(); it != m_Recording.end(); )
            {
                if ( !it->InThisBatch )
                {
                    ++it;
                    continue;
                }
                RecordingUpload failed = std::move( *it );
                it                     = m_Recording.erase( it );
                FailRecording( failed, error );
                ++left;
            }
            return left;
        }
        m_UploadBatches.push_back( std::move( batch ) );

        // THE TEXTURES WHOSE LAST LEVEL IS IN THE BATCH JUST SUBMITTED: they are drawn from here on (FinishCook:
        // checked shared, made unlocked, committed exclusive), and their cooks' admissions go back (PumpUploads).
        for ( RecordingUpload& done : m_Finished )
        {
            TextureCookOutcome outcome;
            outcome.Prepared = std::move( done.Prepared );
            FinishCook( done.Handle, done.Ticket, std::move( outcome ), done.RecordMs );
        }
        left += m_Finished.size();
        m_Finished.clear();
        return left;
    }

    void TextureService::StartAdmittedCooks()
    {
        // The caller holds the table lock exclusive: SubmitCook only queues a job.
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
        const auto&                     demand   = Graphic::TextureStreamingDemand::Get().TakeFrame();
        // THE POOL (r.Streaming.PoolSize), read before the table lock: the scalability state is not the table's.
        const uint64_t budget = static_cast<uint64_t>( Common::Scalability::QualityState::Resolved().As<uint32_t>(
                                     Common::Scalability::Parameter::TextureStreamingPoolMiB ) ) *
                                1024ull * 1024ull;
        // DECIDED SHARED, COMMITTED EXCLUSIVE ONLY WHERE A ROW MOVES (CPU-ONE-SYNC step 12; UE
        // FRenderAssetStreamingManager: residency and wanted mips are read without blocking readers, the pending
        // changes are listed, one short commit applies them). A render worker's Require hit, Get and AwaitedBy
        // run beside the scan, and a frame with nothing to move takes no exclusive lock. A stream-out moves the
        // live image at the hand-off, after both sections: AfterJoin may run it at once or Join (which waits for
        // workers that may wait for this lock).
        struct Change
        {
            Assets::AssetHandle                   Handle;
            std::shared_ptr<Graphic::Texture2D>   Built;  // the image the scan decided on; the commit re-checks it
            std::shared_ptr<Assets::TextureAsset> Source; // re-checked too; names the log line after the lock
            uint32_t                              From = 0, To = 0;
            bool                                  Down      = false;
            bool                                  Committed = false;
        };
        std::vector<Change> changes;
        // THE SCAN, SHARED. The demand fields are the frame loop's own (this pass alone reads and writes them, no
        // worker touches them), so writing them under the shared lock races no reader.
        // THE HELD PEAK (TEX-STREAM-THRASH): a take is not a whole frame -- it lands while the render task may
        // still note the frame before, so one take can hold only a field's far draws and the next its near ones
        // (grass flipped mip 0 <-> 2 every ~200 ms). A larger ask raises the peak at once; a smaller one replaces
        // it only once the peak went HoldFrames without being asked again.
        std::shared_lock scan( m_TableLock );
        for ( const auto& [asset, pixels] : demand )
        {
            if ( const auto it = m_Entries.find( asset ); it != m_Entries.end() )
            {
                Entry& entry         = it->second;
                entry.LastAskedFrame = m_StreamingFrame;
                if ( pixels >= entry.AskedPixels || m_StreamingFrame - entry.HeldFrame > settings.HoldFrames )
                {
                    entry.AskedPixels = pixels;
                    entry.HeldFrame   = m_StreamingFrame;
                }
            }
        }

        // WANTED MIPS (UE FRenderAssetStreamingManager / FStreamingRenderAsset::GetWantedMips): the smallest level
        // that still gives the screen the texels it covers; the tail for a texture no draw asked for lately.
        struct Want
        {
            Assets::AssetHandle Handle;
            const Entry*        Row   = nullptr;
            uint32_t            First = 0;
            uint32_t            Tail  = 0;
            float               Asked = 0.0f; // the held ask, 0 for a texture unseen past UnseenFrames
        };
        std::vector<Want> wants;
        uint64_t          resident = 0, full = 0, wanted = 0;
        uint32_t          streaming = 0;
        for ( const auto& [handle, entry] : m_Entries )
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
            uint32_t    first = tail;
            const bool  seen  = m_StreamingFrame - entry.LastAskedFrame <= settings.UnseenFrames;
            const float asked = seen ? entry.AskedPixels : 0.0f;
            if ( asked > 0.0f )
            {
                const float    needed = asked * settings.TexelsPerPixel;
                const uint32_t top    = std::max( entry.FullWidth, entry.FullHeight );
                first                 = 0;
                while ( first < tail && static_cast<float>( top >> ( first + 1 ) ) >= needed )
                    ++first;
            }
            resident += BytesFrom( entry.LevelBytes, entry.ResidentFirstMip );
            full += BytesFrom( entry.LevelBytes, 0 );
            wanted += BytesFrom( entry.LevelBytes, first );
            wants.push_back( { handle, &entry, first, tail, asked } );
        }

        // THE POOL FIT: over budget, the least asked-for textures give up a level first, one level per texture
        // per pass, until the wanted set fits (UE drops the lowest-priority mips first).
        const bool overBudget = wanted > budget;
        if ( overBudget )
        {
            std::sort( wants.begin(), wants.end(),
                       []( const Want& a, const Want& b ) { return a.Asked < b.Asked; } );
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

        // LIST THE MOVES. RAISES nearest first: a texture short of its wanted level reads it. DROPS only under
        // pool pressure (UE FRenderAssetStreamingManager keeps unwanted mips while the pool has room): when the
        // resident set plus the raises would not fit the budget, the least asked-for textures holding more than
        // they want give it back, until it fits. A texture no longer wanted is unwanted for the hold window first
        // (the held peak above), and without pressure it keeps its mips, so a texture asked for on every other
        // take is never read and dropped in turn. Nothing is moved here.
        std::sort( wants.begin(), wants.end(), []( const Want& a, const Want& b ) { return a.Asked > b.Asked; } );
        uint64_t projected = resident;
        for ( const Want& want : wants )
        {
            const Entry& entry = *want.Row;
            if ( entry.Cooking || want.First >= entry.ResidentFirstMip )
                continue;
            projected += BytesFrom( entry.LevelBytes, want.First ) -
                         BytesFrom( entry.LevelBytes, entry.ResidentFirstMip );
            changes.push_back(
                 Change{ want.Handle, entry.Built, entry.Source, entry.ResidentFirstMip, want.First, false } );
        }
        for ( auto it = wants.rbegin(); it != wants.rend() && projected > budget; ++it )
        {
            const Entry& entry = *it->Row;
            if ( entry.Cooking || it->First <= entry.ResidentFirstMip )
                continue;
            projected -= BytesFrom( entry.LevelBytes, entry.ResidentFirstMip ) -
                         BytesFrom( entry.LevelBytes, it->First );
            changes.push_back(
                 Change{ it->Handle, entry.Built, entry.Source, entry.ResidentFirstMip, it->First, true } );
        }
        const std::size_t textures = wants.size();
        wants.clear(); // its rows are the table's: not read past the shared lock
        scan.unlock();

        // THE COMMIT, EXCLUSIVE, only when a row moves. Each row is re-checked: one cleared, given another image
        // or shell, cooking or moved since the scan is left for the next frame's pass to decide again. Lock
        // order unchanged: a raise's BeginCook reaches only the admission and the job queue (a leaf).
        if ( !changes.empty() )
        {
            const std::unique_lock commit( m_TableLock );
            for ( Change& change : changes )
            {
                const auto it = m_Entries.find( change.Handle );
                if ( it == m_Entries.end() )
                    continue;
                Entry& entry = it->second;
                if ( entry.Cooking || entry.Built != change.Built || entry.Source != change.Source ||
                     entry.ResidentFirstMip != change.From )
                    continue;
                change.Committed = true;
                if ( change.Down )
                {
                    entry.ResidentFirstMip = entry.RequestedFirstMip = change.To;
                    resident -=
                         BytesFrom( entry.LevelBytes, change.From ) - BytesFrom( entry.LevelBytes, change.To );
                    continue;
                }
                entry.RequestedFirstMip = change.To;
                BeginCook( change.Handle, entry );
                ++streaming;
            }
        }

        for ( const Change& change : changes )
        {
            if ( !change.Committed )
                continue;
            const std::string name = change.Source->GetMetadata().Filepath.filename().string();
            if ( change.Down )
            {
                // A DROP READS NOTHING (UE FTexture2DStreamOut): the kept levels are copied on the GPU into a
                // smaller image the same Image2D takes; the .detex is read only to raise a texture again.
                // It moves the live image (a layout transition and the swap), so it runs at the hand-off like
                // a raise's swap (OVERLAP-ON-7): at once when nothing is in flight, else after the next Join.
                StreamOutAtHandOff( change.Built, change.To - change.From );
                LOG_INFO( "[TextureStreaming] '{}' streamed out mip {} -> {} by a GPU copy (no read)", name,
                          change.From, change.To );
            }
            else
                LOG_INFO( "[TextureStreaming] '{}' reads mip {} -> {} from its asset", name, change.From,
                          change.To );
        }

        auto& pool = Graphic::TextureStreamingDemand::Get().Pool();
        pool.ResidentBytes.store( resident, std::memory_order_relaxed );
        pool.WantedBytes.store( wanted, std::memory_order_relaxed );
        pool.BudgetBytes.store( budget, std::memory_order_relaxed );
        pool.FullChainBytes.store( full, std::memory_order_relaxed );
        pool.Textures.store( static_cast<uint32_t>( textures ), std::memory_order_relaxed );
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
                 static_cast<double>( wanted ) / MB, static_cast<double>( full ) / MB, textures, streaming );
        }
    }

    std::size_t TextureService::PendingUploads() const
    {
        return m_Uploads->Pending();
    }

    std::size_t TextureService::InFlight() const
    {
        const std::shared_lock table( m_TableLock );
        return static_cast<std::size_t>( std::count_if( m_Entries.begin(), m_Entries.end(), []( const auto& row )
                                                        { return row.second.Reading || row.second.Cooking; } ) );
    }

    void TextureService::Clear()
    {
        // The rows leave the table under the lock and are destroyed after it: their GPU textures are freed (and
        // their requests released) with the table unlocked. So are the results already queued and the recordings
        // (CPU-STREAM-UPLOAD-RING: each holds the device image its worker made), whose slots go back under the
        // lock; the cooks already on a worker keep theirs until their (now stale) results are taken.
        std::unordered_map<Assets::AssetHandle, Entry> dropped;
        auto                                           queued = m_Uploads->TakeWithinBudget( UINT64_MAX );
        {
            const std::unique_lock table( m_TableLock );
            for ( std::size_t released = queued.size() + m_Recording.size(); released > 0; --released )
                m_CookAdmission.Release();
            m_CookAdmission.DropWaiting();
            dropped.swap( m_Entries );
            m_ReportedMissing.clear();
            m_Waiters.Clear();
            m_AnyWaiting.store( false, std::memory_order_release );
        }
        // Frame loop only, so nothing joined these since their admissions went back. The batches keep what their
        // copies read until the GPU has finished (GpuBatch::Retain), waiting for it as they go.
        m_Recording.clear();
        m_Finished.clear();
        m_UploadBatches.clear();
        queued.clear();
        m_Retiring.Clear();
    }
} // namespace Desert::Runtime
