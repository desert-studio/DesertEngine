#pragma once

#include <Engine/Assets/Serialization/LandscapeLayerInfo.hpp>
#include <Engine/Graphic/Systems/Scene/Grass/GrassRenderer.hpp>
#include <Engine/World/Landscape/LandscapeLayout.hpp>

#include "System.hpp"

#include <Engine/Graphic/ResourceLedger.hpp>

#include <entt/entt.hpp>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

namespace Desert::Graphic
{
    class Image2D;
}

namespace Desert::World::Landscape
{
    class LandscapeTileData;
}

namespace Desert::ECS
{
    struct LandscapeTileComponent;

    /**
     * @brief Draws every loaded landscape tile, and owns the tiles' GPU copies.
     *
     * The tile's uint16 samples (LandscapeTileComponent::Heights) are the one source of the landscape's
     * heights (analysis A2); the GPU holds an R16_UNORM COPY of them, one image per tile, made here:
     *   - uploaded when the tile first appears, and again whenever it reports dirty rectangles (a new tile
     *     is wholly dirty, LandscapeData.hpp) or changes size — and whenever a NEIGHBOUR is edited, appears
     *     or goes away, because the copy carries a one-sample ring of each neighbour's next row (the normal
     *     at a seam is a central difference through it, LandscapeBorderedSamples);
     *   - a re-upload makes a NEW image rather than writing into the old one, because the old one may be
     *     bound by a frame still in flight; dropping it hands it to the allocator's per-frame deletion
     *     queue, which is what makes the release safe;
     *   - released when the entity, its tile component or its loaded heights go away.
     * Nothing is created per frame: a tile that did not change is drawn with the image it already has.
     *
     * Not parallel: it takes the tile's dirty list (a write) and creates GPU images.
     */
    class LandscapeECSSystem : public System
    {
    public:
        // @p scene: the ledger instance of the scene that runs this system (its RayTracingSceneCache's). The
        // tiles' GPU copies are that scene's rows, "scene #N" (RT-LEDGER-4, lead decision): derived from the
        // scene's components, held as long as the scene holds the tiles, shared by every view that draws them.
        explicit LandscapeECSSystem( Graphic::ResourceOwnerInstance scene ) : m_SceneLedger( scene )
        {
        }
        ~LandscapeECSSystem() override;

        void Update( entt::registry& registry, Graphic::Render::RenderCommandBuffer& renderCommandBuffer,
                     const Common::Timestep& ts ) override;

    private:
        struct TileGpu
        {
            // The R16 copy of the tile's samples plus the neighbour ring. Rewritten IN PLACE per edit and
            // recreated only when the sample count changes (LandscapeTileImage.hpp): its address keys the
            // tile's terrain material, so a new image per sculpt frame was a new material per frame (L8-leak).
            std::shared_ptr<Graphic::Image2D> Heightmap;
            uint32_t                          SamplesX = 0u;
            uint32_t                          SamplesZ = 0u;
            uint32_t                          NeighbourMask = 0u; // whose ring rows the copy carries
            // The RGBA8 copy of the tile's weight layers, pages stacked along the height
            // (LandscapeWeightmapAtlasTexels); null while it has none. WeightmapZ counts the stacked rows.
            // Updated IN PLACE per stroke: its address keys the tile's terrain material (TerrainTextureKey),
            // so a new image per stroke would be a new material per stroke.
            std::shared_ptr<Graphic::Image2D> Weightmap;
            uint32_t                          WeightmapX = 0u;
            uint32_t                          WeightmapZ = 0u;
            // Revisions of the two images' PIXELS (from m_Revision): bumped on every rewrite or recreation,
            // since an in-place rewrite keeps the address. The view's ray-tracing BLAS rebuilds on a change.
            uint64_t HeightRevision = 0u;
            uint64_t WeightRevision = 0u;
        };

        // Creates, rewrites in place, or drops the tile's weightmap copy to match its weight layers.
        void UpdateWeightmap( TileGpu& gpu, const World::Landscape::LandscapeTileData& heights,
                              entt::entity entity, const LandscapeTileComponent& tileComp, bool weightsDirty );

        Graphic::ResourceOwnerInstance            m_SceneLedger;
        std::unordered_map<entt::entity, TileGpu> m_Tiles;
        uint64_t                                  m_Revision = 0u; // the last revision handed out
        // Said once per tile, not once per frame: a tile that cannot be drawn stays that way until edited.
        std::unordered_set<entt::entity> m_Warned;
        // Weight-layer problems (a layer the root does not name, a weightmap that could not be created or
        // written), said once per tile until its weights are edited again.
        std::unordered_set<entt::entity> m_WarnedWeights;
        struct GrassTile
        {
            const LandscapeTileComponent* Tile  = nullptr;
            bool                          Dirty = false;
        };
        void EmitGrass( entt::registry& registry, Graphic::Render::RenderCommandBuffer& renderCommandBuffer,
                        uint64_t rootId, const World::Landscape::LandscapeRoot& root,
                        const std::vector<Assets::Serialization::LandscapeLayerInfoData>& layers,
                        const std::vector<GrassTile>&                                     tiles );

        // GRASS-GPU: per landscape root, the packed grass field (rebuilt when a tile's heights or weights, the
        // tile set or the grass layers change) and the varieties' resolved section materials.
        struct GrassRootCache
        {
            std::shared_ptr<Graphic::System::GrassFieldData> Field;
            uint64_t                                         Signature = 0;
        };
        struct GrassSections
        {
            const void*                                    Mesh         = nullptr;
            uint32_t                                       Version      = 0;
            uint64_t                                       TypeRevision = 0;
            std::vector<Graphic::System::GrassSectionDraw> Sections;
            // The look the variety leaves to the far cover, from its section materials (GRASS-ASSET p2): the
            // albedo map's cooked mean over the drawn texels (Assets::LoadGrassCoverLook) times the material's
            // tints, its subsurface colour and roughness; nullopt when no section has an albedo map.
            struct CoverLook
            {
                glm::vec3 Albedo{ 0.0f }, Subsurface{ 0.0f };
                float     Roughness = 0.0f;
            };
            std::optional<CoverLook> Cover;
        };
        std::unordered_map<uint64_t, GrassRootCache> m_Grass;
        std::unordered_map<uint64_t, GrassSections>  m_GrassSections;
        uint64_t                                     m_GrassRevision   = 0;
        double                                       m_WindSeconds     = 0.0;
        double                                       m_PrevWindSeconds = 0.0;
        std::unordered_set<std::string>              m_WarnedGrass;
        // GRASS-FIELD: per root, per grass layer (by LayerName), the far cover its grass types give the ground
        // (TerrainInstance::GrassCover*): EmitGrass derives it from the varieties it resolves, the tile rows of
        // the next frame carry it to the landscape's shading (Common/LandscapeGrassCover.glslh).
        struct GrassCoverValues
        {
            glm::vec4 Cover{ 0.0f }; // rgb = the blades' mean albedo, a = leaf area index at full weight
            glm::vec4 Light{ 0.0f }; // rgb = the blade material's SubsurfaceColor, a = its roughness
            glm::vec4 Range{ 0.0f }; // x/y = Start/EndCullDistance (cm), z = the lowest weight that grows grass
        };
        std::unordered_map<uint64_t, std::unordered_map<std::string, GrassCoverValues>> m_GrassCover;

        // Material handles already reported as unresolvable, so the warning is said once and not once a frame.
        std::unordered_set<uint64_t> m_WarnedMaterials;
    };
} // namespace Desert::ECS
