#pragma once

#include "System.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/Graphic/Clouds/CloudAuthoredPayload.hpp>
#include <Engine/Graphic/Render/Commands/VolumetricCloudCommand.hpp>

#include <Common/Core/Logger.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Desert::ECS
{
    /**
     * @brief Collects the scene's volumetric cloud layer for the frame.
     *
     * A render-data collector with exactly one piece of state of its own: the accumulated wind drift.
     * The GPU-owning half is Graphic::System::VolumetricCloudRenderer.
     *
     * IT RAN IN PARALLEL AND NO LONGER DOES, and the reason is a property of EnTT rather than of this
     * code — it is written down here because the next person to see `false` on a read-only collector will
     * otherwise assume it is a mistake.
     *
     * `registry.view<T...>()`, `has<T>()` and `try_get<T>()` all go through `basic_registry::assure<T>()`,
     * and assure MUTATES: it does `pools.resize( index + 1 )` and constructs a pool the first time a
     * component type is touched. `pools` is `mutable`, so the CONST overload mutates too and constness
     * buys nothing (ThirdParty/entt/include/entt/entt.hpp, `assure() const`). Two systems running
     * concurrently that both first-touch a type therefore race on a `std::vector` grow.
     *
     * Collecting hero clouds walks the entity's parents, which touches `RelationshipComponent` — a type
     * NO scene in this repository has a single instance of, so its pool is always created on the fly, by
     * whichever system asks first. MeshECSSystem does the same walk and also runs in parallel. Observed
     * once, headless, on the frame the scene finished loading: `std::length_error: vector` thrown out of
     * a vector grow, which is exactly what a torn resize produces.
     *
     * WHAT IT COSTS TO BE SERIAL: this collector reads one cloud component and at most four hero clouds,
     * and its whole body is a handful of matrix products. It is not measurable against a frame.
     *
     * THE GENERAL FIX IS NOT HERE. Every parallel system in this engine has the same hazard for any type
     * whose pool does not exist yet; the engine-wide answer is `registry.prepare<T>()` for the hierarchy
     * components before the parallel phase opens, in Core::Scene. That file belongs to another task.
     */
    class VolumetricCloudECSSystem final : public System
    {
    public:
        using System::System;

        bool CanRunParallel() const override
        {
            return false;
        }

        void Update( entt::registry& registry, Graphic::Render::RenderCommandBuffer& renderCommandBuffer,
                     const Common::Timestep& ts ) override
        {
            std::vector<entt::entity> entities;
            auto                      view = registry.view<ECS::VolumetricCloudComponent>();
            for ( const auto entity : view )
            {
                // A hidden layer entity is not a candidate for the election below — same rule, same
                // reason, as the fog's: with two layers authored, hiding the one that wins is how an
                // artist asks for the other. If it was the only one, `entities` is empty and the
                // `present = false` command above says so, which is what makes the layer disappear
                // instead of the renderer keeping last frame's.
                if ( ECS::IsHidden( registry, entity ) )
                    continue;
                entities.push_back( entity );
            }

            if ( entities.empty() )
            {
                // NO LAYER MEANS NO HERO CLOUDS EITHER, and it is said here rather than left implicit. A
                // sculpted body is a shape in the cloud FIELD, and without a layer there is no shell to
                // march, no envelope to sit inside and no lighting to be lit by — a body drawn anyway
                // would be a cloud with no sky, which is a picture nobody asked for.
                renderCommandBuffer.Emplace<Graphic::Render::VolumetricCloudCommand>(
                     false, ECS::VolumetricCloudData{}, glm::vec3( 0.0f ) );
                return;
            }

            // Lowest UUID drives the frame, exactly as the sky and the fog pick theirs. "Whichever entity
            // entt happened to visit first" changes when an unrelated component is added somewhere else,
            // and a sky that changes for no visible reason is a hard bug to even describe.
            size_t chosen = 0;
            for ( size_t i = 1; i < entities.size(); ++i )
            {
                if ( EntityId( registry, entities[i] ) < EntityId( registry, entities[chosen] ) )
                    chosen = i;
            }

            if ( entities.size() > 1 && !m_DuplicateLogged )
            {
                LOG_WARN( "[Clouds] {} Volumetric Cloud components in the scene; the one on entity '{}' "
                          "drives the frame (lowest id). The others are ignored.",
                          entities.size(), EntityName( registry, entities[chosen] ) );
                m_DuplicateLogged = true;
            }

            // COLLECTED FIRST, INTO A LOCAL, and never as an argument beside `clouds.Data`. The order in
            // which a call's arguments are evaluated is UNSPECIFIED, and collecting the hero clouds
            // touches the registry — `registry.view<T...>()` goes through EnTT's `assure<T>()`, which
            // resizes the pool vector the first time a component type is seen. Handing the layer's data
            // as a reference INTO the registry in the same argument list as a call that mutates the
            // registry is the shape of hazard this programme does not need a second instance of; the
            // component is copied out before anything else runs.
            const ECS::VolumetricCloudData data =
                 registry.get<ECS::VolumetricCloudComponent>( entities[chosen] ).Data;

            AdvanceWind( data, ts.GetSeconds() );

            std::vector<Graphic::HeroCloudInstance> heroClouds = CollectHeroClouds( registry );

            renderCommandBuffer.Emplace<Graphic::Render::VolumetricCloudCommand>( true, data, m_WindOffset,
                                                                                  std::move( heroClouds ) );
        }

    private:
        /**
         * The frame's sculpted bodies — slot A of the cloud field's seam.
         *
         * WHAT IS AND IS NOT DECIDED HERE. This is a COLLECTOR: it reads components and transforms and
         * emits them. It does not touch the asset manager, the modelling service or any GPU resource,
         * because an ECS system must not — `Runtime::ResourceRegistry` is renderer-only and says so at
         * its own declaration. Whether a volume is loaded, how big it is and whether the body fits inside
         * the layer are all the renderer's questions.
         *
         * A COMPONENT WITH NO VOLUME IS SKIPPED SILENTLY, and that is not a silent fallback: an empty
         * slot is an artist who has not chosen a body yet — the ordinary state of a component the moment
         * it is added — and it is not an error until something tries to draw it. A volume that is CHOSEN
         * and missing is an error, and Runtime::CloudModellingService is where it is logged with the
         * handle.
         *
         * THE CEILING IS ANNOUNCED ONCE. Four is Graphic::kCloudAuthoredSlots, a budget rather than a
         * structural limit (every instance costs the march a bounds test at every field sample, and the
         * shadow ray pays it thirty times over), so a scene with five is told which one was dropped
         * rather than left wondering why one cloud is missing.
         */
        std::vector<Graphic::HeroCloudInstance> CollectHeroClouds( entt::registry& registry )
        {
            std::vector<Graphic::HeroCloudInstance> instances;

            auto view = registry.view<ECS::HeroCloudComponent, ECS::TransformComponent>();

            size_t seen = 0;
            for ( const auto entity : view )
            {
                // A hidden body is not collected. Enabled is the AUTHORED switch on the component and
                // Visible is the outliner's; they are two different owners of the same decision and the
                // body has to satisfy both, exactly as a hidden mesh with a valid asset is still not drawn.
                if ( ECS::IsHidden( registry, entity ) )
                    continue;

                const auto& hero = registry.get<ECS::HeroCloudComponent>( entity );
                if ( !hero.Data.Enabled || hero.Data.Volume == Assets::AssetHandle::Null() )
                    continue;

                ++seen;
                if ( instances.size() >= Graphic::kCloudAuthoredSlots )
                    continue;

                Graphic::HeroCloudInstance instance;
                instance.Data           = hero.Data;
                instance.WorldTransform = WorldTransformOf( registry, entity );
                instance.Name           = EntityName( registry, entity );
                instances.push_back( std::move( instance ) );
            }

            if ( seen > Graphic::kCloudAuthoredSlots && !m_HeroOverflowLogged )
            {
                LOG_WARN( "[Clouds] {} hero clouds are enabled in the scene; the march carries {}. The "
                          "remaining {} are not drawn.",
                          seen, Graphic::kCloudAuthoredSlots, seen - Graphic::kCloudAuthoredSlots );
                m_HeroOverflowLogged = true;
            }

            return instances;
        }

        /// The entity's world matrix, parents composed. The same walk MeshECSSystem and PointLightSystem
        /// make, and it is here rather than through ECS::Entity::GetWorldTransform because that helper
        /// links against Core::Scene and this header is included by the runtime's own system list.
        static glm::mat4 WorldTransformOf( entt::registry& registry, entt::entity entity )
        {
            glm::mat4 world = registry.get<ECS::TransformComponent>( entity ).GetTransform();

            entt::entity current = entity;
            while ( registry.has<ECS::RelationshipComponent>( current ) )
            {
                const auto& relationship = registry.get<ECS::RelationshipComponent>( current );
                if ( relationship.Parent == entt::null )
                    break;

                current = relationship.Parent;
                if ( registry.has<ECS::TransformComponent>( current ) )
                    world = registry.get<ECS::TransformComponent>( current ).GetTransform() * world;
            }

            return world;
        }

        /**
         * The wind moves the SAMPLE POSITION rather than the data, so the drift is one accumulated vector
         * and costs nothing per sample. Two consequences worth stating, because both are easy to lose:
         *
         *   * The offset accumulates rather than being derived from an absolute clock. A clock would make
         *     the sky's position depend on how long the editor had been open, so two screenshots of the
         *     same scene would never match — and every visual comparison in this programme depends on
         *     them matching.
         *   * A zero-length direction leaves the sky still instead of producing a NaN. `normalize` of a
         *     zero vector is undefined, and the NaN would propagate into every sample position and render
         *     as a black sky with nothing in the log.
         */
        void AdvanceWind( const VolumetricCloudData& data, float seconds )
        {
            if ( !data.Enabled || data.WindSpeed <= 0.0f )
                return;

            const float lengthSquared = glm::dot( data.WindDirection, data.WindDirection );
            if ( lengthSquared <= 1e-12f )
                return;

            const glm::vec3 direction = data.WindDirection / glm::sqrt( lengthSquared );
            m_WindOffset += direction * ( data.WindSpeed * seconds );
        }

        static uint64_t EntityId( entt::registry& registry, entt::entity entity )
        {
            if ( auto* id = registry.try_get<ECS::UUIDComponent>( entity ) )
                return static_cast<uint64_t>( id->UUID );
            return 0;
        }

        static std::string EntityName( entt::registry& registry, entt::entity entity )
        {
            if ( auto* tag = registry.try_get<ECS::TagComponent>( entity ) )
                return tag->Tag;
            return "<unnamed>";
        }

        glm::vec3 m_WindOffset{ 0.0f };

        // Describe the SCENE, not the frame, so each is said once.
        bool m_DuplicateLogged    = false;
        bool m_HeroOverflowLogged = false;
    };
} // namespace Desert::ECS
