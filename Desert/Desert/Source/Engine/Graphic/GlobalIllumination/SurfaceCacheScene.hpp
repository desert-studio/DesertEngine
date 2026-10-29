#pragma once

#include <Engine/Graphic/GlobalIllumination/SurfaceCache.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

// THE LUMEN SCENE, CPU side (GI-SURFACE-CACHE-2). Port of UE LumenSceneData.cpp (FLumenSceneData: the mesh cards
// of every primitive instance, their cards, the page table) and LumenSurfaceCacheAllocator.cpp
// (FLumenSurfaceCacheAllocator: physical pages + bins of sub-allocated elements), with the card resolution of
// LumenSceneData.cpp GetCardMaxResolution (texel density over the viewer's distance).
//
// The scene is fed every frame with the frame's mesh instances (MeshRenderer::GatherSurfaceCacheInstances): an
// instance it has not seen is ADDED (its cards enter the scene), one it no longer gets is REMOVED (its pages
// return to the allocator), one whose world changed MOVES (its cards follow; the captured texels stay valid,
// they are card space - only a change of the card's world SIZE asks for a new resolution). Then every card gets
// the resolution its distance asks for. Every card holds a LOW mip of the smallest card side, always (UE: the
// locked low-resolution allocation of each card, LumenSurfaceCacheSampling.ush MIN_CARD_RESOLUTION), so a ray hit
// always finds a surface, coarse or not; the atlas left over is spent on HIGH mips nearest first, and a card that
// does not fit takes the pages of the farthest high mips (UE: the high-resolution pages are the budget, the
// farthest pay). A low mip outranks every high mip: it evicts high mips, never another low mip.
// Finally the cards still to be captured are chosen, nearest first, and packed into the frame's capture atlas
// (UE LumenSceneCardCapture: a capture atlas of the frame's captures, copied into the surface cache).
namespace Desert::Graphic::SurfaceCache
{
    // A rectangle of the surface-cache atlas, texels.
    struct AtlasRect
    {
        uint32_t X      = 0;
        uint32_t Y      = 0;
        uint32_t Width  = 0;
        uint32_t Height = 0;
    };

    // UE FLumenSurfaceCacheAllocator. The atlas is cut into kPhysicalPageSize² physical pages; an element of a
    // full page takes one page, a smaller element (each side a power of two, kMinCardResolution..page) lives in a
    // BIN: a page cut into equal elements of that size, shared by every card wanting it. A thin card's 128 x 8
    // pages thus fill a page sixteen to one instead of wasting fifteen sixteenths of it.
    class SurfaceCacheAllocator
    {
    public:
        void Reset( uint32_t atlasSize );
        // @p width, @p height: powers of two in [kMinCardResolution, kPhysicalPageSize].
        [[nodiscard]] std::optional<AtlasRect> Allocate( uint32_t width, uint32_t height );
        void                                   Free( const AtlasRect& rect );
        [[nodiscard]] uint32_t                 FreePages() const
        {
            return static_cast<uint32_t>( m_FreePages.size() );
        }
        [[nodiscard]] uint32_t TotalPages() const
        {
            return m_PagesPerSide * m_PagesPerSide;
        }

    private:
        struct Bin
        {
            uint32_t              Page = 0;
            std::vector<uint32_t> FreeElements; // element indices, row-major within the page
        };
        uint32_t                                                  m_PagesPerSide = 0;
        std::vector<uint32_t>                                     m_FreePages; // a stack, lowest page on top
        std::map<std::pair<uint32_t, uint32_t>, std::vector<Bin>> m_Bins;      // by element size
    };

    // One mesh instance of the frame (MeshRenderer::GatherSurfaceCacheInstances). @p Key is stable while the
    // instance lives (entity + part for a static mesh, the batch and index for an instanced one); @p Source names
    // the instance in the gatherer's own list of the frame, so a capture can find its material.
    struct SceneInstance
    {
        uint64_t    Key  = 0;
        const void* Mesh = nullptr; // identity of the mesh, for the key check
        std::shared_ptr<const Assets::MeshCardsBuildData> Cards;
        glm::mat4                                         World{ 1.0f };
        uint32_t                                          Source = 0;
        // The surface's identity (its runtime materials): another value recaptures every card of the instance -
        // a material swap, or a material rebuilt because its texture landed (UE: a primitive's material change
        // invalidates its cards). CaptureReady: its material can draw this frame; a card of an instance that
        // cannot waits uncaptured instead of keeping a stand-in surface.
        uint64_t SurfaceKey   = 0;
        bool     CaptureReady = true;
    };

