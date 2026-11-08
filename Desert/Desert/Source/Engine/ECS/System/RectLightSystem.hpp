#pragma once

#include "System.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/ECS/RectLightGeometry.hpp>

#include <Engine/Graphic/Render/Commands/PointLightCommand.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/gtc/constants.hpp>

namespace Desert::ECS
{
    // THE RECT LIGHTS' COLLECTOR (UE URectLightComponent -> FRectLightSceneProxy). A rect light is a local light
    // the renderer culls, shadows (a cube from its centre) and samples exactly as a point light, so it is handed
    // over as one - a PointLightPayload whose RectHalfWidth > 0 - and Mesh/LightSources.glslh DesertPointLightAt
    // resolves the rect for every pass that lights by it. One list, as UE's one local-light list.
    class RectLightECSSystem : public System
    {
    public:
        using System::System;

        // Render-data collector (read-only light collection) — safe to run concurrently with the other collectors.
        bool CanRunParallel() const override
        {
            return true;
        }

        void Update( entt::registry& registry, Graphic::Render::RenderCommandBuffer& renderCommandBuffer,
                     const Common::Timestep& /*ts*/ ) override
        {
            auto rectLightView = registry.view<RectLightComponent, TransformComponent>();

            rectLightView.each(
                 [&]( auto entity, const auto& rectLight, const auto& transform )
                 {
                     // A hidden light emits nothing (SpotLightSystem says why the outliner's eye is the switch).
                     if ( ECS::IsHidden( registry, entity ) )
                         return;

                     glm::mat4 worldTransform = transform.GetTransform();

                     entt::entity current = entity;
                     while ( registry.has<RelationshipComponent>( current ) )
                     {
                         const auto& rel = registry.get<RelationshipComponent>( current );
                         if ( rel.Parent == entt::null )
                             break;

                         current = rel.Parent;
                         if ( registry.has<TransformComponent>( current ) )
                         {
                             const auto& parentTransform = registry.get<TransformComponent>( current );
                             worldTransform = parentTransform.GetTransform() * worldTransform;
                         }
                     }

                     const RectLightData& data = rectLight.Data;
                     const float          width  = glm::max( data.SourceWidth, 0.0f );
                     const float          height = glm::max( data.SourceHeight, 0.0f );
                     // A rect with no area emits nothing (its luminance would be infinite over nothing).
                     if ( width <= 0.0f || height <= 0.0f )
                         return;

                     // The rect's frame and barn doors: ECS::RectLightGeometryOf, the one answer the editor's
                     // gizmo draws too.
                     const RectLightGeometry rect = RectLightGeometryOf( worldTransform, data.SourceWidth,
                                                                         data.SourceHeight, data.BarnDoorAngle,
                                                                         data.BarnDoorLength );

                     Graphic::ShaderProtocols::PointLightPayload light{};
                     light.Color     = data.Color;
                     light.Intensity = ECS::LightPhotometry::RectShaderLuminance(
                          data.Intensity, data.IntensityUnits, width, height );
                     light.Position  = rect.Centre;
                     light.Radius    = data.Range;
                     light.MinRadius = 0.0f;
                     light.Falloff   = static_cast<int32_t>( LightFalloff::InverseSquare );
                     // Traced shadows (MegaLights, the GI hit lighting) aim at the rect's equal-area disc.
                     light.SourceRadius   = glm::sqrt( width * height / glm::pi<float>() );
                     light.AxisX          = rect.AxisX;
                     light.RectHalfWidth  = rect.HalfWidth;
                     light.AxisY          = rect.AxisY;
                     light.RectHalfHeight = rect.HalfHeight;
                     light.RectBarnReach  = rect.BarnReach;
                     light.RectBarnDepth  = rect.BarnDepth;
                     // UE: contact shadows are part of the light's shadowing, so a light that casts none has none.
                     light.ContactShadowLength = data.CastShadows ? data.ContactShadowLength : 0.0f;
                     light.ContactShadowInWorld             = data.ContactShadowLengthInWorldSpace ? 1 : 0;
                     light.ContactShadowCastingIntensity    = data.ContactShadowCastingIntensity;
                     light.ContactShadowNonCastingIntensity = data.ContactShadowNonCastingIntensity;
                     light.VolumetricScatteringIntensity    = data.VolumetricScatteringIntensity;
                     Runtime::IesProfileService* const ies = Runtime::ResourceRegistry::GetIesProfileService();
                     light.IesProfileRow                   = ies->AcquireRow( data.IesProfile );

                     const Graphic::LocalLightShadowSettings shadow{
                          .CastShadows     = data.CastShadows,
                          .DepthBias       = data.ShadowBias,
                          .SlopeBias       = data.ShadowSlopeBias,
                          .ResolutionScale = data.ShadowResolutionScale,
                          .ShadowMethod    = data.ShadowMethod,
                          .SourceId        = static_cast<uint32_t>( entity ) + 1u };

                     renderCommandBuffer.Emplace<Graphic::Render::PointLightCommand>( light, shadow );
                 } );
        }
    };
} // namespace Desert::ECS
