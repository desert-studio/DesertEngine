#pragma once

#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Graphic/ShadowCascades.hpp>

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
        uint32_t                   Depth          = 1;
        uint32_t                   Mips           = 1;
        uint32_t                   Count          = 1; // identical images (ping-pong pairs, cascades)
        bool                       ScalesWithView = true;

        [[nodiscard]] uint64_t Bytes() const
        {
            uint64_t total = 0;
            uint32_t w = Width, h = Height;
            for ( uint32_t mip = 0; mip < Mips; ++mip )
            {
                total += static_cast<uint64_t>( w ) * h * Depth * Core::Formats::GetBytesPerPixel( Format );
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
        using Core::Formats::ImageFormat;
        using ViewMemoryDetail::Div;
        using ViewMemoryDetail::MipCount;

        std::vector<ViewTarget> rows;
        const auto              add = [&]( const ViewTarget& row ) { rows.push_back( row ); };

        // SceneRenderer::EnsureRendererResources.
        add( { "SceneTarget.Color", "SceneRenderer.cpp", ImageFormat::RGBA32F, width, height } );
        add( { "SceneTarget.Depth", "SceneRenderer.cpp", ImageFormat::DEPTH32F, width, height } );
        add( { "GBufferA.AlbedoMetallic", "SceneRenderer.cpp", ImageFormat::RGBA8F, width, height } );
        add( { "GBufferB.NormalRoughness", "SceneRenderer.cpp", ImageFormat::RGBA32F, width, height } );
        add( { "GBufferC.WorldPosition", "SceneRenderer.cpp", ImageFormat::RGBA32F, width, height } );
        add( { "GBuffer.Emissive", "SceneRenderer.cpp", ImageFormat::RGBA32F, width, height } );
        add( { "GBuffer.Depth", "SceneRenderer.cpp", ImageFormat::DEPTH32F, width, height } );
        add( { "SSAO", "SceneRenderer.cpp", ImageFormat::RGBA8F, width, height } );
        add( { "SceneColorCopy", "SceneRenderer.cpp", ImageFormat::RGBA32F, width, height } );

        // Post stack, all built in Init.
        add( { "SilhouetteMask", "MeshRenderer.cpp", ImageFormat::RGBA8F, width, height } );
        add( { "JFA.Seed+Output", "JumpFloodOutlineRenderer.cpp", ImageFormat::RGBA32F, width, height, 1, 1, 3 } );
        add( { "Tonemap", "TonemapRenderer.cpp", ImageFormat::RGBA32F, width, height } );
        add( { "FXAA", "FXAARenderer.cpp", ImageFormat::RGBA32F, width, height } );
        add( { "SMAA.Edges+Weights", "SMAARenderer.cpp", ImageFormat::RGBA8F, width, height, 1, 1, 2 } );
        add( { "SMAA.Blend", "SMAARenderer.cpp", ImageFormat::RGBA32F, width, height } );
        {
            const uint32_t bw = Div( width, 2 ), bh = Div( height, 2 );
            add( { "Bloom.Chain", "BloomRenderer.cpp", ImageFormat::RGBA32F, bw, bh, 1,
                   std::min( 6u /* BloomRenderer::kMaxBloomMips */, MipCount( bw, bh ) ) } );
        }
        add( { "LightShaft.PingPong", "LightShaftRenderer.cpp", ImageFormat::RGBA16F, Div( width, 2 ),
               Div( height, 2 ), 1, 1, 2 } );
        {
            const uint32_t sw = Div( width, 2 ), sh = Div( height, 2 );
            add( { "LensFlare.Source", "LensFlareRenderer.cpp", ImageFormat::RGBA16F, sw, sh, 1,
                   std::min( 5u /* LensFlareRenderer::kMaxSourceMips */, MipCount( sw, sh ) ) } );
            add( { "LensFlare.Feature", "LensFlareRenderer.cpp", ImageFormat::RGBA16F, Div( width, 4 ),
                   Div( height, 4 ) } );
        }
        add( { "HeightFog", "HeightFogRenderer.cpp", ImageFormat::RGBA16F, width, height } );

        if ( profile.VolumetricClouds )
        {
            // VolumetricCloudRenderer::EnsureTraceTargets: trace pair at a quarter, history pairs at half.
            add( { "Clouds.Trace+Guide", "VolumetricCloudRenderer.cpp", ImageFormat::RGBA16F, Div( width, 4 ),
                   Div( height, 4 ), 1, 1, 2 } );
            add( { "Clouds.History+Guide x2", "VolumetricCloudRenderer.cpp", ImageFormat::RGBA16F, Div( width, 2 ),
                   Div( height, 2 ), 1, 1, 4 } );
        }
        if ( profile.ScreenSpaceReflections )
        {
            add( { "SSR.Trace", "SceneRenderer.cpp", ImageFormat::RGBA32F, width, height } );
            add( { "SSR.Accum x2", "SSRRenderer.hpp", ImageFormat::RGBA32F, width, height, 1, 1, 2 } );
        }
        if ( profile.GlobalIllumination )
        {
            add( { "GI.Resolve", "SceneRenderer.cpp", ImageFormat::RGBA32F, width, height } );
            add( { "GI.Accum x2", "GIResolveRenderer.hpp", ImageFormat::RGBA32F, width, height, 1, 1, 2 } );
            // SceneRenderer::kRSMResolution = 512: albedo RGBA8, three RGBA32F, DEPTH32F.
            ViewTarget rsm8{ "RSM.Albedo", "SceneRenderer.cpp", ImageFormat::RGBA8F, 512, 512 };
            ViewTarget rsm32{
                 "RSM.Normal+Pos+Emissive", "SceneRenderer.cpp", ImageFormat::RGBA32F, 512, 512, 1, 1, 3 };
            ViewTarget rsmD{ "RSM.Depth", "SceneRenderer.cpp", ImageFormat::DEPTH32F, 512, 512 };
            rsm8.ScalesWithView = rsm32.ScalesWithView = rsmD.ScalesWithView = false;
            add( rsm8 );
            add( rsm32 );
            add( rsmD );
        }
        if ( profile.Shadows.CascadeCount != 0 )
        {
            // MeshRenderer::SetupShadowPass — see kShadowBytesPerTexel.
            const uint32_t s = profile.Shadows.ShadowMapSize, n = profile.Shadows.CascadeCount;
            ViewTarget color{ "ShadowCascades.Color", "MeshRenderer.cpp", ImageFormat::RGBA32F, s, s, 1, 1, n };
            ViewTarget depth{
                 "ShadowCascades.Depth", "MeshRenderer.cpp", ImageFormat::DEPTH24STENCIL8, s, s, 1, 1, n };
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
