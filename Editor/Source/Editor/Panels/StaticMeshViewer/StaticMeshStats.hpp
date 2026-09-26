#pragma once

#include <Engine/Assets/Serialization/Mesh.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

namespace Desert::Editor
{
    /**
     * @brief What the static mesh viewer prints about a mesh, computed from the SAME data the engine loads
     * (Serialization::MeshAssetData, read through Serialization::ReadMeshAssetData) and nothing else.
     *
     * Header-only and free of the renderer so that StaticMeshOutput can assert the numbers against the
     * committed probe; the window only formats them.
     *
     * LODS ARE THE BAKED CHAIN. A submesh carries its simplified triangle sets in SubmeshData::LODs
     * (coarsest last); LOD 0 is the full mesh. A mesh cooked without a chain has exactly one LOD here — the
     * runtime may still generate more at load, and the viewer says so rather than inventing a count.
     */
    struct StaticMeshStats
    {
        std::size_t Vertices  = 0;
        std::size_t Triangles = 0; // LOD 0
        std::size_t Sections  = 0; // submeshes: one draw and one material slot each
        // Triangles of each LOD, summed over the sections; [0] == Triangles. A section whose chain is shorter
        // than another's draws its coarsest baked level at the deeper LODs, so it is counted at that level.
        std::vector<std::size_t>          TrianglesPerLOD;
        std::optional<Common::Math::AABB> Bounds; // centimetres, the mesh's own space; nullopt = no sections

        [[nodiscard]] std::size_t LODs() const
        {
            return TrianglesPerLOD.size();
        }
    };

    [[nodiscard]] inline StaticMeshStats DescribeStaticMesh( const Assets::Serialization::MeshAssetData& data )
    {
        StaticMeshStats stats;
        stats.Vertices  = data.IsSkinned ? data.SkinnedVertices.size() : data.StaticVertices.size();
        stats.Triangles = data.Indices.size();
        stats.Sections  = data.Submeshes.size();
        stats.Bounds    = Assets::Serialization::MeshDataBounds( data );

        std::size_t chain = 0;
        for ( const auto& section : data.Submeshes )
            chain = std::max( chain, section.LODs.size() );

        stats.TrianglesPerLOD.assign( chain + 1, 0 );
        for ( const auto& section : data.Submeshes )
        {
            // IndexCount is in uint32 index units; the triangle array is in triplets.
            stats.TrianglesPerLOD[0] += section.IndexCount / 3u;
            for ( std::size_t lod = 1; lod <= chain; ++lod )
            {
                if ( section.LODs.empty() )
                    stats.TrianglesPerLOD[lod] += section.IndexCount / 3u;
                else
                    stats.TrianglesPerLOD[lod] += section.LODs[std::min( lod, section.LODs.size() ) - 1].size();
            }
        }
        return stats;
    }
} // namespace Desert::Editor
