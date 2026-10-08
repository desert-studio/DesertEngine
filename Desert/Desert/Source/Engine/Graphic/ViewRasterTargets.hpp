#pragma once

#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/View/SceneViewState.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>

#include <cstdint>
#include <format>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Graphic
{
    // A colour attachment the GRAPH provides on an engine target (a per-view transient, never an image of the
    // engine framebuffer): appended after the framebuffer's own colours, at the slot the target's pipelines were
    // built for (SceneRenderer::SceneTargetLayout / GBufferLayout). @p Multisample is the attachment a
    // multisampled target draws into (resolved into @p Color); invalid on a single-sample target. @p Clear is the
    // slot's OWN clear value: its first declared writer of the frame clears it to this, every later one loads.
    // @p Resolve is how @p Multisample becomes @p Color (GraphColorResolve).
    //
    // HOW A MULTISAMPLED GRAPH COLOUR IS RESOLVED. Average: the render pass's own resolve attachment (the hardware
    // box filter) — right for colour, wrong for VECTOR DATA: the average of an edge's two motions is a motion no
    // surface has (UE resolves velocity by picking a sample, never by averaging). Vulkan resolves a float or unorm
    // colour attachment only by averaging (VK_RESOLVE_MODE_SAMPLE_ZERO is for depth/stencil and integer colour),
    // so SampleZero is a SHADER resolve: the slot gets no resolve attachment in the passes that draw it, and the
    // graph adds one "<Color>: Resolve" raster node (AddGraphColorResolves) that writes sample 0 of @p Multisample
    // into
    // @p Color — the pattern of "Scene: DepthResolve" for the multisampled scene depth.
    enum class GraphColorResolve : uint8_t
    {
        Average,
        SampleZero,
    };

    struct GraphColor
    {
        RDG::TextureRef   Color;
        RDG::TextureRef   Multisample;
        RDG::ClearValue   Clear;
        GraphColorResolve Resolve = GraphColorResolve::Average;
    };

    // Every attachment a raster node on an engine target declares: the framebuffer's (RDG::ImportedFramebuffer)
    // plus the graph-provided colours, with each colour slot's own clear value where it has one.
    struct RasterTargets : RDG::ImportedFramebuffer
    {
        std::vector<std::optional<RDG::ClearValue>> OwnClears; // by colour slot; nullopt = the pass's LoadOp
    };

    // Appends @p colors after the framebuffer's colours (at MSAA: the multisampled attachment as the colour, the
    // single-sample one as its resolve — or, for a SampleZero colour, NO resolve: an invalid ref at the slot,
    // which DeclareResolves skips; Resolves stays indexed by colour slot). False, nothing appended, when a
    // multisampled target lacks a multisampled attachment or a single-sample one is given one.
    inline bool AppendGraphColors( RasterTargets& targets, std::span<const GraphColor> colors, bool multisampled )
    {
        for ( const GraphColor& color : colors )
            if ( !color.Color.IsValid() || multisampled != color.Multisample.IsValid() )
                return false;
        targets.OwnClears.resize( targets.Colors.size() );
        for ( const GraphColor& color : colors )
        {
            targets.Colors.push_back( multisampled ? color.Multisample : color.Color );
            if ( multisampled )
                targets.Resolves.push_back( color.Resolve == GraphColorResolve::Average ? color.Color
                                                                                        : RDG::TextureRef{} );
            targets.OwnClears.emplace_back( color.Clear );
        }
        return true;
    }

    // Every in-pass resolve of @p resolves (by colour slot) on @p pass; a slot whose ref is invalid has none (a
    // SampleZero graph colour, resolved by its own node). Every raster node on an engine target declares its
    // resolves through this (DeferredFrameNodes::LoadTarget, which cannot include this header, skips the same
    // way).
    inline void DeclareResolves( RDG::PassBuilder& pass, std::span<const RDG::TextureRef> resolves )
    {
        for ( uint32_t slot = 0; slot < resolves.size(); ++slot )
            if ( resolves[slot].IsValid() )
                pass.ResolveTarget( slot, resolves[slot] );
    }

    // The shader resolves of @p colors (graph colours of ONE multisampled target): one "<Color>: Resolve" raster
    // node per SampleZero colour with a multisampled attachment, added where it is called — after the last node
    // drawing that target, before any reader of the resolved colour. The node's only target is @p Color (old
    // contents DontCare: every pixel is written); @p declare( pass, color ) declares its read of color.Multisample
    // (the renderer's binding block, SampledGraphics); @p record( color ) returns its exec. A single-sample target
    // (no Multisample) or an Average colour adds nothing.
    template <typename Declare, typename Record>
    void AddGraphColorResolves( RDG::Builder& graph, std::span<const GraphColor> colors, Declare&& declare,
                                Record&& record )
    {
        for ( const GraphColor& color : colors )
        {
            if ( color.Resolve != GraphColorResolve::SampleZero || !color.Multisample.IsValid() ||
                 !color.Color.IsValid() )
                continue;
            const auto        named = graph.GetTextureName( color.Color );
            const std::string name =
                 std::format( "{}: Resolve", named.IsSuccess() ? std::string( named.GetValue() ) : "GraphColor" );
            graph.AddPass(
                 name, RDG::PassFlags::Raster,
                 [&]( RDG::PassBuilder& pass )
                 {
                     declare( pass, color );
                     pass.ColorTarget( 0, color.Color, RDG::LoadOp::DontCare() );
                 },
                 record( color ) );
        }
    }

    // The LoadOp of every colour slot of one raster node: a slot with its own clear value CLEARS to it on its
    // first declared writer of the graph (@p started records the slots' textures already written) and LOADS after;
    // every other slot takes @p pass. Per slot, so one node clears scene colour to the sky grey and velocity to
    // zero.
    inline std::vector<RDG::LoadOp> ColorLoads( const RasterTargets& targets, const RDG::LoadOp& pass,
                                                std::set<uint32_t>& started )
    {
        std::vector<RDG::LoadOp> loads( targets.Colors.size(), pass );
        for ( std::size_t slot = 0; slot < targets.Colors.size(); ++slot )
        {
            if ( slot >= targets.OwnClears.size() || !targets.OwnClears[slot] )
                continue;
            if ( started.insert( targets.Colors[slot].Index ).second )
            {
                loads[slot].Action = RDG::LoadAction::Clear;
                loads[slot].Value  = *targets.OwnClears[slot];
            }
            else
                loads[slot] = RDG::LoadOp::Load();
        }
        return loads;
    }

    // The view's velocity of this graph (FrameTransients::Velocity): one transient at the render extent, shared by
    // the G-buffer (slot 4) and the scene target (slot 1), cleared to zero motion by its first writer, read as
    // kVelocityFaultDefault when every writer faulted. At MSAA the scene target draws into a multisampled twin
    // whose sample 0 "Velocity: Resolve" writes into it (GraphColorResolve::SampleZero).
    struct ViewVelocity
    {
        RDG::TextureRef Resolved;
        RDG::TextureRef Multisample; // invalid at one sample
    };

    // ONE extent source for a view's raster targets: the view's render extent (SceneRenderer::m_ViewExtent). The
    // scene target and the G-buffer are created and resized at it, and the velocity transient is created from it,
    // so the slot every target shares has the size of every target. A target at another extent is a defect the
    // frame is refused over: empty when @p target is at the view extent, else the named error.
    inline std::string ViewTargetExtentMismatch( const uint32_t viewWidth, const uint32_t viewHeight,
                                                 const std::string_view target, const uint32_t width,
                                                 const uint32_t height )
    {
        if ( width == viewWidth && height == viewHeight )
            return {};
        return std::format( "the {} is {}x{} but the view renders at {}x{}: the view's targets and its velocity "
                            "must share one extent",
                            target, width, height, viewWidth, viewHeight );
    }

    inline ViewVelocity CreateViewVelocity( RDG::Builder& graph, const RDG::Extent3D extent,
                                            const uint32_t samples )
    {
        RDG::TextureDesc desc;
        desc.Size   = extent;
        desc.Format = ViewTargetFormats::kVelocity;
        ViewVelocity velocity;
        velocity.Resolved = graph.CreateTexture( desc, "Velocity" );
        graph.SetFaultDefault( velocity.Resolved, kVelocityFaultDefault );
        if ( samples > 1 )
        {
            desc.Samples         = samples;
            velocity.Multisample = graph.CreateTexture( desc, "Velocity.Multisample" );
        }
        return velocity;
    }

    // The graph colour @p velocity is on a target of @p targetSamples (zero clear: no motion). The G-buffer is
    // always single-sample; the scene target takes the multisampled twin when it is multisampled, resolved by
    // SAMPLE 0 ("Velocity: Resolve"), never averaged: a motion vector is data, not a colour.
    inline GraphColor VelocityColor( const ViewVelocity& velocity, const uint32_t targetSamples )
    {
        return GraphColor{ velocity.Resolved, targetSamples > 1 ? velocity.Multisample : RDG::TextureRef{},
                           RDG::ClearValue{}, GraphColorResolve::SampleZero };
    }
} // namespace Desert::Graphic
