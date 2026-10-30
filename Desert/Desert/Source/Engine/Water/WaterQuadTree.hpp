#pragma once

// THE WATER SURFACE'S QUADTREE (WATER-W6): which square tiles of a water zone are drawn this frame, how big, with
// which grid density and at which LOD, so the surface is fine under the camera and coarse at the horizon and a
// LOD change never opens a crack (the vertex stage morphs every tile toward the next LOD by distance).
//
// Ported from UE 5.8 Engine/Plugins/Experimental/Water:
//   - Source/Runtime/Private/WaterQuadTree.cpp:141-298 (FNode::SelectLOD / SelectLODRefinement: the LOD a node is
//     drawn at from its 2D distance to the observer, LOD L reaching GetLODDistance(L) = 2^(L+1) * LODScale,
//     Public/WaterQuadTree.h:213), :11-100 (AddNodeForRender: the instance, its density bucket and morph flags),
//     :128-135 (CanRender: a node draws whole when its subtree is complete and one body's);
//   - Source/Runtime/Private/WaterMeshSceneProxy.cpp:175-222 (LODScale = leaf size * max(LODScale, 0.5);
//     2^TessellationFactor quads at density 0, halved per density; DensityCount = min(depth, log2(quads))),
//     :1339-1356 (GetWaterLODParams: the lowest LOD and its global morph from the height above the water).
// Adapted: Y-up — UE's (X, Y) plane is our (X, Z) and its Z our Y. The tree is implicit: UE stores the nodes it
// rasterised the water bodies into and generates the children of a complete subtree on the fly (:260-283); here
// every node is generated on the fly and classified against the bodies' footprints in leaf-tile units, which is
// the same tree. Not ported yet: the ocean far mesh past the zone (:680-723), occlusion culling, the GPU-driven
// tree (UE r.Water.WaterMesh.GPUQuadTree).

