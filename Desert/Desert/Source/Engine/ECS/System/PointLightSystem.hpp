#pragma once

#include "System.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EntityVisibility.hpp>

#include <Engine/Graphic/Render/Commands/PointLightCommand.hpp>

namespace Desert::ECS
{
    class PointLightECSSystem : public System
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

            auto pointLightView = registry.view<PointLightComponent, TransformComponent>();

            pointLightView.each(
                 [&]( auto entity, const auto& pointlight, const auto& transform )
                 {
                     // A hidden light emits NOTHING. It is a light, not a mesh, so nothing about it is
                     // "drawn" — but the outliner's eye is the only switch an artist has for killing one
                     // contribution while keeping the authored radius/colour, and a light that keeps lighting
                     // the room after its entity was hidden is the exact complaint that opened this task.
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

                     Graphic::ShaderProtocols::PointLightPayload light{};
                     light.Color     = pointlight.Data.Color;
                     // The authored value in its units, as the multiplier the shader's falloff scales (UE
                     // ULocalLightComponent::ComputeLightBrightness).
                     light.Intensity = ECS::LightPhotometry::ShaderIntensity(
                          pointlight.Data.Intensity, pointlight.Data.IntensityUnits, pointlight.Data.Falloff,
                          pointlight.Data.MinRadius, ECS::LightPhotometry::kPointLightCosHalfCone );
                     light.Position  = glm::vec3(worldTransform[3]); // Extract translation from world matrix
                     light.Radius    = pointlight.Data.Radius;
                     light.MinRadius = pointlight.Data.MinRadius;
                     light.SourceRadius = pointlight.Data.SourceRadius;
                     light.Falloff   = static_cast<int32_t>( pointlight.Data.Falloff );
                     // UE: contact shadows are part of the light's shadowing, so a light that casts none has none.
                     light.ContactShadowLength =
                          pointlight.Data.CastShadows ? pointlight.Data.ContactShadowLength : 0.0f;
                     light.ContactShadowInWorld          = pointlight.Data.ContactShadowLengthInWorldSpace ? 1 : 0;
                     light.ContactShadowCastingIntensity = pointlight.Data.ContactShadowCastingIntensity;
                     light.ContactShadowNonCastingIntensity = pointlight.Data.ContactShadowNonCastingIntensity;

                     // What the light asks of its shadow, and who it is (its tiles stay its own while it and
                     // its casters stand still: MeshRenderer::UpdateLocalShadows).
                     const Graphic::LocalLightShadowSettings shadow{
                          .CastShadows     = pointlight.Data.CastShadows,
                          .DepthBias       = pointlight.Data.ShadowBias,
                          .SlopeBias       = pointlight.Data.ShadowSlopeBias,
                          .ResolutionScale = pointlight.Data.ShadowResolutionScale,
                          .ShadowMethod    = pointlight.Data.ShadowMethod,
                          .SourceId        = static_cast<uint32_t>( entity ) + 1u };

                     renderCommandBuffer.Emplace<Graphic::Render::PointLightCommand>( light, shadow );
                 } );
        }
    };
} // namespace Desert::ECS