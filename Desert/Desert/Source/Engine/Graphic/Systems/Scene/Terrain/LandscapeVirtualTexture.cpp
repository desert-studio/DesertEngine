#include "LandscapeVirtualTexture.hpp"

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>
#include <Engine/Graphic/RenderPassDeclaration.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/SceneRendererFrame.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

namespace
{
    // "Landscape: Virtual Texture Pages" (Programs/Terrain/LandscapeVirtualTexturePage.shader).
    struct PageShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, LandscapeVirtualRows );
        DESERT_SHADER_PARAMETER( StorageBuffer, LandscapeVirtualJobs );
        DESERT_SHADER_PARAMETER( StorageBuffer, LandscapeVirtualTiles );
        DESERT_SHADER_PARAMETER( StorageBuffer, LandscapeVirtualPageTable );
        DESERT_SHADER_PARAMETER( StorageTexture, LandscapeVirtualColorAtlas );
        DESERT_SHADER_PARAMETER( StorageTexture, LandscapeVirtualNormalAtlas );
        DESERT_SHADER_PARAMETER( StorageTexture, LandscapeVirtualOcclusionAtlas );
        DESERT_SHADER_PARAMETER( StorageTexture, LandscapeVirtualHeightAtlas );
    };
    constexpr std::string_view kPagePass   = "Landscape: Virtual Texture Pages";
    constexpr std::string_view kPageShader = "LandscapeVirtualTexturePage";
    // LocalSize 8x8: a page with its border (136 texels a side) is 17 x 17 groups.
    constexpr uint32_t kPageGroups = Desert::Graphic::System::LandscapeVirtualTexture::kPageStride / 8u;
    // The tile grid's largest side in cells (the page render's texel -> tile lookup).
    constexpr uint32_t kMaxTileGrid    = 1024u;
    constexpr uint32_t kTileGridHeader = 4u;

    template <typename T>
    std::span<const std::byte> Bytes( const std::vector<T>& values ) noexcept
    {
        return std::as_bytes( std::span<const T>( values ) );
    }

    uint64_t TileId( const glm::vec2 origin, const float side ) noexcept
    {
        const uint64_t x = std::bit_cast<uint32_t>( origin.x );
        const uint64_t z = std::bit_cast<uint32_t>( origin.y );
        const uint64_t s = std::bit_cast<uint32_t>( side );
        return ( x * 0x9E3779B97F4A7C15ull ) ^ ( z * 0xC2B2AE3D27D4EB4Full ) ^ ( s * 0x165667B19E3779F9ull );
    }

    bool Overlaps( const glm::vec4 a, const glm::vec4 b ) noexcept
    {
        return a.x < b.z && b.x < a.z && a.y < b.w && b.y < a.w;
    }
} // namespace

namespace Desert::Graphic::System
{
    LandscapeVirtualTexture::LandscapeVirtualTexture()  = default;
    LandscapeVirtualTexture::~LandscapeVirtualTexture() = default;

    void LandscapeVirtualTexture::SetQuality( const LandscapeVirtualTextureQuality& quality ) noexcept
    {
        LandscapeVirtualTextureQuality clamped;
        clamped.SizeLog2        = std::clamp( quality.SizeLog2, 12u, 17u );
        clamped.PoolPages       = std::clamp( quality.PoolPages, 8u, 64u );
        clamped.UpdatesPerFrame = std::clamp( quality.UpdatesPerFrame, 8u, 256u );
        if ( clamped == m_Quality )
            return;
        m_Quality        = clamped;
        m_ResourcesValid = false;
        m_LayoutValid    = false;
    }

    void LandscapeVirtualTexture::DeclareDisabled( RDG::Builder& graph, FrameTextures& textures )
    {
        // A zero header (word 7 = 0: disabled), the system black texture for the atlases and one feedback word:
        // TerrainGBuffer then blends the layers per pixel.
        m_Bindings.PageTable = graph.CreateBuffer( RDG::BufferDesc{ kHeaderWords * sizeof( uint32_t ) },
                                                   "Landscape.VirtualTexture.Disabled" );
        graph.QueueBufferClear( m_Bindings.PageTable, 0u );
        m_Bindings.Feedback =
             graph.CreateBuffer( RDG::BufferDesc{ sizeof( uint32_t ) }, "Landscape.VirtualTexture.NoFeedback" );
        graph.QueueBufferClear( m_Bindings.Feedback, 0u );
        const RDG::TextureRef black = textures.GraphRefs().System.Black;
        m_Bindings.Color            = black;
        m_Bindings.Normal           = black;
        m_Bindings.Occlusion        = black;
        m_Bindings.Height           = black;
    }

