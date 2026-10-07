#pragma once

#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/View/SceneViewState.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>

#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace Desert::Graphic
{
    // A colour attachment the GRAPH provides on an engine target (a per-view transient, never an image of the
    // engine framebuffer): appended after the framebuffer's own colours, at the slot the target's pipelines were
    // built for (SceneRenderer::SceneTargetLayout / GBufferLayout). @p Multisample is the attachment a
    // multisampled target draws into (resolved into @p Color); invalid on a single-sample target. @p Clear is the
    // slot's OWN clear value: its first declared writer of the frame clears it to this, every later one loads.
    struct GraphColor
    {
        RDG::TextureRef Color;
        RDG::TextureRef Multisample;
        RDG::ClearValue Clear;
    };

    // Every attachment a raster node on an engine target declares: the framebuffer's (RDG::ImportedFramebuffer)
    // plus the graph-provided colours, with each colour slot's own clear value where it has one.
    struct RasterTargets : RDG::ImportedFramebuffer
    {
        std::vector<std::optional<RDG::ClearValue>> OwnClears; // by colour slot; nullopt = the pass's LoadOp
    };

    // Appends @p colors after the framebuffer's colours (at MSAA: the multisampled attachment as the colour, the
    // single-sample one as its resolve). False, nothing appended, when a multisampled target lacks a
    // multisampled attachment or a single-sample one is given one.
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
                targets.Resolves.push_back( color.Color );
            targets.OwnClears.emplace_back( color.Clear );
        }
        return true;
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
    // resolved into it.
    struct ViewVelocity
    {
        RDG::TextureRef Resolved;
        RDG::TextureRef Multisample; // invalid at one sample
    };

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
    // always single-sample; the scene target takes the multisampled twin when it is multisampled.
    inline GraphColor VelocityColor( const ViewVelocity& velocity, const uint32_t targetSamples )
    {
        return GraphColor{ velocity.Resolved, targetSamples > 1 ? velocity.Multisample : RDG::TextureRef{},
                           RDG::ClearValue{} };
    }
} // namespace Desert::Graphic