#include <Engine/Core/Frustum.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace Desert::Water
{
    /// One water body as the quadtree sees it: its footprint on the (X, Z) plane and its still level.
    struct WaterQuadTreeBody
    {
        glm::vec2 FootprintMin{ 0.0f };
        glm::vec2 FootprintMax{ 0.0f };
        float     SurfaceY = 0.0f;
    };

    /// The zone the tree covers (ECS::WaterZoneData, placed at the zone entity).
    struct WaterQuadTreeDesc
    {
        glm::vec2 ZoneCenter{ 0.0f };
        glm::vec2 ZoneExtent{ 51200.0f, 51200.0f };
        float     TileSize           = 2400.0f;
        int32_t   TessellationFactor = 6;
        float     LODScale           = 1.0f;
    };

    /// One drawn tile (UE FStagingInstanceData): a square grid of QuadsPerSide(DensityIndex) quads per side.
    struct WaterTileInstance
    {
        // The tile's lower (X, Z) corner RELATIVE TO the tree's root corner (WaterQuadTree::GetRootMin): a whole
        // number of leaf tiles, exact in float, so two neighbours place their shared edge at the same bits.
        glm::vec2 Corner{ 0.0f };
        float     Size          = 0.0f;
        float     SurfaceY      = 0.0f;
        int32_t   LODLevel      = 0;
        int32_t   DensityIndex  = 0;
        bool      ShouldMorph   = false;
        bool      CanMorphTwice = false;
        /// The lowest LOD's global morph toward the next one, from the camera's height (0 on every other LOD).
        float HeightMorph = 0.0f;
    };

    struct WaterLODParams
    {
        int32_t LowestLOD         = 0;
        float   HeightLODFactor   = 0.0f;
        float   WaterHeightForLOD = 0.0f;
    };

    class WaterQuadTree
    {
    public:
        /// An empty tree (no bodies inside the zone) draws nothing.
        WaterQuadTree( const WaterQuadTreeDesc& desc, std::span<const WaterQuadTreeBody> bodies );

        [[nodiscard]] static float GetLODDistance( int32_t lodLevel, float lodScale );

        [[nodiscard]] bool IsEmpty() const
        {
            return m_Bodies.empty();
        }
        /// The world (X, Z) of the root's lower corner, the origin every WaterTileInstance::Corner is relative to.
        [[nodiscard]] glm::vec2 GetRootMin() const
        {
            return m_RootMin;
        }
        [[nodiscard]] int32_t GetTreeDepth() const
        {
            return m_TreeDepth;
        }
        [[nodiscard]] int32_t GetDensityCount() const
        {
            return m_DensityCount;
        }
        /// World distance unit of the LOD rings: leaf size * max(authored LODScale, 0.5).
        [[nodiscard]] float GetLODScale() const
        {
            return m_LODScale;
        }
        /// The ring unit one view measures with: GetLODScale() stretched by the view's lens
        /// (Graphic::DrawDistanceScale), so a k-times zoom keeps every ring - and the vertex morph that reads the
        /// same unit - k times farther out. The tree itself stays lens-agnostic: it takes this one scalar.
        [[nodiscard]] float    GetViewLODScale( float viewDistanceScale ) const;
        [[nodiscard]] uint32_t QuadsPerSide( int32_t densityIndex ) const
        {
            return static_cast<uint32_t>( m_NumQuadsLOD0 ) >> static_cast<uint32_t>( densityIndex );
        }

        /// @p viewLODScale: GetViewLODScale of the view the camera height is measured for.
        [[nodiscard]] WaterLODParams GetWaterLODParams( const glm::vec3& observer, float viewLODScale ) const;

        /// The tiles drawn for @p observer, culled by @p frustum (null = none), grouped by density index (all of
        /// density 0 first). @p verticalExtent pads the culling box above and below the still level (the waves).
        /// @p viewDistanceScale: the view's lens scale (Graphic::DrawDistanceScale; 1 = the authored 90-degree
        /// lens) - every LOD ring and the camera-height LOD stretch by it.
        [[nodiscard]] std::vector<WaterTileInstance> Traverse( const glm::vec3& observer, float viewDistanceScale,
                                                               const Core::Frustum* frustum,
                                                               float                verticalExtent ) const;

    private:
        // A body's footprint clipped to the zone, in leaf tiles from the root's corner: [Min, Max).
        struct TileRange
        {
            glm::ivec2 Min{ 0 };
            glm::ivec2 Max{ 0 };
            float      SurfaceY = 0.0f;
        };

        // A node: its level (leaf = 0) and first leaf tile; it spans 2^Level tiles per side.
        struct Node
        {
            int32_t    Level = 0;
            glm::ivec2 Tile{ 0 };
        };

        enum class Coverage : uint8_t
        {
            Empty,
            Complete, // one body covers every leaf of the node (UE HasCompleteSubtree && IsSubtreeSameWaterBody)
            Partial,
        };

        struct Traversal
        {
            glm::vec2                       Observer{ 0.0f };
            const Core::Frustum*            Frustum        = nullptr;
            float                           VerticalExtent = 0.0f;
            float                           LODScale       = 0.0f; // GetViewLODScale of this view
            WaterLODParams                  LOD;
            std::vector<WaterTileInstance>* Output = nullptr;
        };

        [[nodiscard]] Coverage Classify( const Node& node, float& surfaceY ) const;
        [[nodiscard]] bool     Visible( const Node& node, const Traversal& t ) const;
        [[nodiscard]] float    DistanceToNode( const Node& node, const glm::vec2& point ) const;
        [[nodiscard]] Node     Child( const Node& node, int32_t index ) const;

        void SelectLOD( const Node& node, int32_t lodLevel, const Traversal& t ) const;
        void SelectLODRefinement( const Node& node, int32_t densityLevel, int32_t lodLevel,
                                  const Traversal& t ) const;
        void AddNodeForRender( const Node& node, float surfaceY, int32_t densityLevel, int32_t lodLevel,
                               const Traversal& t ) const;

        glm::vec2              m_RootMin{ 0.0f };
        float                  m_LeafSize     = 0.0f;
        float                  m_LODScale     = 0.0f;
        int32_t                m_NumQuadsLOD0 = 0;
        int32_t                m_TreeDepth    = 0;
        int32_t                m_DensityCount = 0;
        float                  m_MinSurfaceY  = 0.0f;
        float                  m_MaxSurfaceY  = 0.0f;
        std::vector<TileRange> m_Bodies;
    };
} // namespace Desert::Water
