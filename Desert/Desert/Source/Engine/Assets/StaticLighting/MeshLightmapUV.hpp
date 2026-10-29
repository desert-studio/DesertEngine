#pragma once

#include <cstdint>

// LIGHTMASS CONTRACT (owner backlog LIGHTMASS, 10-21) — the mesh asset's lightmap UV. Header contract only.
//
// UE counterparts: FMeshBuildSettings::bGenerateLightmapUVs / SrcLightmapIndex / DstLightmapIndex /
// MinLightmapResolution, UStaticMesh::LightMapCoordinateIndex / LightMapResolution (FLayoutUV, the UV packer).
//
// A lightmap needs a UV channel in which no two triangles overlap and every chart has a texel of gutter at the
// mesh's resolution. Like UE, the mesh build GENERATES it by default (charts from the source UV channel, packed
// by a layout at MinLightmapResolution) into its own channel; an artist's authored channel can be named instead.
// The generated channel is mesh-derived data (MeshDerivedData, the DDC key includes these settings), never a
// second source of truth beside the source UVs.
//
// Q6 (lead, 10-22): EVERY LOD has its own lightmap UV channel and its own baked region (UE bakes each LOD). The
// mesh build generates the channel for each LOD from that LOD's own source UVs, at the LOD's resolution: LOD 0's
// LightmapResolution scaled by the LOD's screen-size ratio to LOD 0, rounded up to a multiple of 4, never below
// MinLightmapResolution / 4.
//
// WHERE THIS ENGINE DIFFERS: the settings live on the mesh asset's build settings block (beside the other
// MeshDerivedData inputs) and the per-entity override is ECS::StaticLightingPrimitiveSettings; UE spreads the
// resolution over three places (mesh, component override, the level's StaticLightingLevelScale).
namespace Desert::Assets
{
    struct MeshLightmapUVSettings
    {
        bool     GenerateLightmapUVs   = true; // UE bGenerateLightmapUVs
        uint8_t  SourceUVChannel       = 0;    // charts are cut from this channel (UE SrcLightmapIndex)
        uint8_t  LightmapUVChannel     = 1;    // where the result lives / which authored channel is used
        uint16_t MinLightmapResolution = 64;   // the packer's texel grid (UE MinLightmapResolution)
        uint16_t LightmapResolution    = 64;   // LOD 0's default region side for entities that do not override it
    };
} // namespace Desert::Assets