    // One card capture of the frame: the instance's mesh drawn through the card's orthographic camera into
    // @p CaptureRect of the frame's capture atlas, then each of the card's pages copied out of it.
    struct CardCapture
    {
        uint32_t  Source = 0;         // SceneInstance::Source of the frame
        glm::mat4 World{ 1.0f };      // the instance's world
        glm::mat4 View{ 1.0f };       // the card camera: on the card plane, looking against the card direction
        glm::mat4 Projection{ 1.0f }; // reversed-Z orthographic box of the card (1 = card plane, 0 = far side)
        glm::vec3 Eye{ 0.0f };
        AtlasRect CaptureRect; // texels of the capture atlas
    };

    // One page copy out of the capture atlas into the surface cache (std430, Common/SurfaceCache.glslh
    // SurfaceCardPageCopy). 32 bytes.
    struct CardPageCopy
    {
        glm::uvec4 Capture{ 0u };  // xy = the card's capture rectangle origin, zw = the card's resolution
        glm::uvec4 Physical{ 0u }; // xy = the page's atlas origin, z = its size (x | y << 16), w = card texel
                                   // offset of the page (x | y << 16)
    };
    static_assert( sizeof( CardPageCopy ) == 32 );

    // What the frame hands the GPU: the scene's records (rebuilt each frame, compact) and this frame's captures.
    struct SceneFrame
    {
        std::vector<CardPayload>      Cards;
        std::vector<CardPagePayload>  Pages;
        std::vector<MeshCardsPayload> MeshCards;
        std::vector<CardCapture>      Captures;
        std::vector<CardPageCopy>     Copies;
        // Indices into Pages relit this frame (the direct-lighting budget; every page captured this frame).
        std::vector<uint32_t> LightingPages;
        // The instance table (contract B3, SurfaceCache.hpp): one row per live instance, by its stable row.
        std::vector<InstanceRowPayload> InstanceRows;
        // The scene frame these records belong to (ConfirmCapturesExecuted takes it back).
        uint64_t Frame         = 0;
        uint32_t ResidentCards = 0;
        uint32_t CapturedCards = 0;
        uint32_t HighResCards  = 0; // cards showing their high mip
        // Cards the atlas could not give even their low mip (every high mip already evicted): the quality's
        // GlobalIllumination.SurfaceCacheAtlasSize is too small for the scene; their hits are lit in place.
        uint32_t MissingMinimumCards = 0;
    };

    class SurfaceCacheScene
    {
    public:
        // The resolution policy: the budget's density and bounds, the capture atlas side and the per-frame
        // capture cap. Changing the atlas side drops every allocation (and so every capture).
        void Configure( const SurfaceCacheBudget& budget, uint32_t captureAtlasSize,
                        uint32_t maxCapturesPerFrame );

        // One frame: add / remove / move instances, choose resolutions, allocate, pick captures. @p viewers: the
        // origins of every view of the scene this frame (UE: the Lumen scene update's ViewOrigins, up to
        // MAX_LUMEN_VIEW_ORIGINS): a card's distance - its resolution and its capture priority - is the nearest
        // viewer's, so the cards in front of a second viewport are captured as those of the first. Never empty.
        [[nodiscard]] SceneFrame Update( const std::vector<SceneInstance>& instances,
                                         std::span<const glm::vec3>        viewers );
        // The lit layers no longer hold the scene's light (a significant change of the directional lights,
        // GlobalIllumination::InvalidatesLightingHistory): the next Update relights every visible page in its
        // frame instead of one in DirectLightingUpdateFactor (UE: the scene lighting's full relight).
        void InvalidateLighting()
        {
            m_RelightAll = true;
        }

        // From the exec of the graph that copies frame @p frame's captures into the atlas (any thread): those
        // captures landed, their cards are the surface from the next Update on.
        void ConfirmCapturesExecuted( uint64_t frame );

        [[nodiscard]] uint32_t InstanceCount() const
        {
            return static_cast<uint32_t>( m_Instances.size() );
        }
        // The instance table row of the instance @p key (SceneInstance::Key), kNoInstance when the scene does not
        // hold it (no cards built): what the hardware tracer's TLAS writes as the instance custom index.
        [[nodiscard]] uint32_t RowOf( uint64_t key ) const
        {
            const auto it = m_Instances.find( key );
            return it != m_Instances.end() ? it->second.Row : kNoInstance;
        }

