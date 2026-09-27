#include "TextureService.hpp"

#include <Engine/Assets/RegistryDiscovery.hpp>
#include <Engine/Graphic/TextureFactory.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/Logger.hpp>

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

    void TextureService::Build( const Assets::AssetHandle& handle, Entry& entry )
    {
        entry.Built = Graphic::TextureFactory::Create2D( entry.Source );
        if ( !entry.Built )
        {
            entry.Failed = true;
            LOG_ERROR( "[TextureService] '{}' was read but no GPU texture could be built from it",
                       entry.Source->GetMetadata().Filepath.string() );
            return;
        }
        ClaimTextureImage( entry.Built, handle );
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
                 if ( outcome != Assets::LoadOutcome::Loaded )
                 {
                     it->second.Failed = true;
                     LOG_ERROR( "[TextureService] '{}' could not be read: {}. Materials naming it sample the "
                                "slot's schema default; it is not retried.",
                                it->second.Source->GetMetadata().Filepath.string(), error );
                 }
                 else
                     Build( handle, it->second );

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
        if ( entry->Request.IsValid() )
            return Assets::AssetRef<Graphic::Texture2D>::Pending( handle );

        // Already read (an importer handed over a loaded asset): build now, no request needed.
        if ( entry->Source->IsReadyForUse() )
        {
            Build( handle, *entry );
            return entry->Built ? Assets::AssetRef<Graphic::Texture2D>::Ready( handle, entry->Built )
                                : Assets::AssetRef<Graphic::Texture2D>::Null();
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
        if ( const auto it = m_Entries.find( texture ); it != m_Entries.end() && it->second.Request.IsValid() )
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

    void TextureService::Clear()
    {
        m_Entries.clear();
        m_ReportedMissing.clear();
        m_Waiters.Clear();
    }
} // namespace Desert::Runtime
