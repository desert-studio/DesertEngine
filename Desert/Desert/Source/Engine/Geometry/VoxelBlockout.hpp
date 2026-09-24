#pragma once

#include <Engine/Geometry/EditMeshConversion.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

// The editor-free core of the CubeGrid blockout tool (UE5 Modeling Mode "CubeGrid"): a sparse voxel volume
// at a fixed BASE resolution, the Push / Pull and Corner Mode edits on it, the committed layers, and the bake
// into quads. The tool (picking, marquee, gizmo, panel, the entity it writes) lives in the editor and drives
// this; nothing here knows about ImGui, the scene or the renderer, so the geometry is unit-testable.
//
// KEY ARCHITECTURE (matches UE): solids are stored at a FIXED BASE resolution and NEVER move when the Block
// Size (grid step) changes; changing Block Size only re-scales the brush. A coarser Block Size stamps K*K*K
// base cells at once; a Block Size finer than the base subdivides the base losslessly (Volume::Refine).
namespace Desert::Geometry::VoxelBlockout
{
    // Corner Mode moves a cell's corners along the grid's up axis, so a cell is not a plain solid flag but a
    // box with 8 vertical corner offsets. Offsets are in 1/CornerDen of a BASE cell (60 divides the 1/2, 1/4
    // and 1/10 snap sizes exactly); 0 everywhere is the ordinary axis-aligned block.
    // Corner index bits: 1 = +X, 2 = +Y (top), 4 = +Z.
    constexpr int CornerDen = 60;

    struct Cell
    {
        int16_t V[8] = {};

        bool IsFlat() const
        {
            for ( int16_t o : V )
                if ( o != 0 )
                    return false;
            return true;
        }
    };
    using CellMap = std::unordered_map<uint64_t, Cell>;

    // Sparse volume key: a grid cell packed into 63 bits (21 per axis, centred so negatives fit).
    uint64_t   Pack( const glm::ivec3& c );
    glm::ivec3 Unpack( uint64_t k );
    // Floor division rounding toward -infinity (block-aligns negative coordinates correctly).
    int FloorDiv( int a, int b );

    // The 6 faces of a unit cube (position in [-0.5,0.5] with the engine's cube normals/tangents/UVs), each as
    // 4 CCW corners, copied from PrimitiveMeshFactory::CreateCube so winding and normals are known-good.
    // Face order: +Z, -Z, +Y, -Y, -X, +X.
    struct FaceVert
    {
        glm::vec3 P, N, T, B;
        glm::vec2 UV;
    };
    extern const FaceVert kFace[6][4];
    // Outward neighbour offset per face; a face is generated only on a Solid/Empty border.
    extern const glm::ivec3 kNeighbor[6];
    // The same faces as CORNER indices in kFace's winding. Corner Mode moves corners, so every deformed quad is
    // built from these instead of from a fixed box.
    extern const int kFaceCorner[6][4];
    // The corner-index bit that flips when you step to that face's neighbour.
    extern const int kFaceAxisBit[6];

    // World position of corner `i` of the cell at `c` (edge `unit`, frame `origin`), with its vertical offset.
    glm::vec3 CornerPos( const glm::ivec3& c, const Cell& cell, int i, float unit, const glm::vec3& origin );

    // The work-plane, in BASE cells of the active volume: axis Na (0=X, 1=Y, 2=Z) facing Sign, sitting at the
    // near side of cell index `Cell` along Na. The in-plane axes are u = (Na+1)%3 and v = (Na+2)%3.
    struct WorkPlane
    {
        int Na   = 1;
        int Sign = 1;
        int Cell = 0;
    };

    // An inclusive BASE-cell rectangle on the work-plane.
    struct Rect
    {
        int UMin = 0;
        int UMax = 0;
        int VMin = 0;
        int VMax = 0;
    };

    // The four posts of the selection rectangle, as (at uMax, at vMax, corner index of that post's cell).
    // For a ground grid (Na=1) u is Z (bit 4) and v is X (bit 1). Index order is (uMin,vMin) (uMax,vMin)
    // (uMin,vMax) (uMax,vMax) - the order of Corner Mode's height array.
    struct RectPost
    {
        bool AtUMax, AtVMax;
        int  Corner;
    };
    extern const RectPost kPosts[4];

    // Corner Mode heights of the four rectangle posts, in 1/CornerDen base-cell units.
    using CornerHeights = std::array<int, 4>;

    // A committed piece. Its cells and its Block Size are frozen for good: starting a new marquee commits what
    // was pushed out, so a later Resize Grid only re-scales the volume being worked on NOW.
    struct Layer
    {
        CellMap   Cells;
        float     Unit = 1.0f;
        glm::vec3 Origin{ 0.0f }; // grid frame this piece was built in
    };

    // Re-express a marquee (stored in BASE cells) in a new base, keeping its world position, then re-snap it
    // out to whole blocks of K cells. The plane cell and the marquee's press anchor move with it.
    void RescaleSelection( WorkPlane& plane, glm::ivec2& anchor, Rect& sel, float oldUnit, float newUnit, int K );

    class Volume
    {
    public:
        CellMap            Cells;          // the active volume, BASE-resolution (world = index * Unit + Origin)
        float              Unit = -1.0f;   // base cell size; negative = no base chosen yet
        glm::vec3          Origin{ 0.0f }; // grid frame the active volume is built in
        std::vector<Layer> Frozen;         // committed pieces, each at its own Unit and Origin

        bool IsEmpty() const
        {
            return Cells.empty() && Frozen.empty();
        }

        // Occupancy across every layer, for a query cell of edge `unit` in the frame `origin`.
        bool SolidAt( const glm::ivec3& cell, float unit, const glm::vec3& origin ) const;
        // Is face `f` of `cell` hidden by its neighbour? A deformed cell only hides a face when the four shared
        // corners agree; otherwise the two boxes don't actually meet there.
        bool FaceHidden( const CellMap& cells, const glm::ivec3& cell, const Cell& data, int f, float unit,
                         const glm::vec3& origin ) const;

        // Subdivide the active base by F: every cell splits into F^3 (geometry stays put), Unit /= F.
        void Refine( int F );
        // Commit the active volume into an immutable layer; afterwards the next Block Size starts a new base.
        // Returns false (and changes nothing) when there is nothing to commit.
        bool Freeze();
        // dir > 0: per column of `sel`, fill `height` base cells from the first empty cell out of the plane.
        // dir <= 0: remove `height` base cells just inside the plane. The plane moves with the edit.
        void PushPull( WorkPlane& plane, const Rect& sel, int dir, int height );
        // Corner Mode: every top corner of the cell layer under the selection takes the bilinear blend of the
        // four posts, so raising two posts yields one clean ramp and raising one a hip. Only a ground-facing
        // plane (Na=1, Sign>0) has a top layer; returns false and changes nothing otherwise.
        bool ApplyCornerHeights( const WorkPlane& plane, const Rect& sel, const CornerHeights& heights );
        // Read the rectangle's post heights back out of the cells (zero where no cell or no ground plane), so
        // a second Corner Mode pass continues from the current shape.
        CornerHeights ReadCornerHeights( const WorkPlane& plane, const Rect& sel ) const;

        // One quad soup out of every layer, each meshed at its OWN cell size and face-culled against all
        // layers: flat cells greedy-merged, deformed cells one slanted quad per visible face, world-aligned UVs.
        RenderMeshData Bake() const;
    };
} // namespace Desert::Geometry::VoxelBlockout
