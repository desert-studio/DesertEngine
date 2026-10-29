#include <Engine/GameplayDebugger/GameplayDebuggerCategoryRendering.hpp>

#if DESERT_DEV_INSTRUMENTS
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Graphic/PrimitiveReadout.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>

#include <array>
#include <format>
#include <string>

namespace Desert::Debug
{
    namespace
    {
        using Graphic::PrimitiveDrawReadout;

        const glm::vec4 kRenderingLabel( 0.65f, 0.65f, 0.65f, 1.0f );
        const glm::vec4 kRenderingValue( 1.0f, 1.0f, 1.0f, 1.0f );
        const glm::vec4 kRenderingYes( 0.35f, 1.0f, 0.35f, 1.0f );
        const glm::vec4 kRenderingNo( 1.0f, 0.45f, 0.35f, 1.0f );
        // One colour per draw record, for its bounds box and its title row.
        const std::array<glm::vec4, 4> kRenderingDrawColors{
             glm::vec4( 0.3f, 0.9f, 1.0f, 1.0f ), glm::vec4( 1.0f, 0.8f, 0.3f, 1.0f ),
             glm::vec4( 0.8f, 0.45f, 1.0f, 1.0f ), glm::vec4( 0.45f, 1.0f, 0.6f, 1.0f ) };

        void RenderingRow( GameplayDebuggerCategoryData& out, std::string label, std::string value,
                           const glm::vec4& valueColor = kRenderingValue )
        {
            out.AddTextLine( { { std::move( label ), kRenderingLabel }, { std::move( value ), valueColor } } );
        }

        const char* RenderingQueueName( const PrimitiveDrawReadout::Queue queue )
        {
            switch ( queue )
            {
                case PrimitiveDrawReadout::Queue::Static:
                    return "Static";
                case PrimitiveDrawReadout::Queue::Skinned:
                    return "Skinned";
                case PrimitiveDrawReadout::Queue::Generic:
                    return "Generic";
                case PrimitiveDrawReadout::Queue::Instanced:
                    return "Instanced";
            }
            return "?";
        }

        const char* RenderingPassName( const PrimitiveDrawReadout::Pass pass )
        {
            switch ( pass )
            {
                case PrimitiveDrawReadout::Pass::BasePass:
                    return "G-buffer base pass";
                case PrimitiveDrawReadout::Pass::Forward:
                    return "forward";
                case PrimitiveDrawReadout::Pass::Translucency:
                    return "translucency";
            }
            return "?";
        }

        const char* RenderingBlendName( const Core::Formats::SurfaceBlendMode blend )
        {
            switch ( blend )
            {
                case Core::Formats::SurfaceBlendMode::Opaque:
                    return "Opaque";
                case Core::Formats::SurfaceBlendMode::Masked:
                    return "Masked";
                case Core::Formats::SurfaceBlendMode::Translucent:
                    return "Translucent";
                case Core::Formats::SurfaceBlendMode::Additive:
                    return "Additive";
            }
            return "?";
        }

        const char* RenderingSunName( const Graphic::SunShadowReadout sun )
        {
            switch ( sun )
            {
                case Graphic::SunShadowReadout::None:
                    return "no sun shadow";
                case Graphic::SunShadowReadout::Cascaded:
                    return "CSM";
                case Graphic::SunShadowReadout::Virtual:
                    return "VSM";
                case Graphic::SunShadowReadout::RayTraced:
                    return "RT";
            }
            return "?";
        }

        std::string RenderingLodText( const PrimitiveDrawReadout& draw )
        {
            if ( draw.LodPerInstance )
                return std::format( "per instance, {} levels", draw.LodCount );
            if ( draw.Kind == PrimitiveDrawReadout::Queue::Skinned )
                return std::format( "0/{} (the skin pass draws LOD 0)", draw.LodCount );
            const std::string how = draw.ForcedLod >= 0 ? std::string( "forced" )
                                    : draw.LodBias != 0 ? std::format( "auto, bias {:+}", draw.LodBias )
                                                        : std::string( "auto" );
            return std::format( "{}/{} ({})", draw.Lod, draw.LodCount, how );
        }
    } // namespace

