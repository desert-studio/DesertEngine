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
                         if ( rel.Parent == entt::null ) break;

                         current = rel.Parent;
                         if ( registry.has<TransformComponent>( current ) )
                         {
                             const auto& parentTransform = registry.get<TransformComponent>( current );
                             worldTransform = parentTransform.GetTransform() * worldTransform;
                         }
                     }

                     Graphic::ShaderProtocols::PointLightPayload light{};
                     light.Color     = pointlight.Data.Color;
                     light.Intensity = pointlight.Data.Intensity;
                     light.Position  = glm::vec3(worldTransform[3]); // Extract translation from world matrix
                     light.Radius    = pointlight.Data.Radius;
                     light.MinRadius = pointlight.Data.MinRadius;
                     light.Falloff   = static_cast<int32_t>( pointlight.Data.Falloff );

                     renderCommandBuffer.Emplace<Graphic::Render::PointLightCommand>( light );
                 } );
        }
    };
} // namespace Desert::ECS