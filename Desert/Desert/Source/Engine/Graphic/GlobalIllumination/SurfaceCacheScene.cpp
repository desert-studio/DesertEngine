#include <Engine/Graphic/GlobalIllumination/SurfaceCacheScene.hpp>

#include <Engine/Core/Projection.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <bit>
#include <cmath>

namespace Desert::Graphic::SurfaceCache
{
    namespace
    {
        // UE GetCardMaxResolution clamps the viewer distance to a metre: a card the camera stands inside is not
        // asked for infinite texels.
        constexpr float kMinViewerDistance = 100.0f; // cm

        [[nodiscard]] uint32_t Pow2Ceil( const uint32_t value )
        {
            return std::bit_ceil( std::max( value, 1u ) );
        }

        [[nodiscard]] uint32_t Log2( const uint32_t pow2 )
        {
            return static_cast<uint32_t>( std::countr_zero( pow2 ) );
        }

        // Pages along a card side and the physical size of each: below a page one element of that size, from a
        // page up kVirtualPageSize-strided pages of kPhysicalPageSize (neighbours share their border texel).
        [[nodiscard]] std::pair<uint32_t, uint32_t> PageLayout( const uint32_t resolution )
        {
            if ( resolution < kPhysicalPageSize )
                return { 1u, resolution };
            return { PagesAlongSide( resolution ), kPhysicalPageSize };
        }
    } // namespace

    // ---- Allocator ----

    void SurfaceCacheAllocator::Reset( const uint32_t atlasSize )
    {
        m_PagesPerSide = AtlasPagesPerSide( atlasSize );
        m_Bins.clear();
        m_FreePages.clear();
        const uint32_t pages = m_PagesPerSide * m_PagesPerSide;
        m_FreePages.reserve( pages );
        for ( uint32_t page = pages; page-- > 0; )
            m_FreePages.push_back( page );
    }

    std::optional<AtlasRect> SurfaceCacheAllocator::Allocate( const uint32_t width, const uint32_t height )
    {
        const auto pageOrigin = [this]( const uint32_t page ) {
            return glm::uvec2( page % m_PagesPerSide * kPhysicalPageSize,
                               page / m_PagesPerSide * kPhysicalPageSize );
        };
        if ( width >= kPhysicalPageSize && height >= kPhysicalPageSize )
        {
            if ( m_FreePages.empty() )
                return std::nullopt;
            const uint32_t page = m_FreePages.back();
            m_FreePages.pop_back();
            const glm::uvec2 origin = pageOrigin( page );
            return AtlasRect{ origin.x, origin.y, kPhysicalPageSize, kPhysicalPageSize };
        }
        // A bin of this element size with room, or a new one cut out of a free page.
        auto& bins = m_Bins[{ width, height }];
        auto  bin =
             std::find_if( bins.begin(), bins.end(), []( const Bin& b ) { return !b.FreeElements.empty(); } );
        if ( bin == bins.end() )
        {
            if ( m_FreePages.empty() )
                return std::nullopt;
            Bin fresh;
            fresh.Page = m_FreePages.back();
            m_FreePages.pop_back();
            const uint32_t elements = ( kPhysicalPageSize / width ) * ( kPhysicalPageSize / height );
            for ( uint32_t e = elements; e-- > 0; )
                fresh.FreeElements.push_back( e );
            bins.push_back( std::move( fresh ) );
            bin = std::prev( bins.end() );
        }
        const uint32_t element = bin->FreeElements.back();
        bin->FreeElements.pop_back();
        const uint32_t   perRow = kPhysicalPageSize / width;
        const glm::uvec2 origin = pageOrigin( bin->Page );
        return AtlasRect{ origin.x + element % perRow * width, origin.y + element / perRow * height, width,
                          height };
    }

