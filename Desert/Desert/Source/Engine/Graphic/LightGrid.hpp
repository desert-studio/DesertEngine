#pragma once

#include <Engine/Graphic/GlobalIllumination/RadianceCache.hpp>
#include <Engine/Graphic/ShaderProtocols/PointLight.hpp>
#include <Engine/Graphic/ShaderProtocols/SpotLight.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Desert::Graphic
{
    // THE LIGHT GRID (UE LightGrid: LightGridInjection.usf / LightGridCommon.ush). One buffer of point and spot
    // lights, one culling kernel ("LightGrid" / "LightGridWorld" compute nodes, LightGridBuild.shader), TWO
    // LAYOUTS of the cells it lists them in:
    //
    //   * FRUSTUM — the view's froxels (square screen tiles times logarithmic depth slices); the deferred
    //     composite and the forward meshes shade a pixel with the lights of its froxel only.
    //   * WORLD CLIPMAPS — the cells of the world radiance cache's clipmaps (Graphic/GlobalIllumination/
    //     RadianceCache.hpp: the same corners, cell sizes and resolution, MakeClipmaps), so a GI ray's hit — which
    //     lies anywhere around the camera, mostly off screen — finds the lights that reach it
    //     (Mesh/LightGrid.glslh, LightGridWorldLightsAt). The clipmaps move with the camera in whole cells.
    //
    // The shader side is Mesh/LightGrid.glslh. A layout is a different cell box and a different list storage; the
    // test of a light against a box, and the order lights are listed in, are the kernel's and the same for both.
    //
    // Where this differs from UE:
    //   * UE clamps a froxel's list at r.Forward.MaxCulledLightsPerCell and silently drops the lights past it.
    //   Here
    //     a cell keeps its TRUE count, and a pixel of a cell whose count exceeds the list is shaded by every light
    //     — the capacity is a cost knob, never a correctness one.
    //   * UE's off-screen light lookup (RayTracingLightGrid, r.RayTracing.LightGridResolution/MaxCount) is a
    //     separate grid with its own uniform cells around the camera, its own build and a per-cell cap. Here the
    //     off-screen layout is the same kernel over the radiance cache's nested clipmaps (fine near the camera,
    //     coarse far away — where the probes are), and its lists are COMPACT: the CPU bounds the total number of
    //     (light, cell) pairs exactly from the light spheres, so no cap and no list is ever dropped.

    // Which cells a grid lists its lights in (LIGHT_GRID_LAYOUT_* in Mesh/LightGrid.glslh).
    enum class LightGridLayout : uint32_t
    {
        Frustum       = 0,
        WorldClipmaps = 1,
    };

    // The three quality values the frustum grid is cut by (Scalability, Shadows group; SceneRenderer::BeginScene).
    struct LightGridQuality
    {
        uint32_t TilePixels       = 64; // tile edge in render pixels (UE r.Forward.LightGridPixelSize)
        uint32_t Slices           = 32; // depth slices (UE r.Forward.LightGridSizeZ)
        uint32_t MaxLightsPerCell = 32; // list capacity of one cell (UE r.Forward.MaxCulledLightsPerCell)
    };

    // std430 layout — must match `struct LightGridParams` in Mesh/LightGrid.glslh (288 bytes).
    struct LightGridParamsPayload
    {
        glm::mat4 View{ 1.0f };          // world -> the space cells are boxed in (frustum: view; world: identity)
        glm::mat4 InvProjection{ 1.0f }; // frustum: clip -> view, of the jittered projection; world: unused
        // frustum: tiles x, tiles y, slices, list capacity per cell;
        // world: resolution, resolution, resolution x clipmaps, pool capacity (indices)
        glm::uvec4 Size{ 1u };
        glm::vec4  ZParams{ 0.0f }; // frustum: near, slices / log(far / near), far, tile edge in pixels
        glm::uvec4 Counts{ 0u };    // point lights, spot lights, layout (LightGridLayout), world: clipmaps
        glm::vec4  Extent{ 0.0f };  // frustum: render target width, height in pixels
        // world: clipmap i's min corner (xyz, world cm) and cell size (w, cm) — RadianceCache::MakeClipmaps
        glm::vec4 Clipmaps[RadianceCache::kMaxClipmaps]{};
    };
    static_assert( sizeof( LightGridParamsPayload ) == 288 );

    [[nodiscard]] inline uint32_t LightGridCellCount( const LightGridParamsPayload& grid )
    {
        return grid.Size.x * grid.Size.y * grid.Size.z;
    }

    // Bytes of the cell buffer. Frustum: one count per cell, then a list of Size.w indices per cell. World: the
    // pool's allocator (one uint, zeroed by an upload each frame), one count per cell, one pool offset per cell,
    // then the pool of Size.w indices the cells' lists are packed into.
    [[nodiscard]] inline std::size_t LightGridCellBufferBytes( const LightGridParamsPayload& grid )
    {
        const std::size_t cells = LightGridCellCount( grid );
        if ( static_cast<LightGridLayout>( grid.Counts.z ) == LightGridLayout::WorldClipmaps )
            return ( 1u + 2u * cells + grid.Size.w ) * sizeof( uint32_t );
        return ( cells + cells * grid.Size.w ) * sizeof( uint32_t );
    }

    // PURE. The smallest sphere around a spot light's reach — LightGridBuild.shader's SpotBoundingSphere, term for
    // term (the CPU sizes the world pool with it, so it must bound exactly what the kernel tests). xyz = centre,
    // w = radius.
    [[nodiscard]] inline glm::vec4 SpotLightBoundingSphere( const ShaderProtocols::SpotLightPayload& light )
    {
        const glm::vec3 axis = glm::normalize( light.Direction );
        const float     c    = light.CosOuter;
        if ( c >= 0.70710678f )
        {
            const float r = light.Range / ( 2.0f * c );
            return glm::vec4( light.Position + axis * r, r );
        }
        if ( c > 0.0f )
            return glm::vec4( light.Position + axis * ( light.Range * c ),
                              light.Range * std::sqrt( 1.0f - c * c ) );
        return glm::vec4( light.Position, light.Range );
    }

    // PURE. The grid of one view: @p view / @p jitteredProjection = the camera matrices the G-buffer was
    // rasterised with, @p nearPlane its near plane (cm), @p width x @p height the render target in pixels. FAR is
    // the deepest point any light reaches, so every pixel behind it (lit by no light) can share the last slice.
    [[nodiscard]] inline LightGridParamsPayload
    MakeLightGridParams( const glm::mat4& view, const glm::mat4& jitteredProjection, float nearPlane,
                         uint32_t width, uint32_t height, const ShaderProtocols::PointLight& points,
                         const ShaderProtocols::SpotLight& spots, const LightGridQuality& quality )
    {
        const auto depthOf = [&view]( const glm::vec3& p ) { return -( view * glm::vec4( p, 1.0f ) ).z; };

        // The near edge of slice 1 is near * (far/near)^(1/slices); a near plane at or under 1 cm would give the
        // first slices a sliver of the view each, so the grid's own near is never under a centimetre.
        const float near = std::max( nearPlane, 1.0f );
        float       far  = near * 2.0f;
        for ( const auto& light : points.PointLights )
            far = std::max( far, depthOf( light.Position ) + light.Radius );
        for ( const auto& light : spots.SpotLights )
            far = std::max( far, depthOf( light.Position ) + light.Range );

        const uint32_t tile   = std::max( quality.TilePixels, 1u );
        const uint32_t slices = std::max( quality.Slices, 1u );

        LightGridParamsPayload grid;
        grid.View          = view;
        grid.InvProjection = glm::inverse( jitteredProjection );
        grid.Size          = glm::uvec4( ( std::max( width, 1u ) + tile - 1u ) / tile,
                                         ( std::max( height, 1u ) + tile - 1u ) / tile, slices,
                                         std::max( quality.MaxLightsPerCell, 1u ) );
        grid.ZParams       = glm::vec4( near, static_cast<float>( slices ) / std::log( far / near ), far,
                                        static_cast<float>( tile ) );
        grid.Counts        = glm::uvec4( static_cast<uint32_t>( points.PointLights.size() ),
                                         static_cast<uint32_t>( spots.SpotLights.size() ),
                                         static_cast<uint32_t>( LightGridLayout::Frustum ), 0u );
        grid.Extent        = glm::vec4( static_cast<float>( width ), static_cast<float>( height ), 0.0f, 0.0f );
        return grid;
    }

    // PURE. Cells of one clipmap (@p clipmap, @p resolution per side) that the sphere @p sphere can touch, one
    // cell of margin each way (the kernel pads every cell box by a thousandth of its size). 0 when the sphere
    // misses it.
    [[nodiscard]] inline uint64_t LightGridWorldCellsTouched( const RadianceCache::Clipmap& clipmap,
                                                              uint32_t resolution, const glm::vec4& sphere )
    {
        if ( sphere.w <= 0.0f )
            return 0u;
        const glm::vec3 lo =
             glm::floor( ( glm::vec3( sphere ) - sphere.w - clipmap.Corner ) / clipmap.CellSize ) - 1.0f;
        const glm::vec3 hi =
             glm::floor( ( glm::vec3( sphere ) + sphere.w - clipmap.Corner ) / clipmap.CellSize ) + 1.0f;
        const glm::vec3 last( static_cast<float>( resolution ) - 1.0f );
        const glm::vec3 first = glm::max( lo, glm::vec3( 0.0f ) );
        const glm::vec3 end   = glm::min( hi, last );
        if ( glm::any( glm::greaterThan( first, end ) ) )
            return 0u;
        const glm::vec3 span = end - first + 1.0f;
        return static_cast<uint64_t>( span.x ) * static_cast<uint64_t>( span.y ) * static_cast<uint64_t>( span.z );
    }

    // PURE. The world layout around @p cameraPosition: the cells of the radiance cache's clipmaps (@p cache — its
    // clipmap count, grid resolution and extents; MakeClipmaps snaps them to whole cells, so they move with the
    // camera and a cell keeps its world box while it stays inside). The pool holds every (light, cell) pair the
    // kernel can list: the sum over lights and clipmaps of the cells their bounding sphere's box covers.
    [[nodiscard]] inline LightGridParamsPayload
    MakeLightGridWorldParams( const glm::vec3& cameraPosition, const RadianceCache::RadianceCacheInputs& cache,
                              const ShaderProtocols::PointLight& points, const ShaderProtocols::SpotLight& spots )
    {
        const uint32_t clipmapCount = std::min( cache.Clipmaps, RadianceCache::kMaxClipmaps );
        const uint32_t resolution   = std::max( cache.GridResolution, 1u );
        const auto     clipmaps     = RadianceCache::MakeClipmaps( cameraPosition, cache );

        uint64_t   pool    = 0u;
        const auto touched = [&]( const glm::vec4& sphere )
        {
            for ( uint32_t i = 0; i < clipmapCount; ++i )
                pool += LightGridWorldCellsTouched( clipmaps[i], resolution, sphere );
        };
        for ( const auto& light : points.PointLights )
            touched( glm::vec4( light.Position, light.Radius ) );
        for ( const auto& light : spots.SpotLights )
            touched( SpotLightBoundingSphere( light ) );

        LightGridParamsPayload grid;
        grid.View   = glm::mat4( 1.0f );
        grid.Size   = glm::uvec4( resolution, resolution, resolution * clipmapCount,
                                  static_cast<uint32_t>( std::max<uint64_t>( pool, 1u ) ) );
        grid.Counts = glm::uvec4( static_cast<uint32_t>( points.PointLights.size() ),
                                  static_cast<uint32_t>( spots.SpotLights.size() ),
                                  static_cast<uint32_t>( LightGridLayout::WorldClipmaps ), clipmapCount );
        for ( uint32_t i = 0; i < clipmapCount; ++i )
            grid.Clipmaps[i] = glm::vec4( clipmaps[i].Corner, clipmaps[i].CellSize );
        return grid;
    }
} // namespace Desert::Graphic