    void LandscapeVirtualTexture::ResetLayout( const glm::vec2 origin, const float extent ) noexcept
    {
        m_LayoutValid = true;
        ++m_Generation;
        m_Origin    = origin;
        m_Extent    = extent;
        m_Mip0Pages = 1u << ( m_Quality.SizeLog2 - 7u );
        m_Mips      = m_Quality.SizeLog2 - 7u + 1u;
        m_PoolPages = m_Quality.PoolPages;
        m_MipFirst.fill( 0u );
        uint32_t words = 0u;
        for ( uint32_t mip = 0; mip < m_Mips; ++mip )
        {
            m_MipFirst[mip]     = words;
            const uint32_t side = m_Mip0Pages >> mip;
            words += side * side;
        }
        m_TopEntry = m_MipFirst[m_Mips - 1u];
        m_Entries.assign( words, 0u );
        m_ChangedEntries.clear();
        m_UploadAll = true;
        m_Slots.assign( static_cast<std::size_t>( m_PoolPages ) * m_PoolPages, Slot{} );
        m_Requests.clear();
        m_Requests.push_back( m_TopEntry ); // the top mip (one page) is pinned
        m_TileKeys.clear();
        m_DistanceKey = INT32_MIN;
    }

    uint32_t LandscapeVirtualTexture::MipOf( const uint32_t entry ) const noexcept
    {
        uint32_t mip = 0u;
        while ( mip + 1u < m_Mips && entry >= m_MipFirst[mip + 1u] )
            ++mip;
        return mip;
    }

    glm::vec4 LandscapeVirtualTexture::PageRect( const uint32_t entry ) const noexcept
    {
        const uint32_t mip   = MipOf( entry );
        const uint32_t side  = m_Mip0Pages >> mip;
        const uint32_t local = entry - m_MipFirst[mip];
        const float    page  = m_Extent / static_cast<float>( side );
        const float    x0    = m_Origin.x + static_cast<float>( local % side ) * page;
        const float    z0    = m_Origin.y + static_cast<float>( local / side ) * page;
        return { x0, z0, x0 + page, z0 + page };
    }

    void LandscapeVirtualTexture::SetEntry( const uint32_t entry, const uint32_t value ) noexcept
    {
        if ( entry >= m_Entries.size() || m_Entries[entry] == value )
            return;
        m_Entries[entry] = value;
        if ( !m_UploadAll )
            m_ChangedEntries.push_back( entry );
    }

    void LandscapeVirtualTexture::InvalidateRect( const glm::vec4 rect ) noexcept
    {
        // A page's texels near its edge read the neighbour's ground through the border: widen by one border.
        for ( Slot& slot : m_Slots )
        {
            if ( slot.Virtual < 0 )
                continue;
            const glm::vec4 page = PageRect( static_cast<uint32_t>( slot.Virtual ) );
            const float     border =
                 ( page.z - page.x ) * static_cast<float>( kPageBorder ) / static_cast<float>( kPageTexels );
            if ( Overlaps( rect, page + glm::vec4( -border, -border, border, border ) ) )
                slot.Dirty = true;
        }
    }