    void SurfaceCacheAllocator::Free( const AtlasRect& rect )
    {
        const uint32_t page = rect.Y / kPhysicalPageSize * m_PagesPerSide + rect.X / kPhysicalPageSize;
        if ( rect.Width >= kPhysicalPageSize && rect.Height >= kPhysicalPageSize )
        {
            m_FreePages.push_back( page );
            return;
        }
        auto& bins = m_Bins[{ rect.Width, rect.Height }];
        auto  bin  = std::find_if( bins.begin(), bins.end(), [page]( const Bin& b ) { return b.Page == page; } );
        if ( bin == bins.end() )
            return;
        const uint32_t perRow = kPhysicalPageSize / rect.Width;
        const uint32_t local =
             ( rect.Y % kPhysicalPageSize ) / rect.Height * perRow + ( rect.X % kPhysicalPageSize ) / rect.Width;
        bin->FreeElements.push_back( local );
        // A bin with every element back returns its page.
        if ( bin->FreeElements.size() == ( kPhysicalPageSize / rect.Width ) * ( kPhysicalPageSize / rect.Height ) )
        {
            m_FreePages.push_back( bin->Page );
            bins.erase( bin );
        }
    }

    // ---- Card frames ----

    void CardWorldFrame( const Assets::CardBuildData& card, const glm::mat4& world, glm::mat3& axes,
                         glm::vec3& center, glm::vec3& extent )
    {
        const uint32_t direction = static_cast<uint32_t>( card.Direction );
        const int      k         = static_cast<int>( direction / 2u );
        const float    sign      = direction % 2u == 1u ? 1.0f : -1.0f;
        // Cyclic order keeps e(k+1) x e(k+2) = e(k); a negative direction swaps the two, so X x Y = Z stays.
        int ix = ( k + 1 ) % 3;
        int iy = ( k + 2 ) % 3;
        if ( sign < 0.0f )
            std::swap( ix, iy );
        const glm::mat3 basis( world );
        const float     lx = glm::length( basis[ix] );
        const float     ly = glm::length( basis[iy] );
        const float     lz = glm::length( basis[k] );
        glm::vec3       x  = lx > 0.0f ? basis[ix] / lx : glm::vec3( 1.0f, 0.0f, 0.0f );
        const glm::vec3 y  = ly > 0.0f ? basis[iy] / ly : glm::vec3( 0.0f, 1.0f, 0.0f );
        const glm::vec3 z  = sign * ( lz > 0.0f ? basis[k] / lz : glm::vec3( 0.0f, 0.0f, 1.0f ) );
        // A mirroring world (negative determinant) flips the handedness: X turns round so X x Y = Z again.
        if ( glm::dot( glm::cross( x, y ), z ) < 0.0f )
            x = -x;
        axes   = glm::mat3( x, y, z );
        center = glm::vec3( world * glm::vec4( card.Center, 1.0f ) );
        extent = glm::vec3( card.Extent[ix] * lx, card.Extent[iy] * ly, card.Extent[k] * lz );
    }

    // ---- Scene ----

    void SurfaceCacheScene::Configure( const SurfaceCacheBudget& budget, const uint32_t captureAtlasSize,
                                       const uint32_t maxCapturesPerFrame )
    {
        const bool atlasChanged = budget.AtlasSize != m_Budget.AtlasSize || captureAtlasSize != m_CaptureAtlasSize;
        m_Budget                = budget;
        m_CaptureAtlasSize      = captureAtlasSize;
        m_MaxCapturesPerFrame   = maxCapturesPerFrame;
        if ( !atlasChanged )
            return;
        m_Allocator.Reset( budget.AtlasSize );
        for ( auto& [key, instance] : m_Instances )
            for ( Card& card : instance.Cards )
            {
                card.Pages.clear();
                card.ResX = card.ResY = card.PagesX = card.PagesY = 0;
                card.Captured                                     = false;
                card.CaptureFrame                                 = 0;
            }
    }

    void SurfaceCacheScene::PlaceCards( Instance& instance ) const
    {
        if ( instance.Cards.size() != instance.Build->Cards.size() )
            instance.Cards.resize( instance.Build->Cards.size() );
        for ( std::size_t i = 0; i < instance.Cards.size(); ++i )
        {
            Card& card = instance.Cards[i];
            card.Build = instance.Build->Cards[i];
            CardWorldFrame( card.Build, instance.World, card.Axes, card.Center, card.Extent );
        }
    }

