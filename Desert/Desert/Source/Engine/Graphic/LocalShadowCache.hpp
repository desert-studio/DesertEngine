#pragma once

#include <Engine/Graphic/LocalShadows.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>

namespace Desert::Graphic
{
    class Framebuffer;
    class Image2D;

    // THE SCENE'S CACHED SHADOW DEPTH OF ITS POINT AND SPOT LIGHTS (UE: FScene's FCachedShadowMapData, one per
    // light, read by every view of the scene). One atlas and one record per light, owned by Core::Scene and
    // handed to each of its views for the frame (SceneRenderer::BeginScene): a view lays out its own table of
    // the light's views (MeshRenderer::UpdateLocalShadows) over the tiles held here, and draws a light's tiles
    // only when what they hold is not the light it sees - so the six faces of a reflection capture and the main
    // view draw a still lamp's shadow once between them, not once each.
    //
    // WHO SIZES AND WHO DRAWS. A light's tile size follows the screen size of the light in a PRIMARY view (a
    // level's viewport, not a capture face): the largest primary request that is still being made holds it, and
    // a capture face reads whatever size is held (a face is no coarser than the viewport's map, so it loses no
    // quality). A capture face may draw a map where none is valid - it renders first in a frame - but that map is
    // provisional: a primary view draws it again once, because a caster's LOD is the drawing view's camera's
    // (MeshRenderer::BuildShadowCascadeDraws) and the viewport's look must be its own.
    class LocalShadowCache
    {
    public:
        struct Light
        {
            uint32_t AskedSize = 0; // the request it was sized for (tiles may be smaller)
            uint32_t ViewCount = 0; // 6 for a point light, 1 for a spot
            std::array<AtlasTile, kPointShadowFaces> Tiles{};
            // The primary view that sized it and the frame it last asked; null = a capture face sized it.
            const void* Sizer      = nullptr;
            uint64_t    SizerFrame = 0;
            // WHAT THE TILES HOLD: the light's views (matrices and rectangles) and its casters when they were
            // drawn. Casters nullopt = drawn with a caster that moves by itself, valid in the frame drawn only.
            bool                    Drawn = false;
            uint64_t                Views = 0;
            std::optional<uint64_t> Casters;
            uint64_t                DrawnFrame     = 0;
            bool                    DrawnByPrimary = false;
            uint64_t                LastFrame      = 0; // the last frame a view asked for it
        };

        LocalShadowCache();
        ~LocalShadowCache();
        LocalShadowCache( const LocalShadowCache& )            = delete;
        LocalShadowCache& operator=( const LocalShadowCache& ) = delete;

        // A VIEW OPENS ITS LIGHTS FOR THIS FRAME. On the first view of a new frame, last frame's verdicts land:
        // the atlas is made again at the side the primary views asked (a budget change), and the lights no view
        // asked for give their tiles back. The atlas is created on the first call that finds none, at
        // @p viewSide. Returns the atlas side every view lays its rows out against this frame.
        uint32_t BeginView( uint32_t viewSide, bool primary );

        [[nodiscard]] uint64_t Frame() const
        {
            return m_Frame;
        }
        [[nodiscard]] uint32_t Side() const
        {
            return m_Allocator.Side();
        }
        [[nodiscard]] Light* Find( uint64_t key );
        // Tiles for a light, halving @p askedSize until the pool has room for @p viewCount of one size; null when
        // even the smallest does not fit. The record starts undrawn.
        [[nodiscard]] Light* Allocate( uint64_t key, uint32_t askedSize, uint32_t viewCount );
        void                 Release( uint64_t key );

        [[nodiscard]] const std::shared_ptr<Framebuffer>& GetAtlas() const
        {
            return m_Atlas;
        }
        // The atlas's depth image (D32F), or null while no light of the scene has cast a shadow.
        [[nodiscard]] std::shared_ptr<Image2D> GetAtlasImage() const;

    private:
        void Create( uint32_t side );

        std::unordered_map<uint64_t, Light> m_Lights; // key: SourceId * 2 + (spot ? 1 : 0)
        ShadowAtlasAllocator                m_Allocator;
        std::shared_ptr<Framebuffer>        m_Atlas;
        uint64_t                            m_Frame            = ~0ull;
        uint32_t                            m_PrimarySideAsked = 0; // this frame's largest primary-view side
    };
} // namespace Desert::Graphic
