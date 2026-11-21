#pragma once

// WHAT a fracture level asks the cells for (FractureCells.hpp makes them): the authored settings of the Radial,
// Planar and Brick methods. Apart from the cell generator so that the bake's settings (FractureBake.hpp), the
// `.dfrac` format and everything that reads a fracture asset name the settings without compiling the generator
// (BUILD-MOD-1: FractureCells.hpp reached every object that reached the fracture format).

#include <glm/glm.hpp>

#include <cstdint>

namespace Desert::Destruction
{
    /// UE Fracture Mode "Radial" (FractureToolRadial.cpp GenerateVoronoiSites), made three-dimensional: UE lays
    /// AngularSteps sites on each of RadialSteps RINGS in the plane of a normal, so its cells are wedges through
    /// the whole object; here each ring is a SHELL (a sphere) around the centre, so the cells are small at the
    /// centre and grow outward in every direction — what a hit does to a solid (the At Impact cut, DST-RUNTIME).
    struct RadialSettings
    {
        glm::dvec3 Center{ 0.0 };    // cm, in the space of the mesh being cut
        double     Radius = 50.0;    // cm: the outermost shell sits at Radius * (RadialSteps - 0.5) / RadialSteps
        int        RadialSteps  = 4; // shells, UE RadialSteps
        int        AngularSteps = 8; // sites per shell, UE AngularSteps
        double     Variability  = 0.0; // cm: each site moves by up to this in a random direction (UE Variability)

        bool operator==( const RadialSettings& ) const = default;
    };

    struct CutPlane
    {
        glm::dvec3 Normal{ 0.0, 0.0, 1.0 }; // need not be unit length
        glm::dvec3 Point{ 0.0 };

        bool operator==( const CutPlane& ) const = default;
    };

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

        bool operator==( const BrickSettings& ) const = default;
    };
} // namespace Desert::Destruction
