#pragma once

#include <Engine/Graphic/ShaderProtocols/PointLight.hpp>
#include <Engine/Graphic/ShaderProtocols/SpotLight.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Desert::Graphic
{
    // THE VIEW'S LIGHT GRID (UE LightGrid: LightGridInjection.usf / LightGridCommon.ush). The frustum is cut into
    // froxels — square screen tiles times logarithmic depth slices — and a compute pass ("Deferred: LightGrid",
    // LightGridBuild.shader) lists in each the point and spot lights whose bounds touch it; the deferred composite
    // shades a pixel with the lights of its froxel only. The shader side is Mesh/LightGrid.glslh.
    //
    // Where this differs from UE: UE clamps a cell's list at r.Forward.MaxCulledLightsPerCell and silently drops
    // the lights past it. Here a cell keeps its TRUE count, and a pixel of a cell whose count exceeds the list is
    // shaded by every light — the capacity is a cost knob, never a correctness one.

    // The three quality values the grid is cut by (Scalability, Shadows group; SceneRenderer::BeginScene).
    struct LightGridQuality
    {
        uint32_t TilePixels       = 64; // tile edge in render pixels (UE r.Forward.LightGridPixelSize)
        uint32_t Slices           = 32; // depth slices (UE r.Forward.LightGridSizeZ)
        uint32_t MaxLightsPerCell = 32; // list capacity of one cell (UE r.Forward.MaxCulledLightsPerCell)
    };

    // std430 layout — must match `struct LightGridParams` in Mesh/LightGrid.glslh (192 bytes).
    struct LightGridParamsPayload
    {
        glm::mat4  View{ 1.0f };          // world -> view
        glm::mat4  InvProjection{ 1.0f }; // clip -> view, of the jittered projection
        glm::uvec4 Size{ 1u };            // tiles x, tiles y, slices, list capacity per cell
        glm::vec4  ZParams{ 0.0f };       // near, slices / log(far / near), far, tile edge in pixels
        glm::uvec4 Counts{ 0u };          // point lights, spot lights
        glm::vec4  Extent{ 0.0f };        // render target width, height in pixels
    };
    static_assert( sizeof( LightGridParamsPayload ) == 192 );

    [[nodiscard]] inline uint32_t LightGridCellCount( const LightGridParamsPayload& grid )
    {
        return grid.Size.x * grid.Size.y * grid.Size.z;
    }

    // Bytes of the cell buffer: one count per cell, then a list of Size.w indices per cell.
    [[nodiscard]] inline std::size_t LightGridCellBufferBytes( const LightGridParamsPayload& grid )
    {
        const std::size_t cells = LightGridCellCount( grid );
        return ( cells + cells * grid.Size.w ) * sizeof( uint32_t );
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
        const float zNear = std::max( nearPlane, 1.0f );
        float       zFar  = zNear * 2.0f;
        for ( const auto& light : points.PointLights )
            zFar = std::max( zFar, depthOf( light.Position ) + light.Radius );
        for ( const auto& light : spots.SpotLights )
            zFar = std::max( zFar, depthOf( light.Position ) + light.Range );

        const uint32_t tile   = std::max( quality.TilePixels, 1u );
        const uint32_t slices = std::max( quality.Slices, 1u );

        LightGridParamsPayload grid;
        grid.View          = view;
        grid.InvProjection = glm::inverse( jitteredProjection );
        grid.Size          = glm::uvec4( ( std::max( width, 1u ) + tile - 1u ) / tile,
                                         ( std::max( height, 1u ) + tile - 1u ) / tile, slices,
                                         std::max( quality.MaxLightsPerCell, 1u ) );
        grid.ZParams       = glm::vec4( zNear, static_cast<float>( slices ) / std::log( zFar / zNear ), zFar,
                                        static_cast<float>( tile ) );
        grid.Counts        = glm::uvec4( static_cast<uint32_t>( points.PointLights.size() ),
                                         static_cast<uint32_t>( spots.SpotLights.size() ), 0u, 0u );
        grid.Extent        = glm::vec4( static_cast<float>( width ), static_cast<float>( height ), 0.0f, 0.0f );
        return grid;
    }
} // namespace Desert::Graphic