    void LandscapeVirtualTexture::ReadFeedback( const uint64_t frameIndex )
    {
        // The slot's frame has completed (the frame start waited its fence), as the dice cull's census reads.
        FeedbackSlot& slot = m_Feedback[static_cast<std::size_t>( frameIndex % m_Feedback.size() )];
        if ( slot.Buffer.Physical && slot.Generation == m_Generation && slot.Words > 0u )
        {
            const std::span<const std::byte> bytes = slot.Buffer.Physical->ReadForHost();
            const std::size_t words = std::min<std::size_t>( slot.Words, bytes.size() / sizeof( uint32_t ) );
            for ( std::size_t w = 0; w < words; ++w )
            {
                uint32_t bits = 0u;
                std::memcpy( &bits, bytes.data() + w * sizeof( uint32_t ), sizeof( uint32_t ) );
                while ( bits != 0u )
                {
                    const uint32_t bit = static_cast<uint32_t>( std::countr_zero( bits ) );
                    bits &= bits - 1u;
                    const auto entry = static_cast<uint32_t>( w * 32u + bit );
                    if ( entry >= m_Entries.size() )
                        continue;
                    // The page and its parents up to the pinned top: a missing page falls back to the first
                    // resident parent, so the chain is requested coarse first.
                    uint32_t mip   = MipOf( entry );
                    uint32_t local = entry - m_MipFirst[mip];
                    uint32_t side  = m_Mip0Pages >> mip;
                    uint32_t x     = local % side;
                    uint32_t y     = local / side;
                    while ( true )
                    {
                        const uint32_t e = m_MipFirst[mip] + y * side + x;
                        if ( m_Entries[e] != 0u )
                            m_Slots[m_Entries[e] - 1u].LastUsed = frameIndex;
                        else
                            m_Requests.push_back( e );
                        if ( mip + 1u >= m_Mips )
                            break;
                        ++mip;
                        x >>= 1u;
                        y >>= 1u;
                        side >>= 1u;
                    }
                }
            }
        }
        slot = FeedbackSlot{};
    }

    Common::BoolResultStr LandscapeVirtualTexture::EnsurePipeline()
    {
        if ( m_PagePipeline )
            return BOOLSUCCESS;
        const auto shaders = Runtime::ResourceRegistry::GetShaderService();
        const auto shader  = shaders ? shaders->GetByName( std::string( kPageShader ) ) : nullptr;
        if ( !shader )
            return Common::MakeFormattedError<bool>(
                 "{}: compute shader '{}' is not registered (Programs/Terrain/{}.shader)", kPagePass, kPageShader,
                 kPageShader );
        const auto pipeline =
             ComputePipeline::Create( { .Shader = shader, .DebugName = std::string( kPageShader ) } );
        if ( !pipeline )
            return Common::MakeFormattedError<bool>( "{}: {}", kPagePass, pipeline.GetError() );
        m_PagePipeline = pipeline.GetValue();
        return BOOLSUCCESS;
    }

