#pragma once

#include <Engine/Core/Formats/MaterialParamRow.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingHitGroupTable.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingScene.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneMetadata.hpp>

#include <Common/Core/ResultStr.hpp>

#include <algorithm>
#include <format>
#include <functional>
#include <iterator>
#include <span>
#include <string>

// THE BUILDER OF THE FRAME'S HIT GROUP TABLE (RayTracingHitGroupTable.hpp) from the one per-segment table: pure,
// device-free.
namespace Desert::Graphic::RayTracing
{
    // What one segment is shaded with in a hit: its surface template (the hit group) and its row of that
    // template's MaterialParams - the effective row the raster draws the section with (a landscape tile: the
    // TerrainRayHit program and LandscapeRayHitRow). An empty Template: the segment has no surface template (a
    // whole-entity shader override); it takes the default surface's group and row, and is counted
    // (SegmentsOnDefaultSurface).
    struct RayTracingSegmentSurface
    {
        std::string                     Template;
        Core::Formats::MaterialParamRow Row;
    };
    using RayTracingSegmentSurfaceFn = std::function<RayTracingSegmentSurface( const RayTracingSegmentRecord& )>;
    // Does surface template @p name have a compiled hit group (its "<name>/RayTracing.Hit" cell)?
    using RayTracingHasHitGroupFn = std::function<bool( const std::string& )>;

    // PURE. @p geometry and @p segments are the frame's two sides of the per-segment table
    // (RayTracingSceneRenderer::GetGeometryRecords / GetSegmentRecords) and must be the same length. A record
    // whose geometry row is empty (a run no TLAS instance points at, BuildRayTracingMetadata) is never hit: it
    // gets group 0 and no row. @p defaultSurface is the default surface template and its default row; it must have
    // a hit group. A segment whose template has none (@p hasHitGroup) takes the default surface, counted.
    [[nodiscard]] inline Common::ResultStr<RayTracingHitGroupTable> BuildRayTracingHitGroupTable(
         std::span<const RayTracingGeometryRecord> geometry, std::span<const RayTracingSegmentRecord> segments,
         const RayTracingSegmentSurfaceFn& surfaceOf, const RayTracingSegmentSurface& defaultSurface,
         const RayTracingHasHitGroupFn& hasHitGroup )
    {
        using Result = RayTracingHitGroupTable;
        if ( geometry.size() != segments.size() )
            return Common::MakeFormattedError<Result>(
                 "RayTracingHitGroupTable: {} geometry records for {} segment records - the two sides of the one "
                 "per-segment table disagree",
                 geometry.size(), segments.size() );
        if ( defaultSurface.Template.empty() )
            return Common::MakeError<Result>( "RayTracingHitGroupTable: no default surface template" );
        if ( !hasHitGroup( defaultSurface.Template ) )
            return Common::MakeFormattedError<Result>(
                 "RayTracingHitGroupTable: the default surface template '{}' has no compiled hit group - nothing to "
                 "fall back to",
                 defaultSurface.Template );

        Result table;
        table.Templates.push_back( defaultSurface.Template );
        table.Records.resize( segments.size() );
        const auto groupOf = [&table]( const std::string& name ) -> uint32_t
        {
            const auto it = std::find( table.Templates.begin(), table.Templates.end(), name );
            if ( it != table.Templates.end() )
                return static_cast<uint32_t>( it - table.Templates.begin() );
            table.Templates.push_back( name );
            return static_cast<uint32_t>( table.Templates.size() - 1u );
        };
        for ( std::size_t i = 0; i < segments.size(); ++i )
        {
            const RayTracingGeometryRecord& g      = geometry[i];
            RayTracingHitRecord&            record = table.Records[i];
            if ( g.VertexAddress == 0 )
                continue;
            RayTracingSegmentSurface surface = surfaceOf( segments[i] );
            if ( surface.Template.empty() )
            {
                surface = defaultSurface;
                ++table.SegmentsOnDefaultSurface;
            }
            else if ( std::ranges::find( table.Templates, surface.Template ) == table.Templates.end() )
            {
                auto missing = std::ranges::find( table.TemplatesWithoutHitGroup, surface.Template,
                                                  &RayTracingHitGroupFallback::Template );
                if ( missing != table.TemplatesWithoutHitGroup.end() || !hasHitGroup( surface.Template ) )
                {
                    if ( missing == table.TemplatesWithoutHitGroup.end() )
                    {
                        table.TemplatesWithoutHitGroup.push_back(
                             RayTracingHitGroupFallback{ .Template = surface.Template } );
                        missing = std::prev( table.TemplatesWithoutHitGroup.end() );
                    }
                    ++missing->Segments;
                    // The segment's row is laid out for its own template's parameters, so the default group
                    // takes the default row too.
                    surface = defaultSurface;
                    ++table.SegmentsOnFallbackHitGroup;
                }
            }
            record.HitGroup           = groupOf( surface.Template );
            record.Data.VertexAddress = g.VertexAddress;
            record.Data.IndexAddress  = g.IndexAddress;
            record.Data.VertexStride  = g.VertexStride;
            record.Data.FirstVertex   = g.FirstVertex;
            record.Data.FirstIndex    = g.FirstIndex;
            record.Data.NormalOffset  = g.NormalOffset;
            record.Data.UvOffset      = g.UvOffset;
            record.Data.MaterialRow   = static_cast<uint64_t>( table.Rows.size() * sizeof( glm::vec4 ) );
            table.Rows.insert( table.Rows.end(), surface.Row.begin(), surface.Row.end() );
        }
        return Common::MakeSuccess( std::move( table ) );
    }
} // namespace Desert::Graphic::RayTracing
