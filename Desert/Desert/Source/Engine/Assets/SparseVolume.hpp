#pragma once

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/DerivedDataCache.hpp>
#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace Desert::Assets
{
    // A BAKED SPARSE VOLUME (`.dsvol`) - UE's USparseVolumeTexture, reimplemented: a simulated smoke / fire grid
    // (density and temperature) stored as the bricks that hold any medium plus a page table over the whole grid,
    // so a 256^3 plume that fills a tenth of its box costs a tenth of a dense 3D texture. Read by
    // ECS::HeterogeneousVolumeComponent (DensitySource BakedSparse) by its GUID handle; the
    // renderer (Graphic::System::HeterogeneousVolumeRenderer) streams the bricks of the volumes on screen into one
    // shared atlas and the page tables into one buffer, and Common/HeterogeneousVolume.glslh HvBakedSample reads
    // them.
    //
    // LAYOUT. The grid is cut into pages of kSparseVolumeBrickSize^3 voxels. A page whose every voxel (and apron
    // voxel) is below the import threshold is EMPTY: no brick, page-table entry 0. Every other page stores one
    // brick of kSparseVolumeBrickPhysical^3 voxels - the page plus one voxel of apron on every side copied from
    // its neighbours, so the GPU's trilinear filter never reads outside the brick (UE's SVT tile border). A voxel
    // is RG16F: r = density / PeakDensity, g = temperature / PeakTemperature. Page-table entry = 1 + brick index.
    //
    // NO GPU HERE (the asset layer rule): this header is the format, the brick builder and the importer.
    inline constexpr uint32_t kSparseVolumeBrickSize     = 16u;
    inline constexpr uint32_t kSparseVolumeBrickPhysical = kSparseVolumeBrickSize + 2u;
    inline constexpr uint64_t kSparseVolumeBrickVoxels =
         uint64_t( kSparseVolumeBrickPhysical ) * kSparseVolumeBrickPhysical * kSparseVolumeBrickPhysical;
    // Two half floats per voxel.
    inline constexpr uint64_t kSparseVolumeBrickBytes = kSparseVolumeBrickVoxels * 4u;
    // A grid larger than this per axis is refused at import (the page table and the atlas would not be the
    // problem; a simulation export of this size is a units mistake).
    inline constexpr uint32_t kSparseVolumeMaxResolution = 2048u;

    inline constexpr uint32_t    kSparseVolumeContainerVersion = 1u;
    inline constexpr uint32_t    kSparseVolumeSubsystemTag     = Common::Content::FourCC( "DSVL" );
    inline constexpr const char* kSparseVolumeExtension        = ".dsvol";

    // The import's derived data (UE's DDC for SparseVolumeTexture builds): the encoded payload of a source grid,
    // keyed by the source bytes' content hash and the import settings. Bump Lo when the brick builder changes.
    inline constexpr Common::DDC::Deriver kSparseVolumeDeriver{
         "SparseVolume", ".svb", { 0x6a1f0c93d2b84e57ULL, 0x0000000000000001ULL } };

    struct SparseVolumeData
    {
        Common::Content::AssetGuid Guid;
        glm::uvec3                 Resolution{ 0u };       // voxels per axis of the source grid
        glm::uvec3                 Pages{ 0u };            // ceil( Resolution / kSparseVolumeBrickSize )
        float                      PeakDensity     = 1.0f; // the source density stored as 1
        float                      PeakTemperature = 0.0f; // K stored as 1; 0 = the grid has no temperature
        std::vector<uint32_t>      PageTable;              // Pages.x * Pages.y * Pages.z, x fastest
        std::vector<uint16_t>      Bricks; // BrickCount() bricks of kSparseVolumeBrickVoxels x (r, g) halves

        [[nodiscard]] uint32_t BrickCount() const
        {
            return static_cast<uint32_t>( Bricks.size() / ( kSparseVolumeBrickVoxels * 2u ) );
        }
        [[nodiscard]] uint64_t PageCount() const
        {
            return uint64_t( Pages.x ) * Pages.y * Pages.z;
        }
    };

    // A dense source grid as a simulation exports it: x fastest, then y, then z. Temperature empty = none.
    struct DenseVolumeGrid
    {
        glm::uvec3         Resolution{ 0u };
        std::vector<float> Density;
        std::vector<float> Temperature; // K
    };

    struct SparseVolumeImportSettings
    {
        // A voxel at or below this fraction of the peak density is empty medium (decides which pages are bricks).
        float EmptyThreshold = 1.0f / 512.0f;
    };

    // The Mitsuba / EmberGen / Houdini `.vol` grid ("VOL", version 3, float32 encoding, x fastest): channel 0 is
    // density, channel 1 (when the file has two or more) temperature in K. Refuses anything else by name.
    // OpenVDB / NanoVDB sources need their libraries (not vendored): see REMAINDER-VOLUMETRIC-SMOKE.
    Common::ResultStr<DenseVolumeGrid> ParseVolGrid( std::span<const unsigned char> file );

    // Bricks a dense grid: normalises by its peaks, drops the empty pages, copies the aprons.
    Common::ResultStr<SparseVolumeData> BuildSparseVolume( const DenseVolumeGrid&            grid,
                                                           const SparseVolumeImportSettings& settings );

    // ParseVolGrid + BuildSparseVolume through the DDC: the same source bytes and settings never brick twice.
    Common::ResultStr<SparseVolumeData> ImportSparseVolumeCached( std::span<const unsigned char>    source,
                                                                  const SparseVolumeImportSettings& settings );

    // The payload (header + page table + bricks) and the AF1 envelope around it.
    std::vector<unsigned char>          EncodeSparseVolumePayload( const SparseVolumeData& data );
    Common::ResultStr<SparseVolumeData> DecodeSparseVolumePayload( std::span<const unsigned char> bytes );
    Common::ResultStr<std::vector<unsigned char>> EncodeSparseVolume( const SparseVolumeData& data );
    Common::ResultStr<SparseVolumeData>           DecodeSparseVolume( const std::vector<unsigned char>& file );
} // namespace Desert::Assets