    Common::BoolResultStr LandscapeVirtualTexture::AddPasses( RDG::Builder& graph, FrameTextures& textures,
                                                              const ViewFrame&                             frame,
                                                              std::span<const LandscapeVirtualTextureTile> tiles,
                                                              const bool enabled )
    {
        m_Bindings           = {};
        const uint32_t slots = std::max( 1u, EngineContext::GetInstance().GetMaxFramesInFlight() );
        if ( m_Feedback.size() != slots )
        {
            m_Feedback.clear();
            m_Feedback.resize( slots );
        }
        if ( !enabled || tiles.empty() )
        {
            DeclareDisabled( graph, textures );
            return BOOLSUCCESS;
        }
        if ( Common::BoolResultStr ready = EnsurePipeline(); !ready )
        {
            DeclareDisabled( graph, textures );
            return ready;
        }

        // ── The virtual square: grow-only over the union of the queued tiles; a regrow drops every page. ──
        glm::vec2 lo( FLT_MAX );
        glm::vec2 hi( -FLT_MAX );
        float     minSide = FLT_MAX;
        for ( const LandscapeVirtualTextureTile& tile : tiles )
        {
            lo      = glm::min( lo, tile.Origin );
            hi      = glm::max( hi, tile.Origin + glm::vec2( tile.Side ) );
            minSide = std::min( minSide, tile.Side );
        }
        if ( !( minSide > 0.0f ) || !( hi.x > lo.x ) || !( hi.y > lo.y ) )
        {
            DeclareDisabled( graph, textures );
            return Common::MakeFormattedError<bool>( "{}: the queued tiles cover no ground", kPagePass );
        }
        // An image lost with a failed graph (Extract never handed it back) drops the pages that were in it.
        if ( m_ResourcesValid &&
             ( !m_PageTable.Physical || !m_Color.Physical || !m_Normal.Physical || !m_Occlusion.Physical ||
               !m_Height.Physical ) )
        {
            m_ResourcesValid = false;
            m_LayoutValid    = false;
        }
        const bool inside = m_LayoutValid && lo.x >= m_Origin.x && lo.y >= m_Origin.y &&
                            hi.x <= m_Origin.x + m_Extent && hi.y <= m_Origin.y + m_Extent;
        if ( !inside )
        {
            glm::vec2 newLo = lo;
            glm::vec2 newHi = hi;
            if ( m_LayoutValid )
            {
                newLo = glm::min( newLo, m_Origin );
                newHi = glm::max( newHi, m_Origin + glm::vec2( m_Extent ) );
            }
            ResetLayout( newLo, std::max( newHi.x - newLo.x, newHi.y - newLo.y ) );
        }

        ReadFeedback( frame.FrameIndex );

        // ── Invalidation: tiles whose key changed or that entered the queue; the far tiling's distance key. ──
        std::unordered_map<uint64_t, uint64_t> keys;
        keys.reserve( tiles.size() );
        for ( const LandscapeVirtualTextureTile& tile : tiles )
        {
            const uint64_t id = TileId( tile.Origin, tile.Side );
            keys[id]          = tile.Key;
            const auto known  = m_TileKeys.find( id );
            if ( known == m_TileKeys.end() || known->second != tile.Key )
                InvalidateRect(
                     { tile.Origin.x, tile.Origin.y, tile.Origin.x + tile.Side, tile.Origin.y + tile.Side } );
        }
        m_TileKeys = std::move( keys );
        // The world-height channel's range (header words 28, 29): every sample a queued tile can hold. A change
        // re-renders every resident page (their stored heights are fractions of the old range).
        glm::vec2 heightRange( FLT_MAX, -FLT_MAX );
        for ( const LandscapeVirtualTextureTile& tile : tiles )
            heightRange = glm::vec2( std::min( heightRange.x, tile.HeightMin ), std::max( heightRange.y, tile.HeightMax ) );
        if ( !( heightRange.y > heightRange.x ) )
            heightRange = glm::vec2( heightRange.x, heightRange.x + 1.0f );
        if ( heightRange != m_HeightRange )
        {
            m_HeightRange = heightRange;
            for ( Slot& slot : m_Slots )
                slot.Dirty = slot.Virtual >= 0;
        }
        const float pixelAngle =
             2.0f / std::max( std::abs( frame.Projection[1][1] ) *
                                   std::max( static_cast<float>( frame.Split.Render.Height ), 1.0f ),
                              1.0e-6f );
        const auto distanceKey = static_cast<int32_t>( std::lround( std::log2( pixelAngle ) * 4.0f ) );
        if ( distanceKey != m_DistanceKey )
        {
            m_DistanceKey = distanceKey;
            for ( Slot& slot : m_Slots )
                slot.Dirty = slot.Virtual >= 0;
        }

        // ── The frame's page renders: dirty resident pages first, then the requests coarse mip first. ──
        std::vector<glm::uvec4> jobs;
        const auto              pushJob = [&]( const uint32_t entry, const uint32_t slot )
        {
            const uint32_t mip   = MipOf( entry );
            const uint32_t side  = m_Mip0Pages >> mip;
            const uint32_t local = entry - m_MipFirst[mip];
            jobs.emplace_back( local % side, local / side, mip, slot );
        };
        const uint32_t budget = m_Quality.UpdatesPerFrame;
        for ( uint32_t s = 0; s < m_Slots.size() && jobs.size() < budget; ++s )
        {
            if ( m_Slots[s].Virtual >= 0 && m_Slots[s].Dirty )
            {
                m_Slots[s].Dirty = false;
                pushJob( static_cast<uint32_t>( m_Slots[s].Virtual ), s );
            }
        }
        std::sort( m_Requests.begin(), m_Requests.end(),
                   [&]( const uint32_t a, const uint32_t b )
                   {
                       const uint32_t ma = MipOf( a );
                       const uint32_t mb = MipOf( b );
                       return ma != mb ? ma > mb : a < b;
                   } );
        m_Requests.erase( std::unique( m_Requests.begin(), m_Requests.end() ), m_Requests.end() );
        // The LRU: slots not used in the last two frames, oldest first; the pinned top page never.
        std::vector<uint32_t> victims;
        bool                  victimsBuilt = false;
        std::size_t           nextVictim   = 0;
        uint32_t              nextFree     = 0;
        for ( const uint32_t entry : m_Requests )
        {
            if ( jobs.size() >= budget )
                break;
            if ( m_Entries[entry] != 0u )
                continue;
            while ( nextFree < m_Slots.size() && m_Slots[nextFree].Virtual >= 0 )
                ++nextFree;
            uint32_t slot = 0u;
            if ( nextFree < m_Slots.size() )
                slot = nextFree;
            else
            {
                if ( !victimsBuilt )
                {
                    victimsBuilt = true;
                    for ( uint32_t s = 0; s < m_Slots.size(); ++s )
                        if ( m_Slots[s].Virtual >= 0 && m_Slots[s].Virtual != static_cast<int64_t>( m_TopEntry ) &&
                             m_Slots[s].LastUsed + 2u <= frame.FrameIndex )
                            victims.push_back( s );
                    std::sort( victims.begin(), victims.end(), [&]( const uint32_t a, const uint32_t b )
                               { return m_Slots[a].LastUsed < m_Slots[b].LastUsed; } );
                }
                if ( nextVictim >= victims.size() )
                    break; // the pool is full of pages in use: the rest fall back to their parents
                slot = victims[nextVictim++];
                SetEntry( static_cast<uint32_t>( m_Slots[slot].Virtual ), 0u );
            }
            m_Slots[slot] = Slot{ .Virtual = entry, .LastUsed = frame.FrameIndex, .Dirty = false };
            SetEntry( entry, slot + 1u );
            pushJob( entry, slot );
        }
        m_Requests.clear();

        // ── The persistent page table and atlases. ──
        RDG::BufferRef  table;
        RDG::TextureRef color;
        RDG::TextureRef normal;
        RDG::TextureRef occlusion;
        RDG::TextureRef height;
        if ( !m_ResourcesValid )
        {
            const uint64_t tableBytes = ( kHeaderWords + m_Entries.size() ) * sizeof( uint32_t );
            table = graph.CreateBuffer( RDG::BufferDesc{ tableBytes }, "Landscape.VirtualTexture.PageTable" );
            graph.Extract( table, m_PageTable, RDG::Access::StorageRead );
            const uint32_t texels = m_PoolPages * kPageStride;
            const auto     atlas  = [&]( const Core::Formats::ImageFormat format, const std::string_view name,
                                    RDG::ExternalTexture& into )
            {
                const RDG::TextureRef ref = graph.CreateTexture(
                     RDG::TextureDesc{ .Size = { .Width = texels, .Height = texels }, .Format = format }, name );
                graph.Extract( ref, into, RDG::Access::SampledGraphics, { RDG::Access::StorageWrite } );
                return ref;
            };
            color = atlas( Core::Formats::ImageFormat::RGB10A2_UNORM, "Landscape.VirtualTexture.Color", m_Color );
            normal =
                 atlas( Core::Formats::ImageFormat::RGB10A2_UNORM, "Landscape.VirtualTexture.Normal", m_Normal );
            occlusion =
                 atlas( Core::Formats::ImageFormat::R8_UNORM, "Landscape.VirtualTexture.Occlusion", m_Occlusion );
            height =
                 atlas( Core::Formats::ImageFormat::R16_UNORM, "Landscape.VirtualTexture.Height", m_Height );
            m_ResourcesValid = true;
            m_UploadAll      = true;
        }
        else
        {
            table     = graph.RegisterExternal( m_PageTable, "Landscape.VirtualTexture.PageTable" );
            color     = graph.RegisterExternal( m_Color, "Landscape.VirtualTexture.Color" );
            normal    = graph.RegisterExternal( m_Normal, "Landscape.VirtualTexture.Normal" );
            occlusion = graph.RegisterExternal( m_Occlusion, "Landscape.VirtualTexture.Occlusion" );
            height    = graph.RegisterExternal( m_Height, "Landscape.VirtualTexture.Height" );
        }

        // The header, every frame (the feedback phase moves), then the changed entry runs.
        const auto            feedbackWords = static_cast<uint32_t>( ( m_Entries.size() + 31u ) / 32u );
        std::vector<uint32_t> header( kHeaderWords, 0u );
        header[0] = std::bit_cast<uint32_t>( m_Origin.x );
        header[1] = std::bit_cast<uint32_t>( m_Origin.y );
        header[2] = std::bit_cast<uint32_t>( m_Extent );
        header[3] = static_cast<uint32_t>( frame.FrameIndex & 15u );
        header[4] = m_Mips;
        header[5] = m_Mip0Pages;
        header[6] = m_PoolPages;
        header[7] = 1u;
        for ( uint32_t mip = 0; mip < m_Mips; ++mip )
            header[8u + mip] = m_MipFirst[mip];
        header[24] = m_Quality.SizeLog2;
        header[25] = std::bit_cast<uint32_t>( pixelAngle );
        header[26] = static_cast<uint32_t>( m_Entries.size() );
        header[27] = feedbackWords;
        header[28] = std::bit_cast<uint32_t>( m_HeightRange.x );
        header[29] = std::bit_cast<uint32_t>( m_HeightRange.y );
        graph.QueueBufferUpload( table, 0u, Bytes( header ) );
        constexpr uint64_t kEntriesOffset = kHeaderWords * sizeof( uint32_t );
        if ( m_UploadAll )
        {
            graph.QueueBufferUpload( table, kEntriesOffset, Bytes( m_Entries ) );
            m_UploadAll = false;
        }
        else if ( !m_ChangedEntries.empty() )
        {
            std::sort( m_ChangedEntries.begin(), m_ChangedEntries.end() );
            m_ChangedEntries.erase( std::unique( m_ChangedEntries.begin(), m_ChangedEntries.end() ),
                                    m_ChangedEntries.end() );
            std::size_t i = 0;
            while ( i < m_ChangedEntries.size() )
            {
                std::size_t j = i + 1;
                while ( j < m_ChangedEntries.size() && m_ChangedEntries[j] == m_ChangedEntries[j - 1] + 1u )
                    ++j;
                const uint32_t first = m_ChangedEntries[i];
                const auto     count = static_cast<std::size_t>( m_ChangedEntries[j - 1] - first + 1u );
                graph.QueueBufferUpload(
                     table, kEntriesOffset + uint64_t{ first } * sizeof( uint32_t ),
                     std::as_bytes( std::span<const uint32_t>( m_Entries.data() + first, count ) ) );
                i = j;
            }
        }
        m_ChangedEntries.clear();

        // The G-buffer's feedback of this frame, read back when its slot comes round.
        const RDG::BufferRef feedback = graph.CreateBuffer( RDG::BufferDesc{ uint64_t{ feedbackWords } * 4u },
                                                            "Landscape.VirtualTexture.Feedback" );
        graph.QueueBufferClear( feedback, 0u );
        FeedbackSlot& slot = m_Feedback[static_cast<std::size_t>( frame.FrameIndex % m_Feedback.size() )];
        graph.Extract( feedback, slot.Buffer, RDG::Access::HostRead );
        slot.Generation = m_Generation;
        slot.Words      = feedbackWords;

        m_Bindings = { .PageTable = table,
                       .Color     = color,
                       .Normal    = normal,
                       .Occlusion = occlusion,
                       .Height    = height,
                       .Feedback  = feedback };
        if ( jobs.empty() )
            return BOOLSUCCESS;

        // ── The page render: rows, the tile grid over the square, the jobs. ──
        std::vector<glm::vec4> rows;
        rows.reserve( tiles.size() * ( tiles.front().Row.size() ) );
        for ( const LandscapeVirtualTextureTile& tile : tiles )
            rows.insert( rows.end(), tile.Row.begin(), tile.Row.end() );
        const float cell = std::max( minSide, m_Extent / static_cast<float>( kMaxTileGrid ) );
        const auto  grid = std::clamp( static_cast<uint32_t>( std::ceil( m_Extent / cell ) ), 1u, kMaxTileGrid );
        std::vector<uint32_t> cells( kTileGridHeader + std::size_t{ grid } * grid, 0u );
        cells[0] = grid;
        cells[1] = std::bit_cast<uint32_t>( cell );
        cells[2] = static_cast<uint32_t>( tiles.front().Row.size() );
        for ( uint32_t i = 0; i < tiles.size(); ++i )
        {
            const LandscapeVirtualTextureTile& tile = tiles[i];
            const glm::vec2                    a    = ( tile.Origin - m_Origin ) / cell;
            const glm::vec2                    b    = ( tile.Origin + glm::vec2( tile.Side ) - m_Origin ) / cell;
            const auto x0 = static_cast<uint32_t>( std::clamp( std::floor( a.x ), 0.0f, float( grid - 1u ) ) );
            const auto z0 = static_cast<uint32_t>( std::clamp( std::floor( a.y ), 0.0f, float( grid - 1u ) ) );
            const auto x1 = static_cast<uint32_t>( std::clamp( std::ceil( b.x ), 0.0f, float( grid ) ) );
            const auto z1 = static_cast<uint32_t>( std::clamp( std::ceil( b.y ), 0.0f, float( grid ) ) );
            for ( uint32_t z = z0; z < z1; ++z )
                for ( uint32_t x = x0; x < x1; ++x )
                {
                    const glm::vec2 centre = m_Origin + ( glm::vec2( x, z ) + 0.5f ) * cell;
                    if ( centre.x >= tile.Origin.x && centre.x < tile.Origin.x + tile.Side &&
                         centre.y >= tile.Origin.y && centre.y < tile.Origin.y + tile.Side )
                        cells[kTileGridHeader + std::size_t{ z } * grid + x] = i + 1u;
                }
        }
        const RDG::BufferRef rowBuffer = graph.CreateBuffer( RDG::BufferDesc{ rows.size() * sizeof( glm::vec4 ) },
                                                             "Landscape.VirtualTexture.Rows" );
        graph.QueueBufferUpload( rowBuffer, Bytes( rows ) );
        const RDG::BufferRef tileBuffer = graph.CreateBuffer( RDG::BufferDesc{ cells.size() * sizeof( uint32_t ) },
                                                              "Landscape.VirtualTexture.Tiles" );
        graph.QueueBufferUpload( tileBuffer, Bytes( cells ) );
        const RDG::BufferRef jobBuffer = graph.CreateBuffer( RDG::BufferDesc{ jobs.size() * sizeof( glm::uvec4 ) },
                                                             "Landscape.VirtualTexture.Jobs" );
        graph.QueueBufferUpload( jobBuffer, Bytes( jobs ) );

        const auto&            layout = m_PageLayout.Get( m_PagePipeline->GetSpecification().Shader );
        ComputeNodeDeclaration node( graph.GetFrameArena() );
        node.Name = kPagePass;
        node.Access.Bindings( layout, Renderer::GetPipelineRouteFill( *m_PagePipeline ) )
             .Storage( PageShaderParameters::LandscapeVirtualRows, rowBuffer, RDG::Access::StorageRead )
             .Storage( PageShaderParameters::LandscapeVirtualJobs, jobBuffer, RDG::Access::StorageRead )
             .Storage( PageShaderParameters::LandscapeVirtualTiles, tileBuffer, RDG::Access::StorageRead )
             .Storage( PageShaderParameters::LandscapeVirtualPageTable, table, RDG::Access::StorageRead )
             .Storage( PageShaderParameters::LandscapeVirtualColorAtlas, color, RDG::Access::StorageWrite, 0u )
             .Storage( PageShaderParameters::LandscapeVirtualNormalAtlas, normal, RDG::Access::StorageWrite, 0u )
             .Storage( PageShaderParameters::LandscapeVirtualOcclusionAtlas, occlusion, RDG::Access::StorageWrite,
                       0u )
             .Storage( PageShaderParameters::LandscapeVirtualHeightAtlas, height, RDG::Access::StorageWrite, 0u );
        const auto count = static_cast<uint32_t>( jobs.size() );
        node.Record      = [pipeline = m_PagePipeline, count]( RDG::PassContext& context,
                                                          const FrameGraphRefs& ) -> Common::BoolResultStr
        {
            RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            return Renderer::DispatchCompute( bindings, *pipeline, kPageGroups, kPageGroups, count );
        };
        ComputeNodeList nodes = MakeComputeNodeList( graph.GetFrameArena() );
        nodes.push_back( std::move( node ) );
        if ( !AddComputeNodes( graph, textures, std::move( nodes ) ) )
        {
            // The pages this frame placed were never written: drop the layout, sample per pixel.
            m_ResourcesValid = false;
            m_LayoutValid    = false;
            m_Bindings       = {};
            DeclareDisabled( graph, textures );
            return Common::MakeFormattedError<bool>( "{}: the node could not be declared", kPagePass );
        }
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::System
