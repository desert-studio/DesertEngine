#pragma once

#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Graphic/ShadowCascades.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Graphic
{
    /**
     * @brief WHAT ONE VIEW (one SceneRenderer) HOLDS ON THE DEVICE — its profile and its render-target census.
     *
     * WHY A PROFILE. A SceneRenderer is built for two different jobs: a viewport of a level, and a preview
     * (asset thumbnail, the Details material ball, a UI element hosting a world). Before this, the only
     * per-view budget was the shadow quality; volumetric clouds, SSR and RSM-GI were gated by SCENE settings
     * read every frame, so a preview whose scene enabled clouds paid the cloud trace/history targets like a
     * level viewport. The profile is asked at CONSTRUCTION — the same reason ShadowQuality is (see
     * kNoShadowQuality): allocations happen in Init/Ensure*, and a per-scene flag toggled later is read by
     * nothing that allocates.
     *
     * WHY A CENSUS. A log line said "~245 bytes a pixel" and nothing could say WHICH targets made it up.
     * ViewTargetCensus lists every view-scaled and fixed per-view allocation by name, format and extent,
     * so the number has rows and a future format change (RGBA32F -> 16F) moves a pinned total in a test.
     * Every row cites the file that creates the target; a row whose format disagrees with its creator is
     * the defect this table exists to catch, so change them together.
     */
    struct ViewProfile
    {
        ShadowQuality Shadows;
        bool          VolumetricClouds       = true;
        bool          ScreenSpaceReflections = true;
        bool          GlobalIllumination     = true;

        bool operator==( const ViewProfile& ) const = default;
    };

    // A viewport of a level: every feature may be switched on by the scene.
    inline constexpr ViewProfile kSceneViewProfile{ kSceneShadowQuality, true, true, true };

    // A live preview (Details ball, mesh preview, UI render texture): one 1024 cascade, no SSR and no RSM-GI.
    // Clouds stay ON: a cloud material's preview IS its clouds (RT1b: with them off, all 53 cloud-material
    // thumbnails rendered an empty sky), and the cloud targets are only built when the preview scene has
    // clouds, so a preview without them pays nothing for the flag.
    inline constexpr ViewProfile kPreviewViewProfile{ kPreviewShadowQuality, true, false, false };

    // A one-shot capture (asset thumbnails, photogrammetry preview): the preview profile without the sun.
    inline constexpr ViewProfile kThumbnailViewProfile{ kNoShadowQuality, true, false, false };

    struct ViewTarget
    {
        std::string_view           Name;
        std::string_view           CreatedAt; // file that allocates it
        Core::Formats::ImageFormat Format;
        uint32_t                   Width          = 0;
        uint32_t                   Height         = 0;
        uint32_t                   Mips           = 1;
        uint32_t                   Count          = 1; // identical images (ping-pong pairs, cascades)
        bool                       ScalesWithView = true;

        [[nodiscard]] uint64_t Bytes() const
        {
            // One 2D image per level, asked of the format table rather than multiplied here, so a row whose
            // format is ever a block format is rounded to whole blocks like every other censused size.
            uint64_t total = 0;
            uint32_t w     = Width;
            uint32_t h     = Height;
            for ( uint32_t mip = 0; mip < Mips; ++mip )
            {
                total += Core::Formats::CalculateImageSize( w, h, Format );
                w = std::max( 1u, w / 2 );
                h = std::max( 1u, h / 2 );
            }
            return total * Count;
        }
    };

    namespace ViewMemoryDetail
    {
        [[nodiscard]] inline std::string_view FormatName( const Core::Formats::ImageFormat format )
        {
            using Core::Formats::ImageFormat;
            switch ( format )
            {
                case ImageFormat::RGBA8F:
                    return "RGBA8";
                case ImageFormat::RGBA16F:
                    return "RGBA16F";
                case ImageFormat::RGBA32F:
                    return "RGBA32F";
                case ImageFormat::DEPTH24STENCIL8:
                    return "D24S8";
                case ImageFormat::DEPTH32F:
                    return "D32F";
                default:
                    return "other";
            }
        }

        [[nodiscard]] inline uint32_t Div( const uint32_t extent, const uint32_t divisor )
        {
            return std::max( 1u, extent / divisor );
        }

        [[nodiscard]] inline uint32_t MipCount( uint32_t w, uint32_t h )
        {
            uint32_t levels = 1;
            while ( w > 1 || h > 1 )
            {
                w = std::max( 1u, w / 2 );
                h = std::max( 1u, h / 2 );
                ++levels;
            }
            return levels;
        }
    } // namespace ViewMemoryDetail

    /// Every target a view holds at @p width x @p height once each feature its profile allows has run once,
    /// single-sampled. Debug-view-only targets (overdraw) and UI-requested ones (backdrop blur) are left
    /// out: they are not what a view costs by being open.
    [[nodiscard]] inline std::vector<ViewTarget> ViewTargetCensus( const ViewProfile& profile,
                                                                   const uint32_t width, const uint32_t height )
    {
        namespace F = ViewTargetFormats;
        using ViewMemoryDetail::Div;
        using ViewMemoryDetail::MipCount;

        std::vector<ViewTarget> rows;
        const auto              add = [&]( const ViewTarget& row ) { rows.push_back( row ); };

        // SceneRenderer::EnsureRendererResources.
        add( { "SceneTarget.Color", "SceneRenderer.cpp", F::kSceneColor, width, height } );
        add( { "SceneTarget.Depth", "SceneRenderer.cpp", F::kSceneDepth, width, height } );
        add( { "GBufferA.AlbedoMetallic", "SceneRenderer.cpp", F::kGBufferA, width, height } );
        add( { "GBufferB.NormalRoughness", "SceneRenderer.cpp", F::kGBufferB, width, height } );
        add( { "GBufferC.WorldPosition", "SceneRenderer.cpp", F::kGBufferC, width, height } );
        add( { "GBuffer.Emissive", "SceneRenderer.cpp", F::kGBufferEmissive, width, height } );
        add( { "GBuffer.Depth", "SceneRenderer.cpp", F::kGBufferDepth, width, height } );
        add( { "SSAO", "SceneRenderer.cpp", F::kSSAO, width, height } );
        add( { "SceneColorCopy", "SceneRenderer.cpp", F::kSceneColorCopy, width, height } );

        // Post stack, all built in Init.
        add( { "SilhouetteMask", "MeshRenderer.cpp", F::kSilhouetteMask, width, height } );
        add( { "JFA.Seed+Output", "JumpFloodOutlineRenderer.cpp", F::kJFASeed, width, height, 1, 3 } );
        add( { "Tonemap", "TonemapRenderer.cpp", F::kTonemap, width, height } );
        add( { "FXAA", "FXAARenderer.cpp", F::kFXAA, width, height } );
        add( { "SMAA.Edges+Weights", "SMAARenderer.cpp", F::kSMAAEdges, width, height, 1, 2 } );
        add( { "SMAA.Blend", "SMAARenderer.cpp", F::kSMAABlend, width, height } );
        {
            const uint32_t bw = Div( width, 2 );
            const uint32_t bh = Div( height, 2 );
            add( { "Bloom.Chain", "BloomRenderer.cpp", F::kBloom, bw, bh,
                   std::min( 6u /* BloomRenderer::kMaxBloomMips */, MipCount( bw, bh ) ) } );
        }
        add( { "LightShaft.PingPong", "LightShaftRenderer.cpp", F::kLightShaft, Div( width, 2 ), Div( height, 2 ),
               1, 2 } );
        {
            const uint32_t sw = Div( width, 2 );
            const uint32_t sh = Div( height, 2 );
            add( { "LensFlare.Source", "LensFlareRenderer.cpp", F::kLensFlare, sw, sh,
                   std::min( 5u /* LensFlareRenderer::kMaxSourceMips */, MipCount( sw, sh ) ) } );
            add( { "LensFlare.Feature", "LensFlareRenderer.cpp", F::kLensFlare, Div( width, 4 ),
                   Div( height, 4 ) } );
        }
        add( { "HeightFog", "HeightFogRenderer.cpp", F::kHeightFog, width, height } );

        if ( profile.VolumetricClouds )
        {
            // VolumetricCloudRenderer::EnsureTraceTargets: trace pair at a quarter, history pairs at half.
            add( { "Clouds.Trace+Guide", "VolumetricCloudRenderer.cpp", F::kCloudTrace, Div( width, 4 ),
                   Div( height, 4 ), 1, 2 } );
            add( { "Clouds.History+Guide x2", "VolumetricCloudRenderer.cpp", F::kCloudTrace, Div( width, 2 ),
                   Div( height, 2 ), 1, 4 } );
        }
        if ( profile.ScreenSpaceReflections )
        {
            add( { "SSR.Trace", "SceneRenderer.cpp", F::kSSRTrace, width, height } );
            add( { "SSR.Accum x2", "SSRRenderer.hpp", F::kSSRAccum, width, height, 1, 2 } );
        }
        if ( profile.GlobalIllumination )
        {
            add( { "GI.Resolve", "SceneRenderer.cpp", F::kGIResolve, width, height } );
            add( { "GI.Accum x2", "GIResolveRenderer.hpp", F::kGIAccum, width, height, 1, 2 } );
            // SceneRenderer::kRSMResolution = 512, one row per attachment: they mirror the G-buffer's formats,
            // which need not all be the same.
            for ( ViewTarget rsm : { ViewTarget{ "RSM.Albedo", "SceneRenderer.cpp", F::kRSMAlbedo, 512, 512 },
                                     ViewTarget{ "RSM.Normal", "SceneRenderer.cpp", F::kRSMNormal, 512, 512 },
                                     ViewTarget{ "RSM.Position", "SceneRenderer.cpp", F::kRSMPosition, 512, 512 },
                                     ViewTarget{ "RSM.Emissive", "SceneRenderer.cpp", F::kRSMEmissive, 512, 512 },
                                     ViewTarget{ "RSM.Depth", "SceneRenderer.cpp", F::kRSMDepth, 512, 512 } } )
            {
                rsm.ScalesWithView = false;
                add( rsm );
            }
        }
        if ( profile.Shadows.CascadeCount != 0 )
        {
            // MeshRenderer::SetupShadowPass — see kShadowBytesPerTexel.
            const uint32_t s = profile.Shadows.ShadowMapSize;
            const uint32_t n = profile.Shadows.CascadeCount;
            ViewTarget     color{ "ShadowCascades.Color", "MeshRenderer.cpp", F::kShadowColor, s, s, 1, n };
            ViewTarget     depth{ "ShadowCascades.Depth", "MeshRenderer.cpp", F::kShadowDepth, s, s, 1, n };
            color.ScalesWithView = depth.ScalesWithView = false;
            add( color );
            add( depth );
        }
        return rows;
    }

    struct ViewMemoryTotals
    {
        uint64_t               ViewScaledBytes = 0;
        uint64_t               FixedBytes      = 0;
        [[nodiscard]] uint64_t Total() const
        {
            return ViewScaledBytes + FixedBytes;
        }
    };

    [[nodiscard]] inline ViewMemoryTotals SumViewTargets( const std::vector<ViewTarget>& rows )
    {
        ViewMemoryTotals totals;
        for ( const auto& row : rows )
            ( row.ScalesWithView ? totals.ViewScaledBytes : totals.FixedBytes ) += row.Bytes();
        return totals;
    }

    /// Bytes a pixel of the view costs, counting only view-scaled targets.
    [[nodiscard]] inline double ViewBytesPerPixel( const ViewProfile& profile, const uint32_t width,
                                                   const uint32_t height )
    {
        const auto totals = SumViewTargets( ViewTargetCensus( profile, width, height ) );
        return static_cast<double>( totals.ViewScaledBytes ) / ( static_cast<double>( width ) * height );
    }

    /// The census as a table, largest first — what the suite prints and the renderer logs once per build.
    [[nodiscard]] inline std::string FormatViewTargetCensus( std::vector<ViewTarget> rows, const uint32_t width,
                                                             const uint32_t height )
    {
        std::sort( rows.begin(), rows.end(),
                   []( const ViewTarget& a, const ViewTarget& b ) { return a.Bytes() > b.Bytes(); } );
        const auto mib = []( const uint64_t bytes ) { return static_cast<double>( bytes ) / ( 1024.0 * 1024.0 ); };
        std::string text = "view " + std::to_string( width ) + "x" + std::to_string( height ) + "\n";
        for ( const auto& row : rows )
        {
            char line[192];
            std::snprintf( line, sizeof( line ), "  %-26s %-8s %5ux%-5u x%u mips%u %9.2f MiB  %s\n",
                           std::string( row.Name ).c_str(),
                           std::string( ViewMemoryDetail::FormatName( row.Format ) ).c_str(), row.Width,
                           row.Height, row.Count, row.Mips, mib( row.Bytes() ),
                           std::string( row.CreatedAt ).c_str() );
            text += line;
        }
        const auto totals = SumViewTargets( rows );
        char       tail[160];
        std::snprintf( tail, sizeof( tail ),
                       "  total %.2f MiB = %.2f MiB view-scaled (%.1f B/px) + %.2f MiB fixed\n",
                       mib( totals.Total() ), mib( totals.ViewScaledBytes ),
                       static_cast<double>( totals.ViewScaledBytes ) / ( static_cast<double>( width ) * height ),
                       mib( totals.FixedBytes ) );
        text += tail;
        return text;
    }
} // namespace Desert::Graphic
