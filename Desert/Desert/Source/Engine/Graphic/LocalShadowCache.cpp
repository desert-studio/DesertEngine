#include <Engine/Graphic/LocalShadowCache.hpp>

#include <Engine/Core/FrameManager.hpp>
#include <Engine/Graphic/Framebuffer.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>

namespace Desert::Graphic
{
    LocalShadowCache::LocalShadowCache( const ResourceOwnerInstance scene ) : m_LedgerInstance( scene ) {}
    LocalShadowCache::~LocalShadowCache() = default;

    uint32_t LocalShadowCache::BeginView( const uint32_t viewSide, const bool primary )
    {
        const uint64_t frame = Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
        if ( frame != m_Frame )
        {
            // LAST FRAME'S VERDICTS, at the frame boundary: no view of this frame has recorded a pass against the
            // atlas yet, so it may be made again here and nowhere later in the frame.
            if ( m_Atlas && m_PrimarySideAsked != 0 && m_PrimarySideAsked != m_Allocator.Side() )
            {
                LOG_INFO( "[Shadows] the scene's local-light shadow atlas goes {} -> {}: the viewports' shadow "
                          "budget changed; every light's shadow is drawn again.",
                          m_Allocator.Side(), m_PrimarySideAsked );
                // The atlas may still be read by a frame in flight (the same rule as
                // MeshRenderer::RebudgetShadows).
                Renderer::GetInstance().WaitDeviceIdle();
                Create( m_PrimarySideAsked );
            }
            // A light no view asked for last frame is gone (or out of every budget): its tiles go back.
            for ( auto it = m_Lights.begin(); it != m_Lights.end(); )
            {
                if ( it->second.LastFrame != m_Frame )
                {
                    for ( uint32_t v = 0; v < it->second.ViewCount; ++v )
                        m_Allocator.Free( it->second.Tiles[v] );
                    it = m_Lights.erase( it );
                    continue;
                }
                ++it;
            }
            m_Frame            = frame;
            m_PrimarySideAsked = 0;
        }
        if ( primary )
            m_PrimarySideAsked = std::max( m_PrimarySideAsked, viewSide );
        if ( !m_Atlas && viewSide >= kMinLocalShadowTile )
            Create( viewSide );
        return m_Allocator.Side();
    }

    void LocalShadowCache::Create( const uint32_t side )
    {
        // THE SCENE'S ROWS. Create runs inside the asking view's ledger scope (BeginView from the view's
        // UpdateLocalShadows), so without this the atlas carried that view's instance and outlived its close as
        // the view's rows. Nested: the view's scope is restored when this one closes.
        const ResourceAttributionScope sceneRows( ResourceOwner::SceneRenderer, m_LedgerInstance );
        m_Lights.clear();
        FramebufferSpecification spec;
        spec.DebugName = "LocalShadowAtlas";
        // Depth only, like the cascades: the receivers sample the D32F attachment itself.
        spec.Attachments.Attachments.emplace_back( ViewTargetFormats::kShadowDepth );
        m_Atlas = Framebuffer::Create( spec );
        m_Atlas->Resize( side, side );
        m_Allocator.Reset( side );
        LOG_INFO( "[Shadows] the scene's local-light shadow atlas {}x{}: {} MiB (D32F depth)", side, side,
                  Core::Formats::CalculateImageSize( side, side, ViewTargetFormats::kShadowDepth ) /
                       ( 1024ull * 1024ull ) );
    }

    LocalShadowCache::Light* LocalShadowCache::Find( const uint64_t key )
    {
        const auto it = m_Lights.find( key );
        return it != m_Lights.end() ? &it->second : nullptr;
    }

    LocalShadowCache::Light* LocalShadowCache::Allocate( const uint64_t key, const uint32_t askedSize,
                                                         const uint32_t viewCount )
    {
        Release( key );
        Light light;
        for ( uint32_t size = askedSize; size >= kMinLocalShadowTile && light.ViewCount == 0; size /= 2u )
        {
            std::array<AtlasTile, kPointShadowFaces> tiles{};
            uint32_t                                 got = 0;
            for ( ; got < viewCount; ++got )
            {
                const auto tile = m_Allocator.Allocate( size );
                if ( !tile )
                    break;
                tiles[got] = *tile;
            }
            if ( got == viewCount )
            {
                light.ViewCount = viewCount;
                light.Tiles     = tiles;
            }
            else
            {
                for ( uint32_t v = 0; v < got; ++v )
                    m_Allocator.Free( tiles[v] );
            }
        }
        if ( light.ViewCount == 0 )
            return nullptr;
        // Kept under the size it ASKED for (its tiles may be smaller, when the pool was short), so the same
        // request next frame finds it rather than resizing it.
        light.AskedSize = askedSize;
        return &m_Lights.insert_or_assign( key, light ).first->second;
    }

    void LocalShadowCache::Release( const uint64_t key )
    {
        const auto it = m_Lights.find( key );
        if ( it == m_Lights.end() )
            return;
        for ( uint32_t v = 0; v < it->second.ViewCount; ++v )
            m_Allocator.Free( it->second.Tiles[v] );
        m_Lights.erase( it );
    }

    std::shared_ptr<Image2D> LocalShadowCache::GetAtlasImage() const
    {
        return m_Atlas ? m_Atlas->GetDepthAttachmentImage() : nullptr;
    }
} // namespace Desert::Graphic