        // The rows by placement: PlacementKey( mesh, world ) of every instance the scene holds -> its row. What
        // the global distance field's objects (a mesh and its world, no entity) are listed under in its object
        // grid.
        [[nodiscard]] std::unordered_map<uint64_t, uint32_t> RowsByPlacement() const;
        // A mesh instance's placement: the mesh's identity and its world transform's bits.
        [[nodiscard]] static uint64_t PlacementKey( const void* mesh, const glm::mat4& world );

    private:
        // One resolution of a card: its pages in the atlas and whether they hold a capture.
        struct CardMip
        {
            uint32_t               ResX = 0, ResY = 0; // allocated resolution; 0 = not resident
            uint32_t               PagesX = 0, PagesY = 0;
            std::vector<AtlasRect> Pages; // PagesX * PagesY, row-major
            bool                   Captured = false;
            uint64_t CaptureFrame           = 0; // the frame whose graph captures it, until that graph executes
        };
        enum class ShownMip : uint8_t
        {
            None,
            Low,
            High
        };
        struct Card
        {
            Assets::CardBuildData Build;
            // World frame of the card (rotation rows X, Y, Z; Z = the direction the card faces), its world centre
            // and half size along those axes.
            glm::mat3 Axes{ 1.0f };
            glm::vec3 Center{ 0.0f };
            glm::vec3 Extent{ 0.0f };
            // The guaranteed minimum (the smallest card side on both sides) and the distance-driven resolution
            // above it. The GPU record shows the high mip once it holds a capture, the low mip otherwise.
            CardMip  Low;
            CardMip  High;
            ShownMip Shown    = ShownMip::None; // the mip the last frame's record showed lit
            bool     Ready    = true;           // the owning instance's CaptureReady this frame
            float    Distance = 0.0f;
            uint32_t WantX = 0, WantY = 0;
            // The high resolution asked at the last allocation: a card the full atlas placed smaller than it asked
            // keeps its pages until it asks for more than that, instead of being freed and recaptured every frame.
            uint32_t AskedX = 0, AskedY = 0;
        };
        struct Instance
        {
            const void*                                       Mesh = nullptr;
            std::shared_ptr<const Assets::MeshCardsBuildData> Build;
            glm::mat4                                         World{ 1.0f };
            std::vector<Card>                                 Cards;
            uint32_t                                          Source     = 0;
            uint64_t                                          SeenFrame  = 0;
            uint64_t                                          SurfaceKey = 0;
            uint32_t                                          Row        = kNoInstance; // the B3 table row
        };

        void                   PlaceCards( Instance& instance ) const;
        void                   FreeMip( CardMip& mip );
        void                   FreeCard( Card& card );
        [[nodiscard]] bool     AllocateMip( CardMip& mip, uint32_t resX, uint32_t resY );
        [[nodiscard]] uint32_t WantedLongSide( const Card& card ) const;
        [[nodiscard]] uint32_t AllocateRow();
        void                   FreeRow( uint32_t row );
        // The frame's instance table (SceneFrame::InstanceRows), after the records.
        void BuildInstanceTable( SceneFrame&                                       frame,
                                 const std::vector<std::pair<uint32_t, uint32_t>>& rowRecords ) const;

        SurfaceCacheBudget                     m_Budget;
        uint32_t                               m_CaptureAtlasSize    = 0;
        uint32_t                               m_MaxCapturesPerFrame = 0;
        SurfaceCacheAllocator                  m_Allocator;
        std::unordered_map<uint64_t, Instance> m_Instances;
        uint64_t                               m_Frame = 0;
        bool                                   m_RelightAll = false; // InvalidateLighting, consumed by Update
        // The B3 table's rows: the count ever handed out and the freed ones, reused lowest first.
        uint32_t              m_RowCount = 0;
        std::vector<uint32_t> m_FreeRows;
        // Frames whose capture copy executed, not yet folded into the cards (ConfirmCapturesExecuted).
        static constexpr uint64_t kCaptureConfirmFrames = 4;
        std::mutex                m_ExecutedMutex;
        std::vector<uint64_t>     m_ExecutedCaptureFrames;
    };

    // PURE. The world frame of @p card of a mesh placed by @p world (UE FLumenCard::SetTransform): Z = the card's
    // direction, X and Y the other two mesh axes (right-handed), each normalised; @p extent = the card's half
    // size along them in world centimetres.
    void CardWorldFrame( const Assets::CardBuildData& card, const glm::mat4& world, glm::mat3& axes,
                         glm::vec3& center, glm::vec3& extent );
} // namespace Desert::Graphic::SurfaceCache
