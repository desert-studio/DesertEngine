#include "TextureService.hpp"

#include <Engine/Assets/RegistryDiscovery.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Graphic/TextureFactory.hpp>
#include <Engine/Graphic/TextureStreamingDemand.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/JobSystem.hpp>
#include <Common/Core/Logger.hpp>
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
             [job]
             {
                 TextureCookOutcome outcome;
                 const auto         began  = std::chrono::steady_clock::now();
                 auto cooked = Graphic::Texture2D::ReadCookedMips( job->Asset, job->FirstMip, job->TailSize );
                 LOG_DEBUG( "[TextureService] '{}' platform data read on a worker in {:.1f} ms",
                            job->Asset.filename().string(),
                            std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - began )
                                 .count() );
                 uint64_t bytes = 0;
                 if ( cooked.IsSuccess() )
                 {
                     outcome.Cooked = cooked.ExtractValue();
                     bytes          = outcome.Cooked->UploadBytes();
                 }
                 else
                     outcome.Error = cooked.GetError();
                 job->Uploads->Push( { job->Handle, job->Ticket, bytes, std::move( outcome ) } );
             } );
    }

    void TextureService::FinishCook( const Assets::AssetHandle& handle, Entry& entry, TextureCookOutcome outcome )
    {
        entry.Cooking = false;
        // A STREAMING READ (TEX-STREAMING) finishes over a texture that is already drawn: its new levels go INTO
        // the same texture (Texture2D::ReplaceLevels -- the Image2D every material slot and override points at
        // keeps its address and bindless slot, UE's RHIUpdateTextureReference), so no holder is rebuilt or left
        // with a released image. A read that fails leaves the resident levels drawing.
        const bool streamed = entry.Built != nullptr;
        bool       replaced = false;
        if ( outcome.Cooked )
        {
            const auto     began    = std::chrono::steady_clock::now();
            const uint64_t bytes    = outcome.Cooked->UploadBytes();
            const uint32_t firstMip = outcome.Cooked->FirstMip;
            entry.FullWidth         = outcome.Cooked->FullWidth;
            entry.FullHeight        = outcome.Cooked->FullHeight;
            entry.LevelBytes        = outcome.Cooked->FullLevelBytes;
            if ( streamed )
            {
                const auto swapped = entry.Built->ReplaceLevels( std::move( *outcome.Cooked ) );
                replaced           = swapped.IsSuccess();
                if ( !replaced )
                    outcome.Error = swapped.GetError();
            }
            else
            {
                auto built = Graphic::Texture2D::CreateFromCooked( std::move( *outcome.Cooked ) );
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
                 std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - began ).count(),
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

        m_Waiters.Settle( handle, []( const Assets::AssetHandle& material )
                          { ResourceRegistry::GetMaterialService()->Invalidate( material ); } );
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
                 m_Waiters.Settle( handle, []( const Assets::AssetHandle& material )
                                   { ResourceRegistry::GetMaterialService()->Invalidate( material ); } );
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

    std::size_t TextureService::PumpUploads()
    {
        auto taken = m_Uploads->TakeWithinBudget( m_UploadSettings.BytesPerFrame );
        for ( auto& item : taken )
        {
            const auto it = m_Entries.find( item.Handle );
            // Stale: the entry was cleared, or a later cook replaced the one this result belongs to.
            if ( it == m_Entries.end() || !it->second.Cooking || it->second.CookTicket != item.Ticket )
                continue;
            // Read before the move: argument evaluation order is unspecified (clang L->R, MSVC R->L).
            const Assets::AssetHandle handle = item.Handle;
            FinishCook( handle, it->second, std::move( item.Data ) );
        }
        // Every taken result, stale ones included, was one admitted cook: its decoded pixels are gone now.
        for ( std::size_t released = 0; released < taken.size(); ++released )
            m_CookAdmission.Release();
        StartAdmittedCooks();
        return taken.size();
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
