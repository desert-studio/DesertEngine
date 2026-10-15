#pragma once

#include <Common/Core/Units.hpp>
#include <Engine/Graphic/ShadowCascades.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace Desert::Graphic
{
    // SHADOWS OF POINT AND SPOT LIGHTS, as pure math and bookkeeping — no renderer, no GPU.
    //
    // THE SHAPE IS UE's, ONE PAGE POOL FOR EVERY LOCAL VIEW. UE renders a spot light's shadow as one
    // perspective projection and a point light's as a cube (six 90-degree faces), and Virtual Shadow Maps put
    // every one of those views into ONE physical page pool, a 2D texture addressed through per-view tables. The
    // atlas below is that pool at the granularity of a whole tile: every local shadow VIEW (a spot, or one cube
    // face) owns one power-of-two square of one 2D depth texture, and the receiver reaches it through a row of
    // LocalShadowView (its matrix + its rectangle). Moving to virtual pages later changes what a row points at
    // (a page table instead of one rectangle), not the views, the passes or the lights.
    //
    // A cube face is a 2D view like any other, rather than a layer of a cube texture, for the same reason: the
    // pool is one 2D texture, so a point light's six faces are six tiles that can be sized, cached and
    // re-allocated like any spot's.
    //
    // EVERY DISTANCE IS IN WORLD UNITS (centimetres, Common::Units).

    // How many local shadow views one renderer holds per frame: six per point light, one per spot.
    inline constexpr uint32_t kMaxLocalShadowViews = 48;
    // A light's payload says "no shadow" with this view index (ShaderProtocols::PointLightPayload::ShadowView).
    inline constexpr int32_t  kNoLocalShadowView = -1;
    inline constexpr uint32_t kPointShadowFaces  = 6;

    // The smallest tile any view is given, and the floor the budget halves a request down to.
    inline constexpr uint32_t kMinLocalShadowTile = 64;
    // Texels of guard band on each side of a tile: the projection is widened by them so that the 3x3 PCF of a
    // receiver at a face's edge reads texels of its own view, never the neighbouring tile.
    inline constexpr float kLocalShadowGuardTexels = 2.0f;

    // The atlas side for a renderer's shadow budget: twice the cascade side (UE scales its local-light pool with
    // the same r.Shadow quality as the cascades). 0 = the renderer holds no shadow at all (kNoShadowQuality).
    [[nodiscard]] inline constexpr uint32_t LocalShadowAtlasSize( const uint32_t cascadeMapSize )
    {
        return cascadeMapSize * 2u;
    }

    // What a light asks of its shadow (the component's Shadows category) and who it is, for the cache.
    struct LocalLightShadowSettings
    {
        bool  CastShadows     = false;
        float DepthBias       = 0.5f; // the casters' depth bias, in texels at the caster (LocalCasterBias)
        float SlopeBias       = 0.5f; // texels per unit of the caster's slope to the view's axis
        float ResolutionScale = 1.0f;
        // The light's identity across frames (its entity + 1; 0 = none), so a light keeps its tiles and their
        // cached depth while it and its casters stand still.
        uint32_t SourceId = 0;

        bool operator==( const LocalLightShadowSettings& ) const = default;
    };

    // ONE ROW OF THE VIEW TABLE THE RECEIVER READS — std430, matching `struct LocalShadowView` in
    // Mesh/LocalLightShadow.glslh (112 bytes).
    struct LocalShadowViewPayload
    {
        glm::mat4 ViewProj{ 1.0f };  // world -> clip of this view (standard Z, [0,1])
        glm::vec4 AtlasRect{ 0.0f }; // xy = the tile's top-left in atlas UV, zw = its size in atlas UV
        glm::vec4 Params{ 0.0f };    // x = near, y = far, z = world size of one texel at unit distance, w = unused
        glm::vec4 Params2{ 0.0f };   // x = unused, y = 1 / atlas side, zw = unused
        // No depth bias here: the casters carry it (LocalCasterBias), the receiver only its normal offset.
    };
    static_assert( sizeof( LocalShadowViewPayload ) == 112 );

    // A 2D rectangle of the atlas, in texels.
    struct AtlasTile
    {
        uint32_t X    = 0;
        uint32_t Y    = 0;
        uint32_t Size = 0;

        bool operator==( const AtlasTile& ) const = default;
    };

    // THE ATLAS'S FREE SPACE: a buddy allocator over power-of-two squares. Persistent across frames on purpose:
    // a light that keeps its size keeps its tile, and therefore its cached depth (UE: a cached shadow map is
    // only valid where it was drawn). Freed squares merge with their buddy, so a pool emptied by lights leaving
    // is whole again.
    class ShadowAtlasAllocator
    {
    public:
        ShadowAtlasAllocator() = default;

        explicit ShadowAtlasAllocator( const uint32_t side ) : m_Side( side )
        {
            Reset( side );
        }

        void Reset( const uint32_t side )
        {
            m_Side = side;
            m_Free.clear();
            if ( side == 0 )
                return;
            m_Free.resize( LevelOf( kMinLocalShadowTile ) + 1 );
            m_Free[0].insert( { 0u, 0u } );
        }

        [[nodiscard]] uint32_t Side() const
        {
            return m_Side;
        }

        // A free square of exactly @p size texels (a power of two in [kMinLocalShadowTile, side]), or nullopt.
        [[nodiscard]] std::optional<AtlasTile> Allocate( const uint32_t size )
        {
            if ( m_Side == 0 || size < kMinLocalShadowTile || size > m_Side || !std::has_single_bit( size ) )
                return std::nullopt;
            const uint32_t want = LevelOf( size );
            // The smallest free square at least as large as asked, split down to the size.
            for ( int level = static_cast<int>( want ); level >= 0; --level )
            {
                auto& bucket = m_Free[static_cast<std::size_t>( level )];
                if ( bucket.empty() )
                    continue;
                auto [x, y] = *bucket.begin();
                bucket.erase( bucket.begin() );
                for ( uint32_t l = static_cast<uint32_t>( level ); l < want; ++l )
                {
                    const uint32_t half = SizeOf( l + 1 );
                    // Keep the top-left child, free the other three.
                    m_Free[l + 1].insert( { x + half, y } );
                    m_Free[l + 1].insert( { x, y + half } );
                    m_Free[l + 1].insert( { x + half, y + half } );
                }
                return AtlasTile{ x, y, size };
            }
            return std::nullopt;
        }

        void Free( const AtlasTile& tile )
        {
            if ( m_Side == 0 || tile.Size == 0 )
                return;
            uint32_t level = LevelOf( tile.Size );
            uint32_t x     = tile.X;
            uint32_t y     = tile.Y;
            while ( level > 0 )
            {
                const uint32_t                                     size = SizeOf( level );
                const uint32_t                                     px   = x - ( x % ( size * 2 ) );
                const uint32_t                                     py   = y - ( y % ( size * 2 ) );
                const std::array<std::pair<uint32_t, uint32_t>, 4> quad{
                     { { px, py }, { px + size, py }, { px, py + size }, { px + size, py + size } } };
                auto&      bucket   = m_Free[level];
                const bool siblings = std::all_of( quad.begin(), quad.end(), [&]( const auto& q )
                                                   { return q == std::pair{ x, y } || bucket.contains( q ); } );
                if ( !siblings )
                    break;
                for ( const auto& q : quad )
                    bucket.erase( q );
                x = px;
                y = py;
                --level;
            }
            m_Free[level].insert( { x, y } );
        }

    private:
        [[nodiscard]] uint32_t LevelOf( const uint32_t size ) const
        {
            return static_cast<uint32_t>( std::countr_zero( m_Side ) - std::countr_zero( size ) );
        }
        [[nodiscard]] uint32_t SizeOf( const uint32_t level ) const
        {
            return m_Side >> level;
        }

        uint32_t                                             m_Side = 0;
        std::vector<std::set<std::pair<uint32_t, uint32_t>>> m_Free; // per level, top-left corners
    };

    // THE TILE A LIGHT ASKS FOR (UE: the shadow's resolution follows the light's screen radius, scaled by
    // ShadowResolutionScale and clamped to the pool's maximum). @p screenRadiusPx is the light's influence
    // sphere projected on the view (larger than the view when the camera is inside it). A power of two in
    // [kMinLocalShadowTile, maxTile].
    [[nodiscard]] inline uint32_t LocalShadowTileSize( const float screenRadiusPx, const float resolutionScale,
                                                       const uint32_t maxTile )
    {
        if ( maxTile < kMinLocalShadowTile )
            return 0;
        const float wanted = std::max( 2.0f * screenRadiusPx * std::max( resolutionScale, 0.0f ), 1.0f );
        const auto  pow2   = std::bit_floor( static_cast<uint32_t>( std::min( wanted, 1.0e8f ) ) );
        return std::clamp( pow2, kMinLocalShadowTile, maxTile );
    }

    // How many pixels of the view the sphere (@p centre, @p radius) spans as a radius, for a perspective camera
    // at @p eye with vertical @p tanHalfFov and @p viewportHeight pixels. A camera inside the sphere gets the
    // whole view (it is surrounded by what this light lights).
    [[nodiscard]] inline float SphereScreenRadiusPx( const glm::vec3& centre, const float radius,
                                                     const glm::vec3& eye, const float tanHalfFov,
                                                     const float viewportHeight )
    {
        const float distance = glm::length( centre - eye );
        if ( distance <= radius || tanHalfFov <= 0.0f )
            return viewportHeight;
        return ( radius / ( distance * tanHalfFov ) ) * 0.5f * viewportHeight;
    }

    // Near plane of a local light's projection: a sliver of its reach, never under 2 cm (the standard-Z
    // perspective's precision is spent near the light, where contact shadows are).
    [[nodiscard]] inline float LocalShadowNear( const float reach )
    {
        return std::clamp( reach * 0.001f, Common::Units::Cm( 2.0f ), Common::Units::Cm( 20.0f ) );
    }

    // tan(half field of view) of a view @p tile texels wide covering @p tanHalfCore, widened by the guard band.
    [[nodiscard]] inline float GuardedTanHalf( const float tanHalfCore, const uint32_t tile )
    {
        const float t = static_cast<float>( tile );
        return tanHalfCore * t / std::max( t - 2.0f * kLocalShadowGuardTexels, 1.0f );
    }

    // THE CUBE FACE ORDER the receiver selects by (Mesh/LocalLightShadow.glslh, PointShadowFace): +X -X +Y -Y
    // +Z -Z, each looking along its axis. Standard-Z perspective (depth 0 at the near plane), so the same caster
    // pipeline and compare as the cascades.
    [[nodiscard]] inline glm::mat4 PointShadowFaceViewProj( const glm::vec3& position, const uint32_t face,
                                                            const float nearPlane, const float farPlane,
                                                            const uint32_t tile )
    {
        static const std::array<glm::vec3, kPointShadowFaces> kForward = {
             glm::vec3( 1, 0, 0 ),  glm::vec3( -1, 0, 0 ), glm::vec3( 0, 1, 0 ),
             glm::vec3( 0, -1, 0 ), glm::vec3( 0, 0, 1 ),  glm::vec3( 0, 0, -1 ) };
        static const std::array<glm::vec3, kPointShadowFaces> kUp = {
             glm::vec3( 0, 1, 0 ), glm::vec3( 0, 1, 0 ), glm::vec3( 0, 0, -1 ),
             glm::vec3( 0, 0, 1 ), glm::vec3( 0, 1, 0 ), glm::vec3( 0, 1, 0 ) };
        const float     tanHalf = GuardedTanHalf( 1.0f, tile );
        const glm::mat4 view    = glm::lookAtRH( position, position + kForward[face], kUp[face] );
        const glm::mat4 proj    = glm::perspectiveRH_ZO( 2.0f * std::atan( tanHalf ), 1.0f, nearPlane, farPlane );
        return proj * view;
    }

    // A spot light's one view: along @p direction (the cone's axis, pointing out of the apex), covering the
    // OUTER cone half-angle @p outerHalfAngle (radians) plus the guard band.
    [[nodiscard]] inline glm::mat4 SpotShadowViewProj( const glm::vec3& position, const glm::vec3& direction,
                                                       const float outerHalfAngle, const float nearPlane,
                                                       const float farPlane, const uint32_t tile )
    {
        const glm::vec3 forward = glm::normalize( direction );
        const glm::vec3 up      = std::abs( forward.y ) > 0.99f ? glm::vec3( 1, 0, 0 ) : glm::vec3( 0, 1, 0 );
        const float core = std::tan( std::clamp( outerHalfAngle, glm::radians( 1.0f ), glm::radians( 89.0f ) ) );
        const float tanHalf  = GuardedTanHalf( core, tile );
        const glm::mat4 view = glm::lookAtRH( position, position + forward, up );
        const glm::mat4 proj = glm::perspectiveRH_ZO( 2.0f * std::atan( tanHalf ), 1.0f, nearPlane, farPlane );
        return proj * view;
    }

    // The row the receiver reads for one view.
    [[nodiscard]] inline LocalShadowViewPayload LocalShadowRow( const glm::mat4& viewProj, const AtlasTile& tile,
                                                                const uint32_t atlasSide, const float nearPlane,
                                                                const float farPlane, const float tanHalf,
                                                                const LocalLightShadowSettings& settings )
    {
        const float            inv = 1.0f / static_cast<float>( atlasSide );
        LocalShadowViewPayload row;
        row.ViewProj  = viewProj;
        row.AtlasRect = glm::vec4( static_cast<float>( tile.X ) * inv, static_cast<float>( tile.Y ) * inv,
                                   static_cast<float>( tile.Size ) * inv, static_cast<float>( tile.Size ) * inv );
        row.Params    = glm::vec4( nearPlane, farPlane, 2.0f * tanHalf / static_cast<float>( tile.Size ), 0.0f );
        row.Params2   = glm::vec4( 0.0f, inv, 0.0f, 0.0f );
        return row;
    }

    // THE CASTERS' BIAS IN A POINT/SPOT LIGHT'S VIEW (UE: the spot/point light's ShadowBias and ShadowSlopeBias
    // applied by the shadow DEPTH pass, ShadowDepthVertexShader.usf; the receiver adds only its normal offset).
    // Every caster of the view takes it through the same two inputs a cascade's casters do — the constant part
    // in the caster matrix (CasterViewProj), the slope part per vertex (Common/ShadowCasterBias.glslh) — the mesh
    // cells and the terrain alike, so a caster drawn at another level of detail than the receiver (the terrain at
    // the light's LOD) is pushed back by the same rule as any other. The light's settings are in TEXELS at the
    // caster: one texel of a perspective map grows with the distance from the light, d * texelAtUnit, and a
    // clip-z shift k of a standard-Z perspective [n, f] lengthens the linear depth by d * k * (f - n) / (n f).
    // So k = texels * texelAtUnit * n f / (f - n) moves the caster back by that many texels at its own depth
    // (to first order in texels * texelAtUnit, a few thousandths at any tile size the atlas holds).
    [[nodiscard]] inline CascadeCasterBias LocalCasterBias( const float nearPlane, const float farPlane,
                                                            const float                     texelAtUnit,
                                                            const LocalLightShadowSettings& settings )
    {
        if ( farPlane <= nearPlane || nearPlane <= 0.0f || texelAtUnit <= 0.0f )
            return {};
        const float clipPerTexel = texelAtUnit * nearPlane * farPlane / ( farPlane - nearPlane );
        return { .DepthBias  = settings.DepthBias * clipPerTexel,
                 .SlopeBias  = settings.SlopeBias * clipPerTexel,
                 .SlopeClamp = kShadowMaxSlope };
    }
} // namespace Desert::Graphic
