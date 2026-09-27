#pragma once

#include <Engine/Geometry/EditMeshConversion.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <optional>
#include <cstdint>
#include <string>
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
    // Corner Mode moves a cell's corners along the normal of the face it works on - any of the three lattice
    // axes (UE's corner mode works on whatever face the selection lies on) - so a cell is not a plain solid flag
    // but a box with 8 corner offsets along one axis. Offsets are in 1/CornerDen of a BASE cell (60 divides the
    // 1/2, 1/4 and 1/10 snap sizes exactly); 0 everywhere is the ordinary axis-aligned block.
    // Corner index bits: 1 = +X, 2 = +Y (top), 4 = +Z; the bit of axis a is 1 << a.
    constexpr int CornerDen = 60;

    struct Cell
    {
        int16_t V[8] = {};
        // Material ID per face, in kFace order: an index into the blockout's material set (UE CubeGrid's
        // OpMeshMaterialID). Per FACE, not per cell, because Shift+B repaints only the faces under the
        // selection and a push-in gives the walls it exposes the active material, not the whole cell.
        uint8_t Mat[6] = {};
        // The lattice axis (0 = X, 1 = Y, 2 = Z) the offsets in V move along. One axis per cell: a slope is
        // built on one face at a time, and a cell already sloped along another axis refuses a second one
        // (Volume::ApplyCornerHeights). Meaningless while the cell is flat.
        uint8_t Axis = 1;
        // UE CubeGrid's Crosswise Diagonal, frozen into the cell by the Corner Mode pass that shaped it: the
        // bake splits its non-planar quads along the other diagonal (SplitsAlong13).
        bool Crosswise = false;

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

    // The grid frame: cell (0,0,0) sits at Origin and the lattice axes are Rotation's. Every volume operation
    // runs in FRAME coordinates (cell index * Unit); only meshing, drawing and targeting cross into the world,
    // so a rotated grid builds exactly the blocks an unrotated one does, just turned.
    // Ported from UE 5.8 ModelingComponents/Public/Mechanics/CubeGrid.h:158-186 (ToWorldPoint, ToGridPoint,
    // GridFrame), adapted: glm, float, the cell size is applied by the caller, not stored in the frame.
    struct GridFrame
    {
        glm::vec3 Origin{ 0.0f };
        glm::quat Rotation{ 1.0f, 0.0f, 0.0f, 0.0f };

        glm::vec3 ToWorldPoint( const glm::vec3& p ) const
        {
            return Origin + Rotation * p;
        }
        glm::vec3 ToWorldVector( const glm::vec3& v ) const
        {
            return Rotation * v;
        }
        glm::vec3 ToFramePoint( const glm::vec3& w ) const
        {
            return glm::conjugate( Rotation ) * ( w - Origin );
        }
        glm::vec3 ToFrameVector( const glm::vec3& w ) const
        {
            return glm::conjugate( Rotation ) * w;
        }
        // Same lattice axes (q and -q are one rotation); origins may differ.
        bool SameAxes( const GridFrame& o ) const;
        // Same axes AND same origin: the two lattices coincide cell for cell.
        bool SameAs( const GridFrame& o ) const;

        // `child` (a frame expressed in this one) expressed in this frame's parent: the frame of a piece
        // carried by an entity, in the world.
        GridFrame Compose( const GridFrame& child ) const
        {
            return { ToWorldPoint( child.Origin ), glm::normalize( Rotation * child.Rotation ) };
        }
        // The frame that undoes this one: Inverse().Compose( Compose( f ) ) == f.
        GridFrame Inverse() const
        {
            const glm::quat r = glm::conjugate( Rotation );
            return { r * -Origin, r };
        }
    };
    // The panel's frame: origin in centimetres, orientation as Euler degrees about X, Y, Z (glm's XYZ order,
    // the one the engine's transform rotation uses), UE's GridFrameOrigin + GridFrameOrientation.
    GridFrame MakeGridFrame( const glm::vec3& origin, const glm::vec3& eulerDegrees );

    // Ctrl+MMB: the world position of the corner of face `normal` (a unit axis step) of `cell` (edge `unit`,
    // in `frame`) nearest the ray (origin, dir) - the grid pivot goes there.
    // Ported from UE 5.8 MeshModelingToolsExp/Private/CubeGridTool.cpp:1634-1680 (OnCtrlMiddleClick), adapted:
    // the face comes as a cell and an outward axis instead of a min/max box, and the corner is returned in the
    // world rather than applied to a gizmo.
    glm::vec3 NearestFaceCorner( const GridFrame& frame, const glm::ivec3& cell, const glm::ivec3& normal,
                                 float unit, const glm::vec3& rayOrigin, const glm::vec3& rayDir );

    // FRAME-space position of corner `i` of the cell at `c` (edge `unit`), with its offset along the cell's Axis.
    glm::vec3 CornerPos( const glm::ivec3& c, const Cell& cell, int i, float unit );

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

    // The four posts of the selection rectangle, as (at uMax, at vMax). Index order is (uMin,vMin) (uMax,vMin)
    // (uMin,vMax) (uMax,vMax) - the order of Corner Mode's height array.
    struct RectPost
    {
        bool AtUMax, AtVMax;
    };
    extern const RectPost kPosts[4];
    // The corner index, in the cell just inside `plane` at that post, that post `k` is: the corner on the
    // plane's side of the cell, at the post's u and v ends.
    int PostCorner( const WorkPlane& plane, int k );

    // Which diagonal the bake splits face `f` of a deformed `cell` along: true = corners 1-3 of kFaceCorner[f],
    // false = 0-2. The diagonal runs through the odd corner out, so a single raised or lowered corner makes a
    // pyramid on a square base and a checkerboard a ridge; Cell::Crosswise flips it.
    // Ported from UE 5.8 ModelingOperators/Private/CompositionOps/CubeGridBooleanOp.cpp:255-266, adapted: a
    // corner is "welded at base" when its offset is 0, and the rule is applied per face of a cell.
    bool SplitsAlong13( const Cell& cell, int f );

    // Corner Mode heights of the four rectangle posts, in 1/CornerDen base-cell units, OUT of the work-plane.
    using CornerHeights = std::array<int, 4>;

    // A committed piece. Its cells and its Block Size are frozen for good: starting a new marquee commits what
    // was pushed out, so a later Resize Grid only re-scales the volume being worked on NOW.
    struct Layer
    {
        CellMap   Cells;
        float     Unit = 1.0f;
        GridFrame Frame; // grid frame this piece was built in
    };

    // Re-express a marquee (stored in BASE cells) in a new base, keeping its world position, then re-snap it
    // out to whole blocks of K cells. The plane cell and the marquee's press anchor move with it.
    void RescaleSelection( WorkPlane& plane, glm::ivec2& anchor, Rect& sel, float oldUnit, float newUnit, int K );

    class Volume
    {
    public:
        CellMap            m_Cells;        // the active volume, BASE-resolution (frame = index * Unit)
        float              m_Unit = -1.0f; // base cell size; negative = no base chosen yet
        GridFrame          m_Frame;        // grid frame the active volume is built in
        std::vector<Layer> m_Frozen;       // committed pieces, each at its own Unit and Frame

        bool IsEmpty() const
        {
            return m_Cells.empty() && m_Frozen.empty();
        }

        // Occupancy across every layer, for a query cell of edge `unit` in the grid frame `frame`.
        bool SolidAt( const glm::ivec3& cell, float unit, const GridFrame& frame ) const;
        // Is face `f` of `cell` hidden by its neighbour? A deformed cell only hides a face when the four shared
        // corners agree; otherwise the two boxes don't actually meet there.
        bool FaceHidden( const CellMap& cells, const glm::ivec3& cell, const Cell& data, int f, float unit,
                         const GridFrame& frame ) const;

        // Subdivide the active base by F: every cell splits into F^3 (geometry stays put), Unit /= F.
        void Refine( int F );
        // Commit the active volume into an immutable layer; afterwards the next Block Size starts a new base.
        // Returns false (and changes nothing) when there is nothing to commit.
        bool Freeze();
        // dir > 0: per column of `sel`, fill `height` base cells from the first empty cell out of the plane;
        // every face of a new cell takes `material`. dir <= 0: remove `height` base cells just inside the
        // plane; the faces of the remaining active cells that the removal exposes take `material` (UE gives
        // every triangle an op creates the op's material, the walls of a hole included). The plane moves with
        // the edit.
        void PushPull( WorkPlane& plane, const Rect& sel, int dir, int height, uint8_t material );
        // Shift+B: every exposed face, in ANY layer whose frame shares the active axes (a turned piece's faces
        // never lie on this lattice's planes), that lies on the work-plane and faces out of it, with its
        // centre inside `sel`, takes `material`. Geometry is untouched. Returns the faces painted; 0
        // when no active base is chosen yet or nothing under the selection faces that way.
        int PaintFaces( const WorkPlane& plane, const Rect& sel, uint8_t material );
        // Corner Mode: every outer corner of the cell layer just inside the work-plane, under the selection,
        // takes the bilinear blend of the four posts along the plane's normal (a wall slopes as a floor does),
        // so raising two posts yields one clean ramp and raising one a hip; the cells take `crosswise`.
        // A committed piece on this lattice under the selection is thawed first (ThawUnder), as push-in does:
        // UE's corner push reaches the mesh it targets, so a slab built earlier slopes as the active one does.
        // Refused, and no corner changes, when a cell there is already sloped along another axis.
        Common::BoolResultStr ApplyCornerHeights( const WorkPlane& plane, const Rect& sel,
                                                  const CornerHeights& heights, bool crosswise );
        // Read the rectangle's post heights back out of the cells (zero where no cell, or a cell sloped along
        // another axis), so a second Corner Mode pass continues from the current shape. Thaws that row first,
        // as ApplyCornerHeights does, so a committed slope reads back too.
        CornerHeights ReadCornerHeights( const WorkPlane& plane, const Rect& sel );

        // One quad soup out of every layer, each meshed at its OWN cell size and face-culled against all
        // layers: flat cells greedy-merged (never across two materials), deformed cells one slanted quad per
        // visible face, frame-aligned UVs (the texture turns with the grid), then carried into the world by the
        // layer's own frame. One Submesh per material ID in use, ascending, with SubmeshMaterialIds naming it -
        // the layout FromRenderMesh reads into per-triangle MaterialIDs.
        RenderMeshData Bake() const;

        // Push-in reaches committed pieces too (UE's Cube Grid cuts whatever mesh it targets, so nothing it
        // built earlier is dead): a frozen layer on this lattice that holds a cell of `cells` (base cells of
        // the active volume) is merged into the active volume first - as it is at this Block Size, split into
        // base cells when coarser and flat - and a finer one loses its cells inside the cut. A coarser DEFORMED
        // layer stays frozen (splitting would have to re-derive its corners), as does one in another frame.
        void ThawUnder( const std::vector<glm::ivec3>& cells );

        // Renumber face materials so the i-th ID of `usedAscending` (the bake's SubmeshMaterialIds) becomes
        // i, in every layer: the saved blockout's material ID is then its entity's material slot.
        void CompactMaterials( const std::vector<int>& usedAscending );

    private:
        // The cells just inside `plane` under `sel`: the row Corner Mode shapes.
        static std::vector<glm::ivec3> InsideRow( const WorkPlane& plane, const Rect& sel );
    };

    // Every layer of `v` re-expressed through `parent` (parent.Compose on each frame); cells, units and
    // materials are untouched. The tool edits in the world and the entity keeps its voxels in its own space.
    Volume Reframed( const Volume& v, const GridFrame& parent );

    // --- A blockout's voxels as the scene keeps them beside its mesh (ECS::CubeGridBlockoutComponent), so the
    //     tool can be opened again on the entity and carry on (UE: the Cube Grid tool takes the selected mesh
    //     as its target). Plain reflectable data: the scene file writes it as it is. ---
    struct SavedLayer
    {
        float                Unit = 0.0f;
        std::array<float, 3> Origin{};
        std::array<float, 4> Rotation{ 1.0f, 0.0f, 0.0f, 0.0f }; // w, x, y, z
        // Per flat cell: x, y, z, then the 6 face materials in kFace order.
        std::vector<int32_t> Flat;
        // Per deformed cell: x, y, z, the 6 face materials, the 8 corner offsets (Cell::V), the offset axis
        // (Cell::Axis) and Crosswise (0 / 1).
        std::vector<int32_t> Deformed;
    };
    constexpr size_t kSavedFlatStride     = 9;
    constexpr size_t kSavedDeformedStride = 19;

    struct SavedBlockout
    {
        // Every non-empty layer, the active one last. They come back all committed: push-in thaws what it
        // reaches (Volume::ThawUnder), so a committed layer is as editable as the active one was.
        std::vector<SavedLayer> Layers;
        // MeshKey of the mesh these voxels baked to, in hex (a JSON integer is signed 64-bit). A mesh edited
        // since by another tool no longer matches, and the voxels would overwrite that edit: refused, not
        // re-baked (the tool's reopen).
        std::string MeshKey;
    };

    SavedBlockout Save( const Volume& v, uint64_t meshKey );
    // Refused, by layer, cell and value, when a unit is not positive, an array is not whole cells, a cell is
    // outside the packable range or repeats, a material, offset, axis or diagonal does not fit its field, or a
    // rotation is not a rotation.
    Common::ResultStr<Volume> Load( const SavedBlockout& saved );
    // Parse Save's MeshKey spelling back; nullopt when it is not 16 hex digits.
    std::optional<uint64_t> ParseMeshKey( const std::string& text );

    // Identity of a mesh's shape for the staleness check: its triangle count and its DISTINCT vertex positions
    // (to 1/100 cm, order-free), so the same surface keys the same whether it is the EditMesh the bake welded
    // or the .stmesh the Static Mesh output wrote and a tool lifts back (seams split vertices, never move them).
    uint64_t MeshKey( std::vector<glm::vec3> positions, int triangleCount );

    // Shift+E / Shift+Q: move the selection `baseCells` along the work-plane's outward normal (negative =
    // back into the surface) without editing anything, so the next Push/Pull starts from there.
    // Ported from UE 5.8 MeshModelingToolsExp/Private/CubeGridTool.cpp:878-894 (SlideSelection), adapted: the
    // selection is a base-cell plane index, not a frame-space box, so the displacement is an index step.
    void SlideSelection( WorkPlane& plane, int baseCells );

    // Ctrl+drag push/pull. The parameter along the line (origin, unit dir) of the point closest to the ray
    // (origin, dir; t >= 0): the drag is measured by projecting the cursor ray onto the selection's normal.
    // Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Distance/DistLine3Ray3.h:52-98, adapted:
    // glm, float, only the line parameter is returned (the ray direction is normalised here).
    float LineParameterClosestToRay( const glm::vec3& lineOrigin, const glm::vec3& lineDir,
                                     const glm::vec3& rayOrigin, const glm::vec3& rayDir );
    // Blocks a drag of `paramDelta` along the normal means: rounded to whole steps of Blocks Per Step.
    // Ported from UE 5.8 MeshModelingToolsExp/Private/CubeGridTool.cpp:2033-2035 (OnClickDrag), adapted: int
    // result, no preview op.
    int DragExtrudeBlocks( float paramDelta, float blockSize, int blocksPerStep );
} // namespace Desert::Geometry::VoxelBlockout