    void SurfaceCacheScene::FreeCard( Card& card )
    {
        for ( const AtlasRect& page : card.Pages )
            m_Allocator.Free( page );
        card.Pages.clear();
        card.ResX = card.ResY = card.PagesX = card.PagesY = 0;
        card.Captured                                     = false;
        card.CaptureFrame                                 = 0;
    }

    bool SurfaceCacheScene::AllocateCard( Card& card, const uint32_t resX, const uint32_t resY )
    {
        const auto [pagesX, sizeX] = PageLayout( resX );
        const auto [pagesY, sizeY] = PageLayout( resY );
        std::vector<AtlasRect> pages;
        pages.reserve( pagesX * pagesY );
        for ( uint32_t i = 0; i < pagesX * pagesY; ++i )
        {
            const auto rect = m_Allocator.Allocate( sizeX, sizeY );
            if ( !rect )
            {
                for ( const AtlasRect& taken : pages )
                    m_Allocator.Free( taken );
                return false;
            }
            pages.push_back( *rect );
        }
        card.Pages    = std::move( pages );
        card.ResX     = resX;
        card.ResY     = resY;
        card.PagesX   = pagesX;
        card.PagesY   = pagesY;
        card.Captured = false;
        card.CaptureFrame = 0;
        return true;
    }

    uint32_t SurfaceCacheScene::WantedLongSide( const Card& card ) const
    {
        // UE GetCardMaxResolution: texel density x the card's world size over the viewer's distance to it.
        const float longSide = 2.0f * std::max( card.Extent.x, card.Extent.y );
        const float texels   = static_cast<float>( m_Budget.CardTexelDensityPercent ) * longSide /
                             std::max( card.Distance, kMinViewerDistance );
        return static_cast<uint32_t>( std::max( texels, 1.0f ) );
    }

    void SurfaceCacheScene::ConfirmCapturesExecuted( const uint64_t frame )
    {
        const std::lock_guard lock( m_ExecutedMutex );
        m_ExecutedCaptureFrames.push_back( frame );
    }

