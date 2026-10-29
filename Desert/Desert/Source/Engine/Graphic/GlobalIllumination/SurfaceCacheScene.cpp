#include <Engine/Graphic/GlobalIllumination/SurfaceCacheScene.hpp>

#include <Engine/Core/Projection.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace Desert::Graphic::SurfaceCache
{
    namespace
    {
        // UE GetCardMaxResolution clamps the viewer distance to a metre: a card the camera stands inside is not
        // asked for infinite texels.
        constexpr float kMinViewerDistance = 100.0f; // cm
        // "No card": an eviction for a low mip, which outranks every high mip whatever its distance.
        constexpr uint32_t kNoCard = std::numeric_limits<uint32_t>::max();

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
                card.Low   = {};
                card.High  = {};
                card.Shown = ShownMip::None;
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

    void SurfaceCacheScene::FreeMip( CardMip& mip )
    {
        for ( const AtlasRect& page : mip.Pages )
            m_Allocator.Free( page );
        mip = {};
    }

    void SurfaceCacheScene::FreeCard( Card& card )
    {
        FreeMip( card.Low );
        FreeMip( card.High );
        card.Shown = ShownMip::None;
    }

    bool SurfaceCacheScene::AllocateMip( CardMip& mip, const uint32_t resX, const uint32_t resY )
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
        mip.Pages        = std::move( pages );
        mip.ResX         = resX;
        mip.ResY         = resY;
        mip.PagesX       = pagesX;
        mip.PagesY       = pagesY;
        mip.Captured     = false;
        mip.CaptureFrame = 0;
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
                FreeRow( it->second.Row );
                m_Instances.erase( it );
                it = m_Instances.end();
            }
            if ( it == m_Instances.end() )
            {
                Instance fresh;
                fresh.Mesh       = in.Mesh;
                fresh.Build      = in.Cards;
                fresh.World      = in.World;
                fresh.SurfaceKey = in.SurfaceKey;
                fresh.Row        = AllocateRow();
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
                    for ( CardMip* mip : { &card.Low, &card.High } )
                    {
                        mip->Captured     = false;
                        mip->CaptureFrame = 0;
                    }
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
            FreeRow( it->second.Row );
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

        // ALLOCATION. The high mips' pages are tracked by physical page (page -> indices of the sorted cards whose
        // high mip lies on it): a page is freed for a card by evicting every high mip on it. Low mips sit in bins
        // of the smallest card side, which a high mip never asks for (it is larger on one side at least), so a
        // page holding low mips never has a high occupant and is never evicted.
        const uint32_t                     pagesPerSide = AtlasPagesPerSide( m_Budget.AtlasSize );
        std::vector<std::vector<uint32_t>> occupants( static_cast<std::size_t>( pagesPerSide ) * pagesPerSide );
        const auto                         pageOf = [pagesPerSide]( const AtlasRect& rect )
        { return rect.Y / kPhysicalPageSize * pagesPerSide + rect.X / kPhysicalPageSize; };
        const auto occupy = [&]( const uint32_t index )
        {
            for ( const AtlasRect& rect : cards[index]->High.Pages )
                occupants[pageOf( rect )].push_back( index );
        };
        const auto evictHigh = [&]( const uint32_t index )
        {
            for ( const AtlasRect& rect : cards[index]->High.Pages )
                std::erase( occupants[pageOf( rect )], index );
            FreeMip( cards[index]->High );
        };
        // The page whose nearest high occupant is the farthest, if that occupant is farther than @p nearerThan.
        const auto farthestPage = [&]( const uint32_t nearerThan )
        {
            std::size_t victimPage = occupants.size();
            uint32_t    victimNear = nearerThan;
            for ( std::size_t page = 0; page < occupants.size(); ++page )
            {
                if ( occupants[page].empty() )
                    continue;
                const uint32_t nearest = *std::min_element( occupants[page].begin(), occupants[page].end() );
                if ( victimNear == kNoCard || nearest > victimNear )
                {
                    victimNear = nearest;
                    victimPage = page;
                }
            }
            return victimPage;
        };
        const auto evictPage = [&]( const std::size_t page )
        {
            const std::vector<uint32_t> victims = occupants[page];
            for ( const uint32_t victim : victims )
                evictHigh( victim );
        };
        for ( uint32_t i = 0; i < cards.size(); ++i )
            occupy( i );

        // LOW MIPS, nearest first: every card holds the smallest card side, always. One that does not fit takes
        // the page of the farthest high mips, whatever their distance; with no high mip left the atlas is too
        // small for the scene's minimum (SceneFrame::MissingMinimumCards).
        for ( uint32_t i = 0; i < cards.size(); ++i )
        {
            CardMip& low = cards[i]->Low;
            if ( low.ResX == minRes && low.ResY == minRes )
                continue;
            FreeMip( low );
            for ( ;; )
            {
                if ( AllocateMip( low, minRes, minRes ) )
                    break;
                const std::size_t page = farthestPage( kNoCard );
                if ( page == occupants.size() )
                {
                    ++frame.MissingMinimumCards;
                    break;
                }
                evictPage( page );
            }
        }

        // HIGH MIPS, nearest first, for the cards asking more than the minimum. A resident high mip is
        // reallocated when it wants more texels than it last asked for (a card the full atlas placed smaller keeps
        // what it got until then - reallocating it every frame recaptured it every frame and the cache never
        // settled), or a quarter of what it holds on both sides (hysteresis: a camera wobbling at a level boundary
        // does not recapture). A full atlas makes room by evicting a whole PHYSICAL PAGE whose every high occupant
        // is farther than the card (the page of the farthest such nearest occupant), and only when one exists:
        // evicting farther cards one by one freed bin elements that never added up to a page the card could use.
        // A high mip that does not fit at any resolution above the minimum is not placed; the card shows its low.
        // Once no page is wholly farther than a card, none is for any farther card either.
        bool evictable = true;
        for ( uint32_t i = 0; i < cards.size(); ++i )
        {
            Card&      card     = *cards[i];
            CardMip&   high     = card.High;
            const bool wanted   = card.WantX > minRes || card.WantY > minRes;
            const bool resident = high.ResX != 0;
            if ( !wanted )
            {
                if ( resident )
                    evictHigh( i );
                continue;
            }
            if ( resident && card.WantX <= std::max( high.ResX, card.AskedX ) &&
                 card.WantY <= std::max( high.ResY, card.AskedY ) &&
                 !( card.WantX * 4u <= high.ResX && card.WantY * 4u <= high.ResY ) )
                continue;
            evictHigh( i );
            card.AskedX = card.WantX;
            card.AskedY = card.WantY;
            for ( ;; )
            {
                uint32_t resX = card.WantX, resY = card.WantY;
                bool     placed = false;
                while ( resX > minRes || resY > minRes )
                {
                    if ( AllocateMip( high, resX, resY ) )
                    {
                        placed = true;
                        break;
                    }
                    resX = std::max( minRes, resX / 2u );
                    resY = std::max( minRes, resY / 2u );
                }
                if ( placed )
                {
                    occupy( i );
                    break;
                }
                const std::size_t page = evictable ? farthestPage( i ) : occupants.size();
                if ( page == occupants.size() )
                {
                    evictable = false;
                    break;
                }
                evictPage( page );
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
                    for ( CardMip* mip : { &card.Low, &card.High } )
                    {
                        if ( mip->CaptureFrame == 0 )
                            continue;
                        if ( std::find( executed.begin(), executed.end(), mip->CaptureFrame ) != executed.end() )
                        {
                            mip->Captured     = true;
                            mip->CaptureFrame = 0;
                        }
                        else if ( mip->CaptureFrame + kCaptureConfirmFrames < m_Frame )
                            mip->CaptureFrame = 0;
                    }
        }

        // CAPTURES, shelf-packed into the capture atlas (rows of the tallest mip in them): every low mip first,
        // nearest first - the guaranteed surface before any sharper one - then the high mips, nearest first. A
        // low mip is captured even under a captured high one: it is what the card shows once the high is evicted.
        uint32_t                           shelfX = 0, shelfY = 0, shelfHeight = 0;
        std::map<const CardMip*, uint32_t> captured; // mip -> index in frame.Captures
        const auto                         capture = [&]( const Card& card, CardMip& mip )
        {
            if ( mip.ResX == 0 || mip.Captured || mip.CaptureFrame != 0 || !card.Ready )
                return;
            if ( shelfX + mip.ResX > m_CaptureAtlasSize )
            {
                shelfX = 0;
                shelfY += shelfHeight;
                shelfHeight = 0;
            }
            if ( shelfY + mip.ResY > m_CaptureAtlasSize )
                return; // the next frame's atlas
            CardCapture entry;
            entry.CaptureRect = { shelfX, shelfY, mip.ResX, mip.ResY };
            shelfX += mip.ResX;
            shelfHeight = std::max( shelfHeight, mip.ResY );
            // The card camera: on the card plane (the face the card direction points out of), looking back
            // into the box, X right and Y up, the box's depth its depth range.
            const glm::vec3 y = card.Axes[1], z = card.Axes[2];
            entry.Eye        = card.Center + z * card.Extent.z;
            entry.View       = glm::lookAt( entry.Eye, entry.Eye - z, y );
            entry.Projection = Core::MakeOrthographic( -card.Extent.x, card.Extent.x, -card.Extent.y,
                                                       card.Extent.y, 0.0f, 2.0f * card.Extent.z );
            captured.emplace( &mip, static_cast<uint32_t>( frame.Captures.size() ) );
            frame.Captures.push_back( entry );
            mip.CaptureFrame = m_Frame;
        };
        for ( Card* card : cards )
        {
            if ( frame.Captures.size() >= m_MaxCapturesPerFrame )
                break;
            capture( *card, card->Low );
        }
        for ( Card* card : cards )
        {
            if ( frame.Captures.size() >= m_MaxCapturesPerFrame )
                break;
            capture( *card, card->High );
        }

        // THE GPU RECORDS (compact, this frame's order) and the page copies of this frame's captures.
        const float    atlas          = static_cast<float>( m_Budget.AtlasSize );
        // Every visible page is relit in the frame after InvalidateLighting.
        const uint64_t lightingFactor = m_RelightAll ? 1u : std::max( m_Budget.DirectLightingUpdateFactor, 1u );
        m_RelightAll                  = false;
        frame.Frame                   = m_Frame;
        // (row, MeshCards index) of every instance, for the B3 table.
        std::vector<std::pair<uint32_t, uint32_t>> rowRecords;
        rowRecords.reserve( m_Instances.size() );
        for ( auto& [key, instance] : m_Instances )
        {
            const uint32_t meshIndex = static_cast<uint32_t>( frame.MeshCards.size() );
            rowRecords.emplace_back( instance.Row, meshIndex );
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
                // A mip is shown (and lit) once captured, or while this frame's graph captures it: the same graph
                // copies the capture into the atlas before any reader, and a dropped graph drops its readers with
                // it. The record shows the high mip when it can, the low mip otherwise (UE: an unmapped
                // high-resolution page falls back to the locked low-resolution one).
                const auto shows = [this]( const CardMip& mip )
                { return mip.ResX != 0 && ( mip.Captured || mip.CaptureFrame == m_Frame ); };
                ShownMip shownMip = ShownMip::None;
                if ( shows( card.High ) )
                    shownMip = ShownMip::High;
                else if ( card.Low.ResX != 0 )
                    shownMip = ShownMip::Low;
                else if ( card.High.ResX != 0 )
                    shownMip = ShownMip::High;
                const CardMip& mip       = shownMip == ShownMip::High ? card.High : card.Low;
                const bool     resident  = shownMip != ShownMip::None;
                const bool     visible   = resident && shows( mip );
                const bool     capturing = resident && mip.CaptureFrame == m_Frame;
                // A mip shown lit for the first time without a capture this frame (the high one evicted, the low
                // one back on show) has lit layers its last lighting left, or a stranger's: lit like a capture.
                const bool relight  = visible && ( capturing || card.Shown != shownMip );
                card.Shown          = visible ? shownMip : ShownMip::None;
                payload.LocalExtent = glm::vec4( card.Extent, resident ? 2.0f * card.Extent.x / mip.ResX : 0.0f );
                payload.Pages       = glm::uvec4( mip.PagesX | ( mip.PagesY << 16u ),
                                                  static_cast<uint32_t>( frame.Pages.size() ), 0u, 0u );
                payload.Info =
                     glm::uvec4( ( resident ? Log2( mip.ResX ) | ( Log2( mip.ResY ) << 8u ) : 0u ) |
                                      ( direction << 16u ) | ( 0xFu << 20u ) | ( visible ? 1u << 24u : 0u ),
                                 meshIndex, 0u, 0u );
                frame.Cards.push_back( payload );
                frame.ResidentCards += resident ? 1u : 0u;
                frame.CapturedCards += visible ? 1u : 0u;
                frame.HighResCards += visible && shownMip == ShownMip::High ? 1u : 0u;

                for ( uint32_t py = 0; py < mip.PagesY; ++py )
                    for ( uint32_t px = 0; px < mip.PagesX; ++px )
                    {
                        const AtlasRect& rect = mip.Pages[py * mip.PagesX + px];
                        const glm::uvec2 offset( px * kVirtualPageSize, py * kVirtualPageSize );
                        CardPagePayload  page;
                        // z = 1: captured or first shown this frame (its lit layers hold stale or a stranger's
                        // texels until relit).
                        page.Card       = glm::uvec4( cardIndex, 1u, relight ? 1u : 0u, 0u );
                        page.CardUVRect = glm::vec4( static_cast<float>( offset.x ) / mip.ResX,
                                                     static_cast<float>( offset.y ) / mip.ResY,
                                                     static_cast<float>( rect.Width ) / mip.ResX,
                                                     static_cast<float>( rect.Height ) / mip.ResY );
                        page.PhysicalAtlasUVRect =
                             glm::vec4( rect.X / atlas, rect.Y / atlas, rect.Width / atlas, rect.Height / atlas );
                        page.TexelScale =
                             glm::vec4( 1.0f / mip.ResX, 1.0f / mip.ResY, 1.0f / atlas, 1.0f / atlas );
                        // DIRECT LIGHTING BUDGET (UE r.LumenScene.DirectLighting.UpdateFactor): a page captured
                        // or first shown this frame is lit in it; every other visible page in one frame of
                        // DirectLightingUpdateFactor, chosen by its PHYSICAL page (stable while allocated;
                        // sub-allocated cards of one page relight together).
                        const uint32_t physicalPage =
                             rect.X / kPhysicalPageSize +
                             rect.Y / kPhysicalPageSize * AtlasPagesPerSide( m_Budget.AtlasSize );
                        if ( visible && ( relight || physicalPage % lightingFactor == m_Frame % lightingFactor ) )
                            frame.LightingPages.push_back( static_cast<uint32_t>( frame.Pages.size() ) );
                        frame.Pages.push_back( page );
                    }
                // The page copies of every mip captured this frame, shown or not.
                for ( const CardMip* source : { &card.Low, &card.High } )
                {
                    const auto entry = captured.find( source );
                    if ( entry == captured.end() )
                        continue;
                    CardCapture& captureEntry = frame.Captures[entry->second];
                    captureEntry.Source       = instance.Source;
                    captureEntry.World        = instance.World;
                    for ( uint32_t py = 0; py < source->PagesY; ++py )
                        for ( uint32_t px = 0; px < source->PagesX; ++px )
                        {
                            const AtlasRect& rect = source->Pages[py * source->PagesX + px];
                            const glm::uvec2 offset( px * kVirtualPageSize, py * kVirtualPageSize );
                            frame.Copies.push_back(
                                 { glm::uvec4( captureEntry.CaptureRect.X, captureEntry.CaptureRect.Y,
                                               source->ResX, source->ResY ),
                                   glm::uvec4( rect.X, rect.Y, rect.Width | ( rect.Height << 16u ),
                                               offset.x | ( offset.y << 16u ) ) } );
                        }
                }
            }
            frame.MeshCards.push_back( mesh );
        }
        BuildInstanceTable( frame, rowRecords );
        return frame;
    }

    uint32_t SurfaceCacheScene::AllocateRow()
    {
        if ( m_FreeRows.empty() )
            return m_RowCount++;
        // Lowest first: the table stays dense where instances come and go.
        const auto     lowest = std::min_element( m_FreeRows.begin(), m_FreeRows.end() );
        const uint32_t row    = *lowest;
        *lowest               = m_FreeRows.back();
        m_FreeRows.pop_back();
        return row;
    }

    void SurfaceCacheScene::FreeRow( const uint32_t row )
    {
        if ( row != kNoInstance )
            m_FreeRows.push_back( row );
    }

    void
    SurfaceCacheScene::BuildInstanceTable( SceneFrame&                                       frame,
                                           const std::vector<std::pair<uint32_t, uint32_t>>& rowRecords ) const
    {
        // THE TABLE: every row ever handed out; a freed row reads "no cards" until it is reused.
        frame.InstanceRows.assign( m_RowCount, InstanceRowPayload{} );
        for ( const auto& [row, meshIndex] : rowRecords )
        {
            const MeshCardsPayload& mesh  = frame.MeshCards[meshIndex];
            const uint32_t          first = mesh.Cards.x, count = mesh.Cards.y;
            glm::vec3 lo( std::numeric_limits<float>::max() ), hi( -std::numeric_limits<float>::max() );
            uint32_t  firstPage = 0, pages = 0;
            bool      anyVisible = false;
            for ( uint32_t c = first; c < first + count; ++c )
            {
                const CardPayload& card = frame.Cards[c];
                const glm::vec3 origin( card.WorldToLocal[0].w, card.WorldToLocal[1].w, card.WorldToLocal[2].w );
                // The card box's world half extent along each world axis: |rows of the rotation|^T * extent.
                glm::vec3 half( 0.0f );
                for ( int r = 0; r < 3; ++r )
                    half += glm::abs( glm::vec3( card.WorldToLocal[r] ) ) * card.LocalExtent[r];
                lo                       = glm::min( lo, origin - half );
                hi                       = glm::max( hi, origin + half );
                const uint32_t cardPages = ( card.Pages.x & 0xFFFFu ) * ( card.Pages.x >> 16u );
                if ( c == first )
                    firstPage = card.Pages.y;
                pages += cardPages;
                anyVisible = anyVisible || ( card.Info.x & ( 1u << 24u ) ) != 0u;
            }
            InstanceRowPayload& out = frame.InstanceRows[row];
            // No captured card this frame (none resident: a small or far mesh) - the hit is lit in place.
            out.Cards     = glm::uvec4( anyVisible ? meshIndex : kNoMeshCards, firstPage, pages, 0u );
            out.BoundsMin = glm::vec4( lo, 0.0f );
            out.BoundsMax = glm::vec4( hi, 0.0f );
        }
    }

    uint64_t SurfaceCacheScene::PlacementKey( const void* mesh, const glm::mat4& world )
    {
        uint64_t     hash = std::bit_cast<uint64_t>( mesh ) * 0x9E3779B97F4A7C15ULL;
        const float* m    = &world[0][0];
        for ( int i = 0; i < 16; ++i )
            hash = ( hash ^ std::bit_cast<uint32_t>( m[i] ) ) * 0x100000001B3ULL;
        return hash;
    }

    std::unordered_map<uint64_t, uint32_t> SurfaceCacheScene::RowsByPlacement() const
    {
        std::unordered_map<uint64_t, uint32_t> rows;
        rows.reserve( m_Instances.size() );
        for ( const auto& [key, instance] : m_Instances )
            rows.emplace( PlacementKey( instance.Mesh, instance.World ), instance.Row );
        return rows;
    }
} // namespace Desert::Graphic::SurfaceCache
