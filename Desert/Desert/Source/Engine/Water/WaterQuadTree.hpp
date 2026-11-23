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
#include <Engine/Water/WaterRiver.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
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
        /// Where two footprints overlap, the higher priority owns the tiles (Water::WaterBodyPriority: a river
        /// over an ocean); equal priorities keep the first body that covers a node whole.
        int32_t Priority = 0;
        /// A river's baked flow (null for an ocean): the body owns only the leaf tiles its ribbon (bank mask)
        /// touches, not its whole footprint box (UE draws a river along its spline, not over its bounds).
        std::shared_ptr<const RiverFlowField> Ribbon;
    };

    /// The zone the tree covers (ECS::WaterZoneData, placed at the zone entity).
    struct WaterQuadTreeDesc
    {
        glm::vec2 ZoneCenter{ 0.0f };
        glm::vec2 ZoneExtent{ 51200.0f, 51200.0f };
        float     TileSize           = 2400.0f;
        int32_t   TessellationFactor = 6;
        float     LODScale           = 1.0f;

        bool operator==( const WaterQuadTreeDesc& ) const = default;
    };

    /// One drawn tile (UE FStagingInstanceData): a square grid of QuadsPerSide(DensityIndex) quads per side.
    struct WaterTileInstance
    {
        // The tile's lower (X, Z) corner RELATIVE TO the tree's root corner (WaterQuadTree::GetRootMin): a whole
        // number of leaf tiles, exact in float, so two neighbours place their shared edge at the same bits.
        glm::vec2 Corner{ 0.0f };
        float     Size          = 0.0f;
        float     SurfaceY      = 0.0f;
        /// The body that owns the tile: its index in the bodies the tree was built from (the renderer's per-tile
        /// material, waves and flow row).
        uint32_t  BodyIndex     = 0;
        int32_t   LODLevel      = 0;
        int32_t   DensityIndex  = 0;
        bool      ShouldMorph   = false;
        bool      CanMorphTwice = false;
        /// An ocean drawn UNDER a river in a leaf the river's banks only partly cover (UE: the ocean under the
        /// river, the river-to-ocean transition): this tile is clipped to the complement of the river's bank mask
        /// (WaterMeshTiles.glslh WaterBankClipped), the river's own tile in the same leaf to the mask, so the two
        /// partition the leaf by the one mask the CPU query answers by (Water::QueryWater).
        bool UnderRiver = false;
        /// The lowest LOD's global morph toward the next one, from the camera's height (0 on every other LOD).
        float HeightMorph = 0.0f;
    };

    /// What one traversal writes into, kept by the caller across frames (UE: the per-view water mesh buffers the
    /// FWaterQuadTree traversal fills): once their capacity covers a frame's tiles a traversal allocates nothing.
    struct WaterTraversalBuffers
    {
        std::vector<WaterTileInstance> Tiles;  // the result, density 0 first
        std::vector<WaterTileInstance> Staged; // the walk's order, before the bucket pass
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
        /// The tiles land in @p buffers.Tiles (the returned span views it until the next traversal into them).
        [[nodiscard]] std::span<const WaterTileInstance>
        Traverse( const glm::vec3& observer, float viewDistanceScale, const Core::Frustum* frustum,
                  float verticalExtent, WaterTraversalBuffers& buffers ) const;

    private:
        // The densest tessellation (2^12 quads per side at density 0), so at most this many density buckets.
        static constexpr int32_t kMaxDensityCount = 12;

        // A body's footprint clipped to the zone, in leaf tiles from the root's corner: [Min, Max).
        struct TileRange
        {
            glm::ivec2 Min{ 0 };
            glm::ivec2 Max{ 0 };
            float      SurfaceY  = 0.0f;
            uint32_t   BodyIndex = 0;
            int32_t    Priority  = 0;
            // A ribbon's leaves (WaterQuadTreeBody::Ribbon): the summed-area table of the leaves it touches over
            // [Min, Max], (Max - Min + 1) per side, so a node's owned-leaf count is four reads. Empty = all.
            std::vector<int32_t> LeafSums;
            // The same table of the leaves the ribbon covers WHOLE: no bilinear sample of the bank mask inside the
            // leaf falls below kRiverBankClip, so its surface there is never clipped. Empty = all.
            std::vector<int32_t> FullSums;
            [[nodiscard]] bool   IsRibbon() const noexcept
            {
                return !LeafSums.empty();
            }
            [[nodiscard]] int32_t CountLeaves( glm::ivec2 lo, glm::ivec2 hi ) const noexcept;
            [[nodiscard]] int32_t        CountFullLeaves( glm::ivec2 lo, glm::ivec2 hi ) const noexcept;
            [[nodiscard]] static int32_t SumOver( const std::vector<int32_t>& sums, glm::ivec2 rangeMin,
                                                  glm::ivec2 rangeMax, glm::ivec2 lo, glm::ivec2 hi ) noexcept;
        };

        // The body a node's tiles are drawn as.
        struct TileOwner
        {
            float    SurfaceY  = 0.0f;
            uint32_t BodyIndex = 0;
            // A leaf a ribbon owns but does not cover whole, over a non-ribbon body (an ocean): that body is drawn
            // too, under the ribbon, clipped to the bank mask's complement (WaterTileInstance::UnderRiver).
            bool     HasUnder       = false;
            float    UnderSurfaceY  = 0.0f;
            uint32_t UnderBodyIndex = 0;
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

        [[nodiscard]] Coverage Classify( const Node& node, TileOwner& owner ) const;
        // Fills @p range.LeafSums from the ribbon's bank mask; false when no in-bank texel lies inside the range.
        [[nodiscard]] bool BuildRibbonLeaves( const RiverFlowField& flow, TileRange& range ) const noexcept;
        [[nodiscard]] Common::Math::AABB NodeBox( const Node& node, const Traversal& t ) const;
        [[nodiscard]] bool               Visible( const Node& node, const Traversal& t ) const;
        // Bit i: Visible( Child( node, i ) ), the four tested at once (Frustum::Intersects4).
        [[nodiscard]] uint32_t           VisibleChildren( const Node& node, const Traversal& t ) const;
        [[nodiscard]] float    DistanceToNode( const Node& node, const glm::vec2& point ) const;
        [[nodiscard]] Node     Child( const Node& node, int32_t index ) const;

        void SelectLOD( const Node& node, int32_t lodLevel, const Traversal& t ) const;
        void SelectLODRefinement( const Node& node, int32_t densityLevel, int32_t lodLevel,
                                  const Traversal& t ) const;
        void AddNodeForRender( const Node& node, const TileOwner& owner, int32_t densityLevel, int32_t lodLevel,
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