    SceneFrame SurfaceCacheScene::Update( const std::vector<SceneInstance>& instances, const glm::vec3& viewer )
    {
        ++m_Frame;
        SceneFrame frame;
        if ( m_Budget.AtlasSize < kPhysicalPageSize || m_CaptureAtlasSize == 0 )
            return frame;

        // ADD / MOVE. A key that comes back with another mesh or another card build is a new instance.
        for ( const SceneInstance& in : instances )
        {
            if ( !in.Cards || in.Cards->Cards.empty() )
                continue;
            auto it = m_Instances.find( in.Key );
            if ( it != m_Instances.end() && ( it->second.Mesh != in.Mesh || it->second.Build != in.Cards ) )
            {
                for ( Card& card : it->second.Cards )
                    FreeCard( card );
                m_Instances.erase( it );
                it = m_Instances.end();
            }
            if ( it == m_Instances.end() )
            {
                Instance fresh;
                fresh.Mesh  = in.Mesh;
                fresh.Build = in.Cards;
                fresh.World      = in.World;
                fresh.SurfaceKey = in.SurfaceKey;
                PlaceCards( fresh );
                it = m_Instances.emplace( in.Key, std::move( fresh ) ).first;
            }
            else if ( it->second.World != in.World )
            {
                // Moved: the captured texels are card space and stay; the card frames follow the world.
                it->second.World = in.World;
                PlaceCards( it->second );
            }
            if ( it->second.SurfaceKey != in.SurfaceKey )
            {
                // Another surface: the texels hold the old material; the pages stay, the cards are recaptured.
                it->second.SurfaceKey = in.SurfaceKey;
                for ( Card& card : it->second.Cards )
                    card.Captured = false;
                card.CaptureFrame = 0;
            }
            for ( Card& card : it->second.Cards )
                card.Ready = in.CaptureReady;
            it->second.Source    = in.Source;
            it->second.SeenFrame = m_Frame;
        }
        // REMOVE: every instance the frame no longer has gives its pages back.
        for ( auto it = m_Instances.begin(); it != m_Instances.end(); )
        {
            if ( it->second.SeenFrame == m_Frame )
            {
                ++it;
                continue;
            }
            for ( Card& card : it->second.Cards )
                FreeCard( card );
            it = m_Instances.erase( it );
        }

        // RESOLUTIONS. Each side a power of two, at least the smallest card side - a thin card's thin side
        // included (a kerb keeps its eight texels across) - at most the largest and the capture atlas.
        const uint32_t minRes =
             Pow2Ceil( std::max( kMinCardResolution, std::min( m_Budget.CardMinResolution, kPhysicalPageSize ) ) );
        const uint32_t maxRes = std::max(
             minRes, std::bit_floor( std::max( 1u, std::min( { m_Budget.CardMaxResolution, m_CaptureAtlasSize,
                                                               ResLevelResolution( kMaxResLevel ) } ) ) ) );
        std::vector<Card*> cards;
        for ( auto& [key, instance] : m_Instances )
            for ( Card& card : instance.Cards )
            {
                const glm::vec3 local = glm::transpose( card.Axes ) * ( viewer - card.Center );
                card.Distance = glm::length( glm::max( glm::abs( local ) - card.Extent, glm::vec3( 0.0f ) ) );
                const float    longest = std::max( std::max( card.Extent.x, card.Extent.y ), 1e-3f );
                const uint32_t want    = WantedLongSide( card );
                const auto     side    = [&]( const float extent )
                {
                    const float texels = static_cast<float>( want ) * extent / longest;
                    return std::clamp( Pow2Ceil( static_cast<uint32_t>( std::ceil( texels ) ) ), minRes, maxRes );
                };
                card.WantX = side( card.Extent.x );
                card.WantY = side( card.Extent.y );
                cards.push_back( &card );
            }
        std::stable_sort( cards.begin(), cards.end(),
                          []( const Card* a, const Card* b ) { return a->Distance < b->Distance; } );

        // ALLOCATION, nearest first. A resident card is reallocated when it wants more texels, or a quarter of
        // what it holds on both sides (hysteresis: a camera wobbling at a level boundary does not recapture).
        for ( std::size_t i = 0; i < cards.size(); ++i )
        {
            Card&      card     = *cards[i];
            const bool resident = card.ResX != 0;
            if ( resident && card.WantX <= card.ResX && card.WantY <= card.ResY &&
                 !( card.WantX * 4u <= card.ResX && card.WantY * 4u <= card.ResY ) )
                continue;
            FreeCard( card );
            std::size_t victim = cards.size();
            for ( ;; )
            {
                uint32_t resX = card.WantX, resY = card.WantY;
                bool     placed = false;
                for ( ;; )
                {
                    if ( AllocateCard( card, resX, resY ) )
                    {
                        placed = true;
                        break;
                    }
                    if ( resX <= minRes && resY <= minRes )
                        break;
                    resX = std::max( minRes, resX / 2u );
                    resY = std::max( minRes, resY / 2u );
                }
                if ( placed )
                    break;
                // The atlas is full: the farthest card holding pages pays, if it is farther than this one.
                while ( victim > i + 1 && cards[victim - 1]->ResX == 0 )
                    --victim;
                if ( victim <= i + 1 )
                    break;
                FreeCard( *cards[--victim] );
            }
        }

        // CONFIRMED CAPTURES (GI-SURFACE-CACHE-4). A capture becomes the card's surface only once the graph that
        // drew it EXECUTED (ConfirmCapturesExecuted, from the copy pass's exec): a frame whose graph was dropped
        // (a pass refused at build) never ran its copy, and its cards are captured again rather than shown empty
        // for ever. A capture that no execution confirms within kCaptureConfirmFrames is taken as dropped.
        {
            std::vector<uint64_t> executed;
            {
                const std::lock_guard lock( m_ExecutedMutex );
                executed.swap( m_ExecutedCaptureFrames );
            }
            for ( auto& [key, instance] : m_Instances )
                for ( Card& card : instance.Cards )
                {
                    if ( card.CaptureFrame == 0 )
                        continue;
                    if ( std::find( executed.begin(), executed.end(), card.CaptureFrame ) != executed.end() )
                    {
                        card.Captured     = true;
                        card.CaptureFrame = 0;
                    }
                    else if ( card.CaptureFrame + kCaptureConfirmFrames < m_Frame )
                        card.CaptureFrame = 0;
                }
        }

        // CAPTURES, nearest first, shelf-packed into the capture atlas (rows of the tallest card in them).
        uint32_t                        shelfX = 0, shelfY = 0, shelfHeight = 0;
        std::map<const Card*, uint32_t> captured; // card -> index in frame.Captures
        for ( Card* card : cards )
        {
            if ( frame.Captures.size() >= m_MaxCapturesPerFrame )
                break;
            if ( card->ResX == 0 || card->Captured || card->CaptureFrame != 0 || !card->Ready )
                continue;
            if ( shelfX + card->ResX > m_CaptureAtlasSize )
            {
                shelfX = 0;
                shelfY += shelfHeight;
                shelfHeight = 0;
            }
            if ( shelfY + card->ResY > m_CaptureAtlasSize )
                continue; // the next frame's atlas
            CardCapture capture;
            capture.CaptureRect = { shelfX, shelfY, card->ResX, card->ResY };
            shelfX += card->ResX;
            shelfHeight = std::max( shelfHeight, card->ResY );
            // The card camera: on the card plane (the face the card direction points out of), looking back
            // into the box, X right and Y up, the box's depth its depth range.
            const glm::vec3 y = card->Axes[1], z = card->Axes[2];
            capture.Eye        = card->Center + z * card->Extent.z;
            capture.View       = glm::lookAt( capture.Eye, capture.Eye - z, y );
            capture.Projection = Core::MakeOrthographic( -card->Extent.x, card->Extent.x, -card->Extent.y, card->Extent.y,
                                                  0.0f, 2.0f * card->Extent.z );
            captured.emplace( card, static_cast<uint32_t>( frame.Captures.size() ) );
            frame.Captures.push_back( capture );
            card->CaptureFrame = m_Frame;
        }

        // THE GPU RECORDS (compact, this frame's order) and the page copies of this frame's captures.
        const float    atlas          = static_cast<float>( m_Budget.AtlasSize );
        const uint64_t lightingFactor = std::max( m_Budget.DirectLightingUpdateFactor, 1u );
        frame.Frame                   = m_Frame;
        for ( auto& [key, instance] : m_Instances )
        {
            const uint32_t   meshIndex = static_cast<uint32_t>( frame.MeshCards.size() );
            MeshCardsPayload mesh;
            const glm::mat3  basis( instance.World );
            for ( int r = 0; r < 3; ++r )
            {
                const float     length = glm::length( basis[r] );
                const glm::vec3 axis   = length > 0.0f ? basis[r] / length : glm::vec3( 0.0f );
                mesh.WorldToLocal[r]   = glm::vec4( axis, instance.World[3][r] );
            }
            mesh.Cards = glm::uvec4( static_cast<uint32_t>( frame.Cards.size() ),
                                     static_cast<uint32_t>( instance.Cards.size() ),
                                     instance.Build->MostlyTwoSided ? 2u : 0u, 0u );
            for ( uint32_t c = 0; c < instance.Cards.size(); ++c )
            {
                Card&          card      = instance.Cards[c];
                const uint32_t direction = static_cast<uint32_t>( card.Build.Direction );
                mesh.CardLookup[direction / 4u][static_cast<int>( direction % 4u )] |= 1u << c;

                const uint32_t cardIndex = static_cast<uint32_t>( frame.Cards.size() );
                CardPayload    payload;
                for ( int r = 0; r < 3; ++r )
                    payload.WorldToLocal[r] = glm::vec4( card.Axes[r], card.Center[r] );
                const bool resident = card.ResX != 0;
                payload.LocalExtent = glm::vec4( card.Extent, resident ? 2.0f * card.Extent.x / card.ResX : 0.0f );
                payload.Pages       = glm::uvec4( card.PagesX | ( card.PagesY << 16u ),
                                                  static_cast<uint32_t>( frame.Pages.size() ), 0u, 0u );
                // Shown (and lit) once captured, or while this frame's graph captures it: the same graph copies
                // the capture into the atlas before any reader, and a dropped graph drops its readers with it.
                const bool capturing = card.CaptureFrame == m_Frame;
                const bool visible   = resident && ( card.Captured || capturing );
                payload.Info =
                     glm::uvec4( ( resident ? Log2( card.ResX ) | ( Log2( card.ResY ) << 8u ) : 0u ) |
                                      ( direction << 16u ) | ( 0xFu << 20u ) | ( visible ? 1u << 24u : 0u ),
                                 meshIndex, 0u, 0u );
                frame.Cards.push_back( payload );
                frame.ResidentCards += resident ? 1u : 0u;
                frame.CapturedCards += visible ? 1u : 0u;

                const auto capture = captured.find( &card );
                for ( uint32_t py = 0; py < card.PagesY; ++py )
                    for ( uint32_t px = 0; px < card.PagesX; ++px )
                    {
                        const AtlasRect& rect = card.Pages[py * card.PagesX + px];
                        const glm::uvec2 offset( px * kVirtualPageSize, py * kVirtualPageSize );
                        CardPagePayload  page;
                        page.Card       = glm::uvec4( cardIndex, 1u, 0u, 0u );
                        page.CardUVRect = glm::vec4( static_cast<float>( offset.x ) / card.ResX,
                                                     static_cast<float>( offset.y ) / card.ResY,
                                                     static_cast<float>( rect.Width ) / card.ResX,
                                                     static_cast<float>( rect.Height ) / card.ResY );
                        page.PhysicalAtlasUVRect =
                             glm::vec4( rect.X / atlas, rect.Y / atlas, rect.Width / atlas, rect.Height / atlas );
                        page.TexelScale =
                             glm::vec4( 1.0f / card.ResX, 1.0f / card.ResY, 1.0f / atlas, 1.0f / atlas );
                        // DIRECT LIGHTING BUDGET (UE r.LumenScene.DirectLighting.UpdateFactor): a page captured
                        // this frame is lit in it; every other visible page in one frame of
                        // DirectLightingUpdateFactor, chosen by its PHYSICAL page (stable while allocated;
                        // sub-allocated cards of one page relight together).
                        const uint32_t physicalPage =
                             rect.X / kPhysicalPageSize +
                             rect.Y / kPhysicalPageSize * AtlasPagesPerSide( m_Budget.AtlasSize );
                        if ( visible &&
                             ( capturing || physicalPage % lightingFactor == m_Frame % lightingFactor ) )
                            frame.LightingPages.push_back( static_cast<uint32_t>( frame.Pages.size() ) );
                        frame.Pages.push_back( page );
                        if ( capture != captured.end() )
                        {
                            const AtlasRect& source = frame.Captures[capture->second].CaptureRect;
                            frame.Copies.push_back(
                                 { glm::uvec4( source.X, source.Y, card.ResX, card.ResY ),
                                   glm::uvec4( rect.X, rect.Y, rect.Width | ( rect.Height << 16u ),
                                               offset.x | ( offset.y << 16u ) ) } );
                        }
                    }
                if ( capture != captured.end() )
                {
                    frame.Captures[capture->second].Source = instance.Source;
                    frame.Captures[capture->second].World  = instance.World;
                }
            }
            frame.MeshCards.push_back( mesh );
        }
        return frame;
    }
} // namespace Desert::Graphic::SurfaceCache
