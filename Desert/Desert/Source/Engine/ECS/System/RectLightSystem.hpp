#pragma once

#include "System.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EntityVisibility.hpp>

#include <Engine/Graphic/Render/Commands/PointLightCommand.hpp>

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

                     // The face looks along the entity's forward (-Z of the world rotation), as a spot's cone;
                     // width runs along the entity's X, and Y completes the frame so that X x Y is the face's
                     // normal (LightSources.glslh takes the cross as the emitting side).
                     const glm::vec3 forward = glm::normalize( -glm::vec3( worldTransform[2] ) );
                     const glm::vec3 axisX   = glm::normalize( glm::vec3( worldTransform[0] ) );
                     const glm::vec3 axisY   = glm::cross( forward, axisX );

                     // UE barn doors: hinged on the rect's four edges, BarnDoorAngle from the normal (90 = folded
                     // flat into the rect's plane, no effect), BarnDoorLength long. Their free edge as the shader
                     // clips by it: outward in the plane, and along the normal.
                     const float barnAngle = glm::radians( glm::clamp( data.BarnDoorAngle, 0.0f, 90.0f ) );
                     const float barnLength = glm::max( data.BarnDoorLength, 0.0f );

                     Graphic::ShaderProtocols::PointLightPayload light{};
                     light.Color     = data.Color;
                     light.Intensity = ECS::LightPhotometry::RectShaderLuminance(
                          data.Intensity, data.IntensityUnits, width, height );
                     light.Position  = glm::vec3( worldTransform[3] );
                     light.Radius    = data.Range;
                     light.MinRadius = 0.0f;
                     light.Falloff   = static_cast<int32_t>( LightFalloff::InverseSquare );
                     // Traced shadows (MegaLights, the GI hit lighting) aim at the rect's equal-area disc.
                     light.SourceRadius   = glm::sqrt( width * height / glm::pi<float>() );
                     light.RectAxisX      = axisX;
                     light.RectHalfWidth  = 0.5f * width;
                     light.RectAxisY      = axisY;
                     light.RectHalfHeight = 0.5f * height;
                     light.RectBarnReach  = barnLength * glm::sin( barnAngle );
                     light.RectBarnDepth  = barnLength * glm::cos( barnAngle );
                     // UE: contact shadows are part of the light's shadowing, so a light that casts none has none.
                     light.ContactShadowLength = data.CastShadows ? data.ContactShadowLength : 0.0f;
                     light.ContactShadowInWorld             = data.ContactShadowLengthInWorldSpace ? 1 : 0;
                     light.ContactShadowCastingIntensity    = data.ContactShadowCastingIntensity;
                     light.ContactShadowNonCastingIntensity = data.ContactShadowNonCastingIntensity;
                     light.VolumetricScatteringIntensity    = data.VolumetricScatteringIntensity;

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
