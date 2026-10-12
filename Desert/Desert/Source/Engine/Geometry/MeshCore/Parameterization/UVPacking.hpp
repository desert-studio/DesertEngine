// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Parameterization/UVPacking.h:20-63 and
// Private/Parameterization/UVPacking.cpp:24-946 (FUVIsland, FStandardChartPacker, FUVPacker::StandardPack,
// GetIslandStats), adapted: the multi-threaded look-ahead of FindBestPacking (only taken from 100k charts at
// 1000+ texels) is not ported; FUVSpaceAllocator's run/segment raster is replaced by UVRaster, a plain bit raster
// searched in order of the packer's own IsBestRect cost (X + W + Y + H), so the placement it picks is the one
// FindWithSegments picks; the flip orientations are rasterized rather than mirrored from the previous raster.
// StackPack is not ported (the Layout tool's Stack mode has no caller yet).
#pragma once

#include "Engine/Geometry/MeshCore/IndexTypes.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <functional>
#include <vector>

namespace Desert::Geometry
{
    class UVPacker
    {
    public:
        // What the packer reads and writes: a mesh's triangles, their UV triangles and the UV elements.
        class IUVMeshView
        {
        public:
            virtual ~IUVMeshView()                                              = default;
            [[nodiscard]] virtual Index3i    GetTriangle( int32_t TID ) const   = 0;
            [[nodiscard]] virtual Index3i    GetUVTriangle( int32_t TID ) const = 0;
            [[nodiscard]] virtual glm::dvec3 GetVertex( int32_t VID ) const     = 0;
            [[nodiscard]] virtual glm::vec2  GetUV( int32_t EID ) const         = 0;
            virtual void                     SetUV( int32_t EID, glm::vec2 UV ) = 0;
        };

        // Resolution of the packing raster: the gutter between islands is one texel of it.
        int32_t TextureResolution = 512;
        // Kept for UE's interface; the standard packer's gutter is the one-texel dilation of its raster.
        float GutterSize = 1.0f;
        // Islands keep their UV scale (packed into the unit square's frame, not scaled to fill it).
        bool bPreserveScale = false;
        // Islands keep their rotation (implies no flips).
        bool bPreserveRotation = false;
        // Islands may be mirrored in X and/or Y to pack tighter.
        bool bAllowFlips = false;
        // Islands are scaled so one texel covers about the same world length on every island.
        bool bScaleIslandsByWorldSpaceTexelRatio = false;

        /**
         * Packs NumIslands islands (CopyIsland(i, out) fills island i's triangles) into the positive unit square,
         * without overlap. False when no packing exists (more islands than texels, or no UV area).
         */
        bool StandardPack( IUVMeshView* Mesh, int32_t NumIslands,
                           const std::function<void( int32_t, std::vector<int32_t>& )>& CopyIsland );

        void GetIslandStats( IUVMeshView* Mesh, const std::vector<int32_t>& Island, glm::dvec2& IslandMinOut,
                             glm::dvec2& IslandMaxOut, double& IslandScaleFactorOut, double& UVAreaOut ) const;
    };
} // namespace Desert::Geometry
