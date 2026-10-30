#pragma once

#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace Desert::Geometry
{
    // THE SHAPE OF A MESH'S GENERATED LOD CHAIN (UE: FStaticMeshSourceModel::ReductionSettings.PercentTriangles
    // per LOD, the LOD count of the mesh's LOD group). Import data, one per mesh: MeshImportSettings::LodChain, in
    // the render-data key, so a change re-derives the chain. The screen size each level begins at is the import
    // record's (ImportRecordData::LodScreenSize, computed by Geometry::ComputeLODScreenSize where it states none)
    // - the chain says how coarse each level is, the record says when it is drawn.
    //
    // The default walks the triangle count down with the automatic screen sizes' area (each level from LOD 3 on
    // a quarter of the previous one's triangles, as the automatic screen size shrinks by 0.4 a level, 0.16 of the
    // area): a level keeps about the triangles per pixel the level before it had, and the last one is a few dozen
    // triangles - what a mesh a few pixels across can show (UE: the chain runs until the mesh is a card).
    struct LODChainSettings
    {
        // Each generated level's share of LOD 0's triangles, LOD 1 first: strictly decreasing, each in (0, 1).
        // Eight levels with LOD 0 (UE MAX_STATIC_MESH_LODS).
        std::vector<float> TriangleRatios{ 0.5f, 0.25f, 0.1f, 0.04f, 0.01f, 0.0025f, 0.001f };
        // THE COARSEST LEVEL'S SIZE: a level is never reduced below this many triangles, and the chain ends at the
        // first level reduced to it - a small mesh gets fewer levels, none coarser than a few dozen triangles.
        uint32_t MinTriangles = 24;

        bool operator==( const LODChainSettings& ) const = default;
    };

    // A chain the reduction can walk: at least one ratio, each finite, in (0, 1) and below the one before it, and
    // a floor of at least one triangle. Returns the defect, or empty.
    [[nodiscard]] inline std::string LODChainSettingsDefect( const LODChainSettings& chain )
    {
        if ( chain.TriangleRatios.empty() )
            return "states no level (LodPolicy None is the chain without levels)";
        for ( std::size_t level = 0; level < chain.TriangleRatios.size(); ++level )
        {
            const float ratio = chain.TriangleRatios[level];
            if ( !std::isfinite( ratio ) || ratio <= 0.0f || ratio >= 1.0f )
                return std::format( "LOD {}'s triangle ratio {} is not in (0, 1)", level + 1, ratio );
            if ( level > 0 && ratio >= chain.TriangleRatios[level - 1] )
                return std::format( "LOD {}'s triangle ratio {} is not below LOD {}'s {}", level + 1, ratio, level,
                                    chain.TriangleRatios[level - 1] );
        }
        if ( chain.MinTriangles == 0 )
            return "the coarsest level's floor is 0 triangles";
        return {};
    }

    // THE TRIANGLE TARGET OF EACH LEVEL @p chain generates for a submesh of @p baseTriangles (LOD 1 first): each
    // ratio's share, never below MinTriangles, ending at the first target at the floor; a target that cannot beat
    // the level before it ends the chain there. The one rule SimplifyLODLevels reduces by and AutoLODIndexTarget
    // sizes the geometry pool by.
    [[nodiscard]] inline std::vector<uint32_t> LODChainTargets( const uint32_t          baseTriangles,
                                                                const LODChainSettings& chain )
    {
        std::vector<uint32_t> targets;
        uint32_t              previous = baseTriangles;
        for ( const float ratio : chain.TriangleRatios )
        {
            const auto share  = static_cast<uint32_t>( static_cast<double>( baseTriangles ) * ratio );
            const auto target = share > chain.MinTriangles ? share : chain.MinTriangles;
            if ( target >= previous )
                break;
            targets.push_back( target );
            if ( target == chain.MinTriangles )
                break;
            previous = target;
        }
        return targets;
    }
} // namespace Desert::Geometry
