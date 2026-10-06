#pragma once

// The CELLS a fracture cuts a mesh with: convex polytopes that tile a box. Every cutting method ends here —
// Voronoi (the cells of seeded sites), Planar (the arrangement of planes) and Brick (a bond pattern of boxes) —
// so the bake has ONE cut path: intersect the mesh with each cell (FractureBake.cpp, MeshBoolean Intersect).
//
// Port from UE:
//   - Voronoi sites:  ChaosEditor/Source/FractureEditor/Private/FractureToolUniform.cpp:51 (uniform in the
//                     bounds), FractureToolClusterCutter.cpp:50 (clusters of sites around centres).
//   - Voronoi cells:  Engine/Source/Runtime/Experimental/Voronoi/Private/Voronoi/Voronoi.cpp:96-139
//                     (container over the bounds + guess_optimal grid), :218-246 ComputeAllCellsSerial, the
//                     per-cell extraction UE added as voronoicell_neighbor::extractCellInfo — here through
//                     voro++'s public vertices/face_vertices/neighbors, which yield the same arrays.
//   - Brick:          Plugins/Experimental/Fracture/Source/FractureEngine/Private/FractureEngineFracturing.cpp:844
//                     GenerateBrickTransforms (Stretcher and Stack bonds).
//   - Planar:         PlanarCut.cpp FPlanarCells(planes) — the plane arrangement inside the bounds; built with
//                     voro++'s own cell clipping (voronicell::plane) so all three methods share one polytope type.

#include <glm/glm.hpp>

#include <cstdint>
#include <random>
#include <vector>

namespace Desert::Destruction
{
    /// One convex cell. `Faces` index `Vertices`, each polygon wound counter-clockwise seen from OUTSIDE.
    /// `Neighbors[f]` is the cell across face f, or a negative number for the bounding box's walls.
    struct ConvexCell
    {
        std::vector<glm::dvec3>       Vertices;
        std::vector<std::vector<int>> Faces;
        std::vector<int>              Neighbors;
    };

    struct CellBounds
    {
        glm::dvec3 Min{ 0.0 };
        glm::dvec3 Max{ 0.0 };
    };

    /// The seeded generator every fracture method draws from. std::mt19937_64's sequence is fixed by the
    /// standard; std::uniform_real_distribution's mapping is NOT (libc++ and MSVC differ), so the mapping to
    /// [0,1) is written out here — the same seed must cut the same pieces on every platform.
    class FractureRandom
    {
    public:
        explicit FractureRandom( uint64_t seed ) : m_Engine( seed )
        {
        }

        /// Uniform in [0, 1): the top 53 bits of one draw.
        double Unit()
        {
            return static_cast<double>( m_Engine() >> 11 ) * 0x1.0p-53;
        }

        double Range( double lo, double hi )
        {
            return lo + ( hi - lo ) * Unit();
        }

    private:
        std::mt19937_64 m_Engine;
    };

    /// UE FractureToolUniform: `count` sites uniform in the bounds.
    std::vector<glm::dvec3> GenerateUniformSites( const CellBounds& bounds, int count, FractureRandom& random );

    /// UE FractureToolClusterCutter: `clusters` centres uniform in the bounds, then `sitesPerCluster` sites
    /// around each at a distance in [minRadius, maxRadius] (cm) in a uniformly drawn direction.
    std::vector<glm::dvec3> GenerateClusteredSites( const CellBounds& bounds, int clusters, int sitesPerCluster,
                                                    double minRadius, double maxRadius, FractureRandom& random );

    /// The Voronoi cells of `sites` clipped to `bounds` (voro++). A site the container cannot hold (outside
    /// the bounds, or on top of another) yields no cell; the result holds only computed cells, in site order.
    std::vector<ConvexCell> ComputeVoronoiCells( const std::vector<glm::dvec3>& sites, const CellBounds& bounds );

    struct CutPlane
    {
        glm::dvec3 Normal{ 0.0, 0.0, 1.0 }; // need not be unit length
        glm::dvec3 Point{ 0.0 };
    };

    /// The arrangement of `planes` inside `bounds`: every non-empty convex region the planes cut the box into.
    std::vector<ConvexCell> ComputePlanarCells( const std::vector<CutPlane>& planes, const CellBounds& bounds );

    enum class BrickBond : uint8_t
    {
        Stretcher, // running bond: alternate courses offset by half a brick
        Stack,     // no offset between courses
    };

    struct BrickSettings
    {
        BrickBond Bond   = BrickBond::Stretcher;
        double    Length = 194.0; // cm, UE's defaults (FractureToolBrick.h:49)
        double    Height = 52.0;
        double    Depth  = 96.0;
    };

    /// Axis-aligned bricks covering `bounds` in the bond pattern, each clipped to the bounds.
    std::vector<ConvexCell> ComputeBrickCells( const BrickSettings& settings, const CellBounds& bounds );
} // namespace Desert::Destruction
