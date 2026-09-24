#pragma once

#include "EditMesh.hpp"

#include <Common/Core/ResultStr.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Desert::Geometry
{
    // WHOLE-OBJECT TRANSFORM OPERATIONS - UE's XForm tab: Edit Pivot (UEditPivotTool), Bake Transform
    // (UBakeTransformTool), Merge (UCombineMeshesTool), Split (USplitMeshesTool) and Pattern (UPatternTool).
    //
    // Same contract as the other EditMesh operation headers: pure functions, the input is not touched, the
    // result is a NEW mesh; a refusal leaves nothing half-done and names what and where. What is new here is
    // that each of these also answers "where does the entity end up": a transform comes in and, for Edit
    // Pivot and Bake, a different one goes out, chosen so that every vertex stays where it is in the WORLD.
    // Units are centimetres; +Y is up (the engine's convention - CutPlane's default normal).

    // An entity's LOCAL transform in the engine's own spelling (ECS::TransformComponent): Matrix() is
    // translate(Translation) * mat4(quat(Rotation)) * scale(Scale), Rotation in radians (XYZ Euler as glm's
    // quat constructor reads it). Kept here, not in ECS, so the relation "world is unchanged" is testable
    // without a scene.
    struct Trs
    {
        glm::vec3 Translation{ 0.0f };
        glm::vec3 Rotation{ 0.0f };
        glm::vec3 Scale{ 1.0f };

        [[nodiscard]] glm::mat4 Matrix() const;
    };

    struct MeshBox
    {
        glm::vec3 Min{ 0.0f };
        glm::vec3 Max{ 0.0f };
    };

    // The axis-aligned box of the live vertices, in the mesh's space. Refused for a mesh with no vertex.
    [[nodiscard]] Common::ResultStr<MeshBox> ComputeMeshBox( const EditMesh& mesh );

    struct TransformedMesh
    {
        EditMesh Mesh;
        // The linear part's determinant was negative (a mirror): every triangle is wound the other way round,
        // so a surface that faced outwards still does.
        bool WindingReversed = false;
        // The linear part was not a rotation times a uniform scale, so a transformed tangent would no longer
        // lie in the transformed surface: the tangent layer was rebuilt (ComputeTangentsAt) instead.
        bool TangentsRebuilt = false;
    };

    // The mesh with every vertex mapped by `transform` (any invertible affine matrix):
    //   * positions: transform * p;
    //   * normals: the inverse transpose of the linear part, renormalised - the one map that keeps a normal
    //     perpendicular to its surface under a non-uniform scale;
    //   * tangents: the linear part, renormalised, the bitangent sign negated when the determinant is
    //     negative (a reflection turns a right-handed frame left-handed); rebuilt instead when the map is not
    //     conformal (see TransformedMesh::TangentsRebuilt);
    //   * a negative determinant reverses every triangle's corner order (and its overlay elements with it);
    //   * UVs, colours, polygroups, materials: unchanged. Topology: unchanged, IDs compacted.
    // Refused: a transform whose linear part is singular (|det| below 1e-12 of its scale) - it would flatten
    // the mesh; a non-affine matrix (a projective bottom row).
    [[nodiscard]] Common::ResultStr<TransformedMesh> TransformMesh( const EditMesh& mesh, const glm::mat4& transform );

    // ── Edit Pivot ─────────────────────────────────────────────────────────────────────────────────────

    enum class PivotLocation : uint8_t
    {
        BoundsCenter, // the centre of the mesh's local box
        BoundsBase,   // the centre of the box's bottom face (min Y): an object then stands on its pivot
        WorldOrigin,  // the world's zero, wherever the entity is
        WorldPoint,   // an arbitrary point, given in world space
    };

    [[nodiscard]] const char* ToString( PivotLocation location );

    // The new pivot as a point in the mesh's OWN space. `meshToWorld` is the entity's WORLD transform (parent
    // chain included) - the world-space choices are mapped back through it, so a child entity's pivot lands on
    // the world point, not on its parent-relative image. Refused: an empty mesh (bounds choices); a singular
    // `meshToWorld` (world choices).
    [[nodiscard]] Common::ResultStr<glm::vec3> ResolvePivot( const EditMesh& mesh, PivotLocation location,
                                                             const glm::mat4& meshToWorld,
                                                             const glm::vec3& worldPoint );

    struct XformOutcome
    {
        EditMesh Mesh;
        Trs      Transform; // the entity's new LOCAL transform
        std::string Report;
    };

    // EDIT PIVOT: the entity's origin moves to `pivot` (mesh space) and the vertices move back by the same
    // amount - the mesh is translated by -pivot and Translation grows by (R * S) * pivot, so
    // local' * v' == local * v for every vertex, and the world, which is parent * local, does not change.
    // Rotation and Scale are kept. Refused: a pivot that is not finite; a transform with a zero scale.
    [[nodiscard]] Common::ResultStr<XformOutcome> EditPivot( const EditMesh& mesh, const Trs& local,
                                                             const glm::vec3& pivot );

    // ── Bake Transform ─────────────────────────────────────────────────────────────────────────────────

    struct BakeOptions
    {
        bool Rotation    = true;
        bool Scale       = true;
        bool Translation = false; // UE bakes rotation and scale by default and keeps the location
    };

    // BAKE TRANSFORM: the chosen parts of the local transform move into the vertices and reset on the entity
    // (Translation to 0, Rotation to 0, Scale to 1); the kept parts stay. The mesh is mapped by
    // inverse(local') * local, so the world does not change whatever combination is baked (a rotation baked
    // under a kept non-uniform scale becomes a shear in the vertices - the only map that keeps the world).
    // A negative scale reverses the winding (TransformMesh). Refused: nothing chosen; a transform that is
    // singular (a zero scale: the object has no volume left to bake).
    [[nodiscard]] Common::ResultStr<XformOutcome> BakeTransform( const EditMesh& mesh, const Trs& local,
                                                                 const BakeOptions& options );

    // ── Merge ──────────────────────────────────────────────────────────────────────────────────────────

    struct MergePart
    {
        const EditMesh* Mesh = nullptr;
        // Maps this part's space into the result's (inverse(targetWorld) * partWorld for entities).
        glm::mat4 ToResult{ 1.0f };
        // Material ID i of this part becomes MaterialRemap[i] in the result; empty keeps the IDs. An ID
        // outside the table is refused - the caller built the table from the part's own slots.
        std::vector<int> MaterialRemap;
    };

    struct MergeOutcome
    {
        EditMesh Mesh;
        std::string Report; // layers dropped because some part lacked them
    };

    // MERGE the parts into one mesh (UE's Merge: the inputs' triangles side by side, nothing welded). Each
    // part goes through TransformMesh's rules (so a mirrored part is re-wound). Polygroups stay distinct: part
    // k's groups are shifted past every group of parts 0..k-1. An attribute layer is carried when EVERY part
    // has it - a layer only some parts carry would leave the others' triangles unset, which the render
    // conversion refuses - and the Report names each layer dropped. Refused: fewer than one part, a null
    // mesh, an empty part, a part's transform refused by TransformMesh, a material ID outside its remap.
    [[nodiscard]] Common::ResultStr<MergeOutcome> MergeMeshes( std::span<const MergePart> parts );

    // ── Split ──────────────────────────────────────────────────────────────────────────────────────────

    enum class SplitMethod : uint8_t
    {
        ConnectedComponents, // triangles joined through a shared edge are one part
        PolyGroups,          // each polygroup is one part, connected or not
    };

    [[nodiscard]] const char* ToString( SplitMethod method );

    // SPLIT the mesh into parts, in the order of each part's lowest triangle ID. Each part keeps its
    // triangles' positions (same space - the entities share the source's transform), attributes, seams,
    // polygroups and materials. Refused: an empty mesh; a mesh that is one part already.
    [[nodiscard]] Common::ResultStr<std::vector<EditMesh>> SplitMesh( const EditMesh& mesh, SplitMethod method );

    // ── Pattern ────────────────────────────────────────────────────────────────────────────────────────

    enum class PatternShape : uint8_t
    {
        Line,   // Count copies along AxisA, Spacing apart
        Grid,   // Count x CountB copies over AxisA x AxisB
        Circle, // Count copies around AxisA, the source at Radius from the centre
    };

    [[nodiscard]] const char* ToString( PatternShape shape );

    inline constexpr int kMaxPatternCopies = 1024;

    struct PatternSettings
    {
        PatternShape Shape = PatternShape::Line;
        int          AxisA = 0; // 0 = X, 1 = Y, 2 = Z (in the entity's local frame)
        int          AxisB = 2; // Grid only
        int          Count = 4;
        int          CountB   = 1; // Grid only
        float        Spacing  = 200.0f;
        float        SpacingB = 200.0f; // Grid only
        // Circle: the centre is Radius behind the source along the axis after AxisA, so copy 0 is the source.
        float Radius         = 300.0f;
        float SweepDegrees   = 360.0f; // +-360 = a full ring (no copy doubled on the source)
        bool  OrientToCircle = true;   // each copy turned with its position, like UE's "Orient"
    };

    // The copies' transforms in the source's own space; copy 0 is always the identity (the source stays where
    // it is and the pattern grows from it). Refused: an axis outside 0..2, Grid axes equal, a count below 1,
    // one copy in total, more than kMaxPatternCopies, a zero spacing (copies would coincide), a Circle
    // radius <= 0 or a sweep of 0 or beyond +-360 degrees.
    [[nodiscard]] Common::ResultStr<std::vector<glm::mat4>> PatternTransforms( const PatternSettings& settings );
} // namespace Desert::Geometry
