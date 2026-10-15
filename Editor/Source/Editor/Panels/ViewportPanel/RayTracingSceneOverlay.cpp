#include "RenderMethodsOverlay.hpp"

#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneRenderer.hpp>
#include <Engine/Graphic/Systems/Scene/Terrain/LandscapeRayTracing.hpp>
#include <Engine/Localization/LocaleFormat.hpp>
#include <Engine/Localization/LocalizationService.hpp>

#include <cstddef>
#include <format>
#include <string>
#include <vector>

namespace Desert::Editor
{
    namespace
    {
        // Sizes in MiB with two decimals, counts as integers, both laid out by the current language's separators.
        std::string FormatMiB( const Localization::LocaleRow& locale, const uint64_t bytes )
        {
            constexpr double kMiB = 1024.0 * 1024.0;
            return std::format( "{} MiB",
                                Localization::FormatNumber( locale, static_cast<double>( bytes ) / kMiB, 2 ) );
        }
    } // namespace

    void DrawRayTracingSceneOverlay( const Graphic::RayTracing::RayTracingSceneStats&     stats,
                                     const Graphic::RayTracing::LandscapeRayTracingFrame& landscape,
                                     const ImVec2& /*viewportMin*/, const ImVec2& viewportMax )
    {
        if ( !stats.Built )
            return;

        using Graphic::RayTracing::RayTracingPrimitiveKind;
        using Graphic::RayTracing::RayTracingSkip;
        using Graphic::RayTracing::RayTracingPrimitiveKindName;
        using Graphic::RayTracing::RayTracingSkipName;
        const Localization::LocaleRow& locale = Localization::Localization::Get().Language();

        std::vector<std::string> rows;
        rows.emplace_back( "Ray tracing scene" );
        for ( std::size_t k = 0; k < stats.InstancesByKind.size(); ++k )
            rows.push_back(
                std::format( "  {}: {}", RayTracingPrimitiveKindName( static_cast<RayTracingPrimitiveKind>( k ) ),
                             Localization::FormatInteger( locale, stats.InstancesByKind[k] ) ) );
        const std::size_t firstSkip = rows.size();
        for ( std::size_t r = 0; r < stats.SkippedByReason.size(); ++r )
            rows.push_back(
                std::format( "  Skipped, {}: {}",
                             RayTracingSkipName( static_cast<RayTracingSkip>( r ) ),
                             Localization::FormatInteger( locale, stats.SkippedByReason[r] ) ) );
        const std::size_t endSkip = rows.size();
        rows.push_back( std::format( "  Traced BLAS: {}", FormatMiB( locale, stats.TracedBlasBytes ) ) );
        rows.push_back( std::format( "  Variant BLAS: {}", FormatMiB( locale, stats.VariantBlasBytes ) ) );
        rows.push_back( std::format( "  Procedural BLAS: {}", FormatMiB( locale, stats.ProceduralBlasBytes ) ) );
        rows.push_back( std::format( "  Skin cache BLAS: {}, positions: {}",
                                     FormatMiB( locale, stats.SkinCacheBlasBytes ),
                                     FormatMiB( locale, stats.SkinCachePositionsBytes ) ) );
        rows.push_back( std::format( "  Landscape tiles: {} (built {}, refit {}), vertices: {}, BLAS: {}",
                                     Localization::FormatInteger(
                                          locale, static_cast<int64_t>( landscape.Primitives.size() ) ),
                                     Localization::FormatInteger( locale, landscape.Built ),
                                     Localization::FormatInteger( locale, landscape.Refit ),
                                     FormatMiB( locale, landscape.VertexBytes ),
                                     FormatMiB( locale, landscape.BlasBytes ) ) );
        rows.push_back( std::format( "  TLAS: {}, instances: {}", FormatMiB( locale, stats.TlasBytes ),
                                     FormatMiB( locale, stats.TlasInstanceBytes ) ) );

        // Bottom-right, right-aligned: the render-methods rows hold the bottom-left, the axis triad the top-right.
        constexpr float kMargin = 12.0f;
        const float     lineH   = ImGui::GetTextLineHeight();
        ImDrawList*     dl      = ImGui::GetWindowDrawList();
        float           y       = viewportMax.y - kMargin - lineH * static_cast<float>( rows.size() );
        for ( std::size_t i = 0; i < rows.size(); ++i, y += lineH )
        {
            const ImVec2 at( viewportMax.x - kMargin - ImGui::CalcTextSize( rows[i].c_str() ).x, y );
            // A queued primitive the rays cannot see is drawn amber, like a lowered render method.
            const bool skipped = i >= firstSkip && i < endSkip && stats.SkippedByReason[i - firstSkip] > 0;
            // One dark tap behind the text instead of a plate, as the viewport's other labels: readable over a
            // white sky without hiding the scene in the corner.
            dl->AddText( ImVec2( at.x + 1.0f, at.y + 1.0f ), IM_COL32( 0, 0, 0, 200 ), rows[i].c_str() );
            dl->AddText( at, skipped ? IM_COL32( 240, 190, 80, 255 ) : IM_COL32( 225, 230, 240, 210 ),
                         rows[i].c_str() );
        }
    }
} // namespace Desert::Editor
