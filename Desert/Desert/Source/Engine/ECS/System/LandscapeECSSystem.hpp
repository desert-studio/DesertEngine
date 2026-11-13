#pragma once

#include <Engine/Assets/Serialization/LandscapeLayerInfo.hpp>
#include <Engine/Graphic/Systems/Scene/Grass/GrassRenderer.hpp>
#include <Engine/World/Landscape/LandscapeLayout.hpp>

#include "System.hpp"

#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Graphic/Materials/MaterialOverrides.hpp>

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
            // THE TILE'S LAYER ALLOCATION (UE: a component holds only the weightmap layers painted on it), the
            // complement: bit i when weightmap channel i (tile weight layer i, or none) is zero at every sample
            // of the tile. Taken with WeightRevision; the surface skips such a channel whole
            // (Graphic::System::LandscapeWeightDraw::EmptyLayers).
            uint32_t EmptyLayers = 0u;
        };

        // Creates, rewrites in place, or drops the tile's weightmap copy to match its weight layers.
        void UpdateWeightmap( TileGpu& gpu, const World::Landscape::LandscapeTileData& heights,
                              entt::entity entity, const LandscapeTileComponent& tileComp, bool weightsDirty );

        Graphic::ResourceOwnerInstance            m_SceneLedger;
        std::unordered_map<entt::entity, TileGpu> m_Tiles;
        uint64_t                                  m_Revision = 0u; // the last revision handed out
        // This frame's dirty flags, index i = DrawableLandscape::Tiles[i] (the shared tile table): resized
        // to the table each frame, so a steady frame reuses the storage.
        struct TileDirty
        {
            bool Heights = false;
            bool Weights = false;
            bool Grass   = false;
        };
        std::vector<TileDirty> m_TileDirty;
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
        // Each root's look, resolved once per frame and shared by all its tiles. The material is a `.demat`
        // like every other: its values (Tint) arrive as named overrides that TerrainRenderer applies on top
        // of the shader's schema defaults. A root without a LandscapeMaterial draws with those defaults.
        // Kept per root across frames (m_Surfaces) so the vectors and strings keep their capacity; Frame says
        // whether this frame filled it yet.
        struct Surface
        {
            uint64_t Frame = 0u;
            // Never null: the root's overrides of this material revision (m_RootOverrides), empty for no material.
            std::shared_ptr<const Graphic::MaterialOverrides> Overrides;
            // The root's weight layers (their `.delayerinfo` data, owned by the LandscapeLayerInfoService: valid
            // for this frame), which give each tile channel its colour and blend by name. Pending: a layer is
            // still loading, so a channel that names nothing yet is not reported as unknown.
            std::vector<const Assets::Serialization::LandscapeLayerInfoData*> Layers;
            // The layers' identity: each one's data address and asset Revision, in root order. What the grass
            // draw cache (GrassDrawCache) keys the layers' grass types by: a hot reload or Save moves it.
            uint64_t LayerSignature = 0u;
            bool     LayersPending  = false;
            // The root's FIRST layer's colour with a = 1 (UE shows its first layer where nothing is
            // painted); a = 0 while the root names none or that layer is not loaded: white ground.
            glm::vec4 Ground = glm::vec4( 0.0f );
            // That first layer's LayerName: the material input the ground draws with is matched by it.
            std::string GroundLayer;
        };
        std::unordered_map<uint64_t, Surface> m_Surfaces;
        uint64_t                              m_Frame = 0u;
        // Pass 3's per-frame grouping of the drawn tiles by root, kept for its capacity.
        struct GrassRootTiles
        {
            uint64_t                        Frame = 0u;
            World::Landscape::LandscapeRoot Root;
            std::vector<GrassTile>          Tiles;
        };
        std::unordered_map<uint64_t, GrassRootTiles> m_GrassTiles;
        std::vector<uint64_t>                        m_GrassRootOrder; // this frame's roots, ascending

        void EmitGrass( entt::registry& registry, Graphic::Render::RenderCommandBuffer& renderCommandBuffer,
                        uint64_t rootId, const World::Landscape::LandscapeRoot& root, const Surface& surface,
                        const std::vector<GrassTile>& tiles );
        // Builds the root's varieties and far cover anew into @p cache (EmitGrass, when its key moved).
        struct GrassDrawCache;
        void BuildGrassVarieties( uint64_t rootId, GrassDrawCache& cache );

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
            // tints, its subsurface colour and opacity (UE's Opacity pin, the subsurface density) and roughness;
            // nullopt when no section has an albedo map.
            struct CoverLook
            {
                glm::vec3 Albedo{ 0.0f }, Subsurface{ 0.0f };
                float     Roughness = 0.0f, Opacity = 0.0f;
                float     Coverage = 1.0f; // the share of the cards' world area their mask keeps (a blade's)
            };
            std::optional<CoverLook> Cover;
            // FPS-GRASS-HULL: a blade variety's cards' hulls, cooked with its look (Assets::GrassCardHull, the
            // union over its sections) and packed as GrassBladeDraw::CardHulls; empty = full cards.
            std::vector<glm::vec4> CardHulls;
            glm::vec2              HullTexels{ 0.0f };
        };
        // GRASS-EMIT (UE: ULandscapeGrassType / ALandscapeProxy::UpdateGrass - the types are resolved on load and
        // the grass data changes only when the landscape, a type or a material does): per root, the varieties as
        // the grass renderer takes them, built once per key and shared with the frame's command by pointer. A
        // frame whose key holds touches no variety: it only writes the wind (MakeInstanceWind of each variety's
        // response at this frame's air and clock) into one draw of the root's ring.
        // THE RING (UE: a render proxy's per-frame-in-flight uniform buffers, reused once the GPU frame retires):
        // the frame's command holds its draw until the frame is done, so a frame writes a slot no command holds -
        // starting at its frame number modulo the ring - and the ring holds kGrassDrawRing draws (the frames in
        // flight). A slot whose varieties predate the last build takes Built by copy assignment, which reuses its
        // storage; nothing is allocated in a frame whose key holds. More frames in flight than slots grows the
        // ring once (the deeper pipeline is then the steady state), never a copy per frame.
        static constexpr size_t kGrassDrawRing = 3u;
        struct GrassDrawSlot
        {
            std::shared_ptr<Graphic::System::GrassFieldDraw> Draw;
            uint64_t Generation = 0u; // GrassDrawCache::Generation it holds
        };
        struct GrassTypeSlot
        {
            uint32_t Layer    = 0u; // the grass layer's index in the field
            uint64_t Handle   = 0u; // the type's handle, from its GUID text (parsed once per key)
            uint64_t Revision = 0u; // the type asset's Revision when built; 0 = was not ready
        };
        struct GrassDrawCache
        {
            // The varieties as BuildGrassVarieties made them (wind unset); Generation counts its builds (0 =
            // none).
            Graphic::System::GrassFieldDraw Built;
            uint64_t                        Generation = 0u;
            std::vector<GrassDrawSlot> Ring; // kGrassDrawRing slots, grown only past that many frames in flight
            // Per variety of Built, the wind response (Strength, Speed, Height) MakeInstanceWind takes.
            std::vector<glm::vec3> WindResponse;
            // The key. Types: re-parsed when the layers or the content registry moved; Complete: every type,
            // mesh and material was ready or failed for good (a pending one is asked for again next frame; a
            // failed mesh or type is not, until the content registry or its service's revision moves).
            std::vector<GrassTypeSlot>                                        Types;
            uint64_t                                                          LayerSignature  = 0u;
            uint64_t                                                          ContentSerial   = 0u;
            uint64_t                                                          MeshRevision    = 0u;
            uint32_t                                                          MaterialVersion = 0u;
            bool                                                              TypesResolved   = false;
            bool                                                              Complete        = false;
        };
        std::unordered_map<uint64_t, GrassDrawCache> m_GrassDraws;
        // This frame's grass layers of the root EmitGrass works on (the Surface's layers that name a grass type).
        std::vector<const Assets::Serialization::LandscapeLayerInfoData*> m_GrassLayers;
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
            // GRASS-VIS-16: the blades' mean width per MinScreenWidth pixel (cm / px; 0 = no screen-width floor),
            // the view-independent half of the floor's distance D (Graphic::System::GrassFloorDistance).
            float FloorWidth = 0.0f;
            // GPU-COST-29: the share of the layer's leaf area that is blades, whose survivors widen; the rest is
            // meshes, which shrink out instead (GrassFieldShare.glslh GrassFieldCoverageOf).
            float WidenShare = 1.0f;
            // GRASS-SHADOW-1: the mesh varieties' bounding radius about the root at their mean scale (cm, weighted
            // by leaf area; 0 = no mesh variety), the view-independent half of their far LOD distance.
            float FarLodRadius = 0.0f;
        };
        std::unordered_map<uint64_t, std::unordered_map<std::string, GrassCoverValues>> m_GrassCover;

        // Material handles already reported as unresolvable, so the warning is said once and not once a frame.
        std::unordered_set<uint64_t> m_WarnedMaterials;

        // EACH ROOT'S MATERIAL OVERRIDES, RESOLVED ONCE PER REVISION (UE: a landscape proxy's material is rebuilt
        // on a material change, not read per frame), by root UUID. Every tile's draw command and the terrain
        // renderer's tile proxies share the one object by pointer, so a new pointer IS a new revision downstream.
        // Resolved again when the root names another handle, the material service moves its invalidation version
        // (a load landed or failed, a live edit, a shader change, Clear) or the content registry wrote a row (a
        // material saved after the scene first asked for it: ResolveOverrides discovers it only then).
        struct ResolvedOverrides
        {
            uint64_t                                          Material            = 0u; // the handle, 0 = none
            const void*                                       Service             = nullptr;
            uint32_t                                          InvalidationVersion = 0u;
            uint64_t                                          ContentSerial       = 0u;
            uint32_t                                          ValuesEditSerial    = 0u; // in-place value edits
            std::shared_ptr<const Graphic::MaterialOverrides> Overrides;            // never null once resolved
        };
        std::unordered_map<uint64_t, ResolvedOverrides> m_RootOverrides;
    };
} // namespace Desert::ECS
