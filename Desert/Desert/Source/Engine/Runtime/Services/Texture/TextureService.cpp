#include "TextureService.hpp"

#include <Engine/Assets/RegistryDiscovery.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Graphic/TextureFactory.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/JobSystem.hpp>
#include <Common/Core/Logger.hpp>

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
        struct CookJob
        {
            Assets::AssetHandle             Handle;
            uint64_t                        Ticket = 0;
            std::filesystem::path           Asset;
            std::shared_ptr<TextureUploads> Uploads;
        };
        auto job = std::make_shared<CookJob>(
             CookJob{ handle, entry.CookTicket, entry.Source->GetMetadata().Filepath, m_Uploads } );
        Common::JobSystem::Get().Submit(
             [job]
             {
                 TextureCookOutcome outcome;
                 const auto         began  = std::chrono::steady_clock::now();
                 auto               cooked = Graphic::Texture2D::ReadCooked( job->Asset );
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
        if ( outcome.Cooked )
        {
            const auto     began = std::chrono::steady_clock::now();
            const uint64_t bytes = outcome.Cooked->UploadBytes();
            auto           built = Graphic::Texture2D::CreateFromCooked( std::move( *outcome.Cooked ) );
            if ( built.IsSuccess() )
                entry.Built = built.ExtractValue();
            else
                outcome.Error = built.GetError();
            LOG_DEBUG(
                 "[TextureService] '{}' uploaded on the main thread in {:.1f} ms ({} bytes)",
                 entry.Source->GetMetadata().Filepath.filename().string(),
                 std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - began ).count(),
                 bytes );
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
        return taken.size();
    }

    std::size_t TextureService::PendingUploads() const
    {
        return m_Uploads->Pending();
    }

    void TextureService::Clear()
    {
        m_Uploads->Clear();
        m_Retiring.Clear();
        m_Entries.clear();
        m_ReportedMissing.clear();
        m_Waiters.Clear();
    }
} // namespace Desert::Runtime
