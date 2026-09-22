#pragma once

#include "System.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/Graphic/Render/Commands/DrawTerrainCommand.hpp>

#include <Common/Core/Logger.hpp>

#include <cstdint>
#include <unordered_set>
#include <utility>

namespace Desert::ECS
{
    // Collects every TerrainComponent entity each frame and forwards its transform + params to the
    // GPU TerrainRenderer (via a DrawTerrainCommand). Mirrors MeshECSSystem / SkyboxECSSystem.
    class TerrainECSSystem : public System
    {
    public:
        using System::System;

        // Render-data collector (read-only terrain collection) — safe to run concurrently with the other collectors.
        bool CanRunParallel() const override
        {
            return true;
        }

        void Update( entt::registry& registry, Graphic::Render::RenderCommandBuffer& renderCommandBuffer,
                     const Common::Timestep& /*ts*/ ) override
        {
            auto view = registry.view<TerrainComponent, TransformComponent>();
            for ( const auto entity : view )
            {
                if ( ECS::IsHidden( registry, entity ) )
                    continue;

                auto&       terrainComp = view.get<TerrainComponent>( entity );
                const auto& terrain     = terrainComp.Data;
                const auto& transform   = view.get<TransformComponent>( entity );

                // The terrain's material is a `.demat` like every other material: its values (Tint,
                // DetailTiling and the u_GrassTex/u_RockTex/u_SnowTex splat layers) arrive as named
                // overrides that TerrainRenderer applies on top of the shader's own schema defaults.
                //
                // It used to be an ECS::MaterialComponent authored in Details, which made the terrain the
                // last thing in the engine with a second way to edit a material. Unset resolves to nothing
                // and leaves the schema defaults standing, which is what an empty slot means.
                Graphic::MaterialOverrides overrides;
                if ( static_cast<uint64_t>( terrain.Material ) != 0 )
                {
                    auto* materialService = Runtime::ResourceRegistry::GetMaterialService();
                    if ( !materialService || !materialService->ResolveOverrides( terrain.Material, overrides ) )
                    {
                        // Not a silent default: a handle that resolves to nothing is a material the scene
                        // names and the asset database does not have, and the terrain then renders in the
                        // shader's defaults, which looks like an authoring mistake rather than a missing file.
                        // Said ONCE per handle — this runs every frame, and a warning repeated sixty times a
                        // second is how a log stops being read.
                        const uint64_t raw = static_cast<uint64_t>( terrain.Material );
                        if ( m_WarnedMissingMaterials.insert( raw ).second )
                        {
                            LOG_WARN( "[Terrain] material handle {} does not resolve to a registered "
                                      "material — the terrain renders with the Terrain shader's own "
                                      "defaults.",
                                      raw );
                        }
                    }
                }

                const glm::vec3 layerModes( static_cast<float>( terrain.GrassMode ),
                                            static_cast<float>( terrain.RockMode ),
                                            static_cast<float>( terrain.SnowMode ) );

                renderCommandBuffer.Emplace<Graphic::Render::DrawTerrainCommand>(
                     transform.GetTransform(), terrain.Size, terrain.Resolution, terrain.HeightScale,
                     terrain.NoiseFrequency, terrain.Seed, layerModes, terrainComp.SplatMap.get(),
                     std::move( overrides ) );
            }
        }

    private:
        // Handles already reported as unresolvable, so the warning above is said once and not once a frame.
        // Owned by the system and touched only from its own Update, which runs on exactly one thread per
        // frame even when the collector group runs in parallel.
        std::unordered_set<uint64_t> m_WarnedMissingMaterials;
    };
} // namespace Desert::ECS
