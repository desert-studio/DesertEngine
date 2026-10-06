#pragma once

// THE FRACTURE BAKE: a closed source mesh -> the piece hierarchy a destructible object breaks along (UE's
// fractured UGeometryCollection, made offline by the Fracture Mode tools).
//
// ONE CUT PATH. Every method yields convex cells (FractureCells.hpp); a piece is cut by intersecting it with
// each cell (MeshBoolean Intersect) and splitting each result into its connected islands (UE PlanarCut.cpp
// does the same: a cell that meets the mesh twice gives two pieces). The cell walls become the piece's
// INTERIOR surface: their own material slot (one past the source's highest material ID, UE's "interior
// material" of the fracture tools), box-projected UVs (UE PlanarCut.cpp:1922-2031 box projection) and flat
// normals.
//
// LEVELS (UE's fracture levels): level 0 is the source itself; the cut of level N splits EVERY level N-1 leaf
// with that level's settings, each leaf under its own seed derived from (seed, level, parent node), so
// re-baking one branch never moves another. A leaf the cut leaves whole (one island) stays a leaf.
//
// AUTO-CLUSTER (UE FractureToolAutoCluster.cpp:216 -> FractureEngineClustering.cpp AutoCluster, ByGrid): the
// children of every parent are grouped by k-means over their bounding-box centres, the initial centres a
// GridX x GridY x GridZ lattice over the parent's bounds (GenerateGridSites :1035), refined until no child
// changes group or DriftIterations is spent (FVoronoiPartitioner::KMeansPartition :137, Refine :485). A group
// of two or more children becomes a Cluster node between the parent and them; a group of one stays as it is.
//
// Every number is in centimetres. The bake is deterministic: the same source and settings give the same
// nodes bit for bit on every platform (FractureRandom; sequential cutting, no unordered containers).

#include <Engine/Destruction/FractureCells.hpp>
#include <Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp>
#include <Engine/Geometry/SavedMeshForm.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace Desert::Destruction
{
    enum class FractureMethod : uint8_t
    {
        Uniform,   // UE Uniform Voronoi: SiteCount sites uniform in the piece's bounds
        Clustered, // UE Cluster Voronoi: Clusters centres, SitesPerCluster sites around each
        Planar,    // UE Planar: the arrangement of Planes
        Brick,     // UE Brick: Brick bond pattern
    };

    [[nodiscard]] const char* ToString( FractureMethod method );

    /// One fracture level's cut (UE: one Fracture Mode tool run on the selection of that level).
    struct FractureLevelSettings
    {
        FractureMethod Method    = FractureMethod::Uniform;
        int            SiteCount = 20; // Uniform

        int    Clusters        = 4; // Clustered
        int    SitesPerCluster = 6;
        double MinRadius       = 5.0; // cm
        double MaxRadius       = 30.0;

        std::vector<CutPlane> Planes; // Planar, in the source mesh's space
        BrickSettings         Brick;  // Brick

        /// UE Damage Threshold of this level's pieces: the strain a piece of this level breaks off at.
        float DamageThreshold = 500000.0f;

        bool operator==( const FractureLevelSettings& ) const = default;
    };

    /// UE Auto Cluster, ByGrid.
    struct FractureAutoCluster
    {
        bool Enabled         = false;
        int  GridX           = 2;
        int  GridY           = 2;
        int  GridZ           = 2;
        int  DriftIterations = 0; // UE AutoClusterSettings->DriftIterations

        bool operator==( const FractureAutoCluster& ) const = default;
    };

    struct FractureSettings
    {
        uint64_t                           Seed = 1;
        std::vector<FractureLevelSettings> Levels; // Levels[0] cuts level 0 (the source) into level 1
        FractureAutoCluster                AutoCluster;
        double InteriorUVScale = 0.01; // UV units per cm on the interior faces (1 per metre)

        bool operator==( const FractureSettings& ) const = default;
    };

    enum class FractureNodeKind : uint8_t
    {
        Piece,   // a cut of the source: has geometry while it is a leaf
        Cluster, // an auto-cluster group: geometry only through its children
    };

    /// One node of the hierarchy (UE: one transform of the geometry collection). Geometry and hull live on
    /// leaves only; an inner node is the union of its leaves, as a UE cluster is.
    struct FractureNode
    {
        int32_t          Parent          = -1; // -1 = the root (node 0)
        uint32_t         Level           = 0;  // depth below the root
        FractureNodeKind Kind            = FractureNodeKind::Piece;
        float            DamageThreshold = 0.0f;
        double           Volume          = 0.0; // cm^3, of the node's whole subtree
        glm::dvec3       CenterOfMass{ 0.0 };   // cm, of uniform density

        Geometry::EditMeshSer         Mesh;         // leaves: the piece's closed mesh
        std::vector<glm::vec3>        HullVertices; // leaves: the convex hull (Jolt ConvexHullBuilder)
        std::vector<std::vector<int>> HullFaces;    // polygons into HullVertices, counter-clockwise outside

        bool operator==( const FractureNode& ) const = default;
    };

    struct FractureBakeResult
    {
        int32_t                   InteriorMaterialId = 0;
        std::vector<FractureNode> Nodes; // parents before children; node 0 is the root
    };

    /// Bakes @p source (closed, cm). Refused by name and numbers: an empty or open source, a level with
    /// no usable cut (no sites, no planes, non-positive brick sizes), a cut whose seam the boolean could not
    /// weld, and a hull Jolt could not build.
    [[nodiscard]] Common::ResultStr<FractureBakeResult> BakeFracture( const Geometry::DynamicMesh3& source,
                                                                      const FractureSettings&       settings );

    /// The volume a closed mesh encloses, positive for the port's outward winding.
    [[nodiscard]] double EnclosedVolume( const Geometry::DynamicMesh3& mesh );
} // namespace Desert::Destruction
