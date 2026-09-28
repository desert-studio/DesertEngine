#pragma once

#include "System.hpp"

#include <Engine/World/Landscape/LandscapeGrass.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>

namespace Desert::Graphic
{
    class MaterialPBR;
    class MaterialInstance;
} // namespace Desert::Graphic

namespace Desert::ECS
{
    /**
     * @brief Landscape grass (UE: ALandscapeProxy::UpdateGrass over ULandscapeGrassType), generated around the
     * camera and never stored (owner decision O1).
     *
     * For every landscape root, every target layer whose `.delayerinfo` names a `.degrasstype`, and every
     * variety of that type, one GrassCellStreamer keeps the cells within the variety's EndCullDistance of the
     * camera; at most kGrassCellsPerFrame cells are generated per frame across ALL streamers. Each streamer's
     * instances are drawn through the existing instanced-static-mesh path (DrawInstancedStaticMeshCommand) —
     * no pass of its own. A paint or sculpt stroke drops the cells under it (LandscapeDirtyConsumer::Grass),
     * and they grow back from the new surface on the next frames.
     */
    class LandscapeGrassECSSystem : public System
    {
    public:
        /// UE's grass.MaxCreatePerFrame in spirit: the most cells generated in one frame, over every variety.
        static constexpr uint32_t kGrassCellsPerFrame = 4u;

        LandscapeGrassECSSystem();
        ~LandscapeGrassECSSystem() override;

        void SetCameraSnapshot( const glm::mat4& view, const glm::vec3& position ) override;

        void Update( entt::registry& registry, Graphic::Render::RenderCommandBuffer& renderCommandBuffer,
                     const Common::Timestep& ts ) override;

    private:
        /// (root UUID, layer info handle, variety index): one streamer each.
        using StreamerKey = std::tuple<uint64_t, uint64_t, uint32_t>;

        /// The instance a variety draws with: its `.demat` resolved once per material invalidation, or the
        /// default PBR instance for a variety that names none. Null: the material did not resolve (said once).
        std::shared_ptr<Graphic::MaterialInstance> VarietyMaterial( const std::string& materialGuid );

        std::map<StreamerKey, World::Landscape::GrassCellStreamer> m_Streamers;
        glm::vec3                                                  m_Camera = glm::vec3( 0.0f );
        std::shared_ptr<Graphic::MaterialPBR>                      m_Material;
        std::shared_ptr<Graphic::MaterialInstance>                 m_MaterialInstance;
        /// Material GUID text -> its runtime instance, dropped when the material service invalidates.
        std::map<std::string, std::shared_ptr<Graphic::MaterialInstance>> m_VarietyMaterials;
        uint32_t                                                          m_SeenMaterialsVersion = 0;
        /// The wind's clock (FO-7): the gameplay step the scene hands every system, summed like
        /// MeshECSSystem's, so grass and painted foliage sway on the same time.
        double m_WindSeconds = 0.0;
    };
} // namespace Desert::ECS