    void GameplayDebuggerCategoryRendering::CollectData( const Core::Scene& scene, const ECS::Entity* targetEntity,
                                                         GameplayDebuggerCategoryData& out )
    {
        const entt::registry& registry = scene.GetRegistry();
        if ( targetEntity == nullptr || !registry.valid( targetEntity->GetHandle() ) )
        {
            out.AddTextLine( { { "no target", kRenderingLabel } } );
            return;
        }
        Graphic::SceneRenderer* renderer = scene.GetSceneRenderer();
        if ( renderer == nullptr )
        {
            out.AddTextLine( { { "no view: the scene has no renderer", kRenderingLabel } } );
            return;
        }

        // The draw records are keyed by the entity's handle (MeshRenderData::Entity, the GpuScene key).
        const auto entity = static_cast<uint32_t>( targetEntity->GetHandle() );
        renderer->WatchPrimitive( entity );
        const Graphic::PrimitiveReadout* readout = renderer->GetPrimitiveReadout();
        if ( readout == nullptr || readout->Entity != entity )
        {
            out.AddTextLine( { { "waiting for the renderer (one frame)", kRenderingLabel } } );
            return;
        }
        if ( readout->Draws.empty() )
        {
            out.AddTextLine( { { "not drawn by the mesh renderer this frame (no mesh, hidden, or not queued)",
                                 kRenderingLabel } } );
            return;
        }

        const auto* tag = registry.try_get<ECS::TagComponent>( targetEntity->GetHandle() );
        RenderingRow( out, "Entity    ",
                      std::format( "{} - {} draw record(s), frame {}",
                                   tag != nullptr && !tag->Tag.empty() ? tag->Tag : std::string( "<untagged>" ),
                                   readout->Draws.size(), readout->Frame ) );

        for ( size_t i = 0; i < readout->Draws.size(); ++i )
        {
            const PrimitiveDrawReadout& draw  = readout->Draws[i];
            const glm::vec4&            color = kRenderingDrawColors[i % kRenderingDrawColors.size()];
            out.AddTextLine(
                 { { std::format( "[{}] {} queue, slot {}", i, RenderingQueueName( draw.Kind ), draw.SectionSlot ),
                     color } } );
            RenderingRow( out, "  LOD           ", RenderingLodText( draw ) );
            // The one draw path of this tree: the CPU walk of the queues (no GPU bins, no virtual geometry).
            RenderingRow( out, "  Draw path     ",
                          std::format( "CPU walk -> {}", RenderingPassName( draw.ShadingPass ) ) );
            RenderingRow( out, "  Material      ",
                          std::format( "{} ({})", draw.MaterialInstance.empty() ? "<none>" : draw.MaterialInstance,
                                       draw.Template.empty() ? "<no template>" : draw.Template ) );
            RenderingRow( out, "  Blend         ", RenderingBlendName( draw.Blend ) );
            RenderingRow( out, "  Shading model ",
                          draw.ShadingModel.empty() ? std::string( "<not a surface program>" )
                                                    : draw.ShadingModel );
            if ( draw.CastShadows )
                RenderingRow( out, "  Shadows       ",
                              std::format( "casts -> {}", RenderingSunName( readout->Sun ) ), kRenderingYes );
            else
                RenderingRow( out, "  Shadows       ", "does not cast", kRenderingNo );
            RenderingRow( out, "  In frustum    ", draw.InFrustum ? "yes" : "no (culled)",
                          draw.InFrustum ? kRenderingYes : kRenderingNo );
            if ( draw.Kind == PrimitiveDrawReadout::Queue::Instanced )
                RenderingRow( out, "  Instances     ", std::format( "{}", draw.Instances ) );
            else
                RenderingRow( out, "  GpuScene prim ", std::format( "{}", draw.GpuPrimitive ) );

            const glm::vec3 centre = 0.5f * ( draw.WorldBounds.Min + draw.WorldBounds.Max );
            const glm::vec3 half   = 0.5f * ( draw.WorldBounds.Max - draw.WorldBounds.Min );
            if ( half.x < 0.0f || half.y < 0.0f || half.z < 0.0f )
                continue; // no authored bounds: nothing to draw
            out.AddShape( { .Type        = GameplayDebuggerShape::Kind::Box,
                            .A           = centre,
                            .Extent      = half,
                            .Color       = color,
                            .Description = std::format( "[{}] LOD {}", i, draw.Lod ) } );
        }
    }
} // namespace Desert::Debug
#endif // DESERT_DEV_INSTRUMENTS
