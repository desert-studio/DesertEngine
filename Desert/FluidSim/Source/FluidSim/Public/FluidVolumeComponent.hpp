#pragma once

// THE FLUID VOLUME (FLUID-SIM; UE reference: a Niagara Fluids 3D FLIP emitter - a bounded Grid3D, its cell size,
// its particles per cell and the simulation stages that solve it). A world-aligned box centred on the entity in
// which a GPU FLIP/APIC solver moves one or more liquid PHASES of their own density and viscosity: a lighter phase
// rises through a heavier one (oil over water) because the pressure projection divides by each cell's density.
// Box colliders inside the box are solid: the fluid flows around them, and a dynamic rigid body receives the
// pressure the fluid puts on its faces (FluidModule's physics feature).

#include <CoreReflection/ReflectionMacros.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp> // rfl writes the glm members of a list element

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Desert::ECS
{
    // One liquid of the volume. Its index in FluidVolumeData::Phases is what a source and a fill name.
    struct FluidPhaseData
    {
        REFLECT( ScriptStruct )

        PROPERTY( DisplayName( "Name" ), Tooltip( "What the phase is called in the editor and in the logs" ) )
        std::string Name = "Water";

        PROPERTY( DisplayName( "Density" ), Range( 1.0f, 20000.0f ), Units( "kg/m3" ),
                  Tooltip( "Mass per volume. A phase lighter than its neighbour rises through it (oil ~900 over "
                           "water 1000); the solver works in g/cm3 (this / 1000)" ) )
        float Density = 1000.0f;

        PROPERTY(
             DisplayName( "Viscosity" ), Range( 0.0f, 100000.0f ), Units( "cSt" ),
             Tooltip( "Kinematic viscosity in centistokes (water 1, olive oil ~80, honey ~10000); the solver "
                      "works in cm2/s (this / 100) and clamps a value its step cannot carry, with a warning" ) )
        float Viscosity = 1.0f;

        PROPERTY( DisplayName( "Colour" ), Tooltip( "Linear base colour the particle view draws the phase with" ) )
        glm::vec3 Colour{ 0.2f, 0.45f, 0.8f };
    };

    // A box of the volume filled with fluid when the simulation starts (and again on every reset).
    struct FluidFillData
    {
        REFLECT( ScriptStruct )

        PROPERTY( DisplayName( "Centre" ), Length, Tooltip( "The box's centre, relative to the volume's centre" ) )
        glm::vec3 Centre{ 0.0f };

        PROPERTY( DisplayName( "Half Extents" ), Length )
        glm::vec3 HalfExtents{ 50.0f, 50.0f, 50.0f };

        PROPERTY( DisplayName( "Phase" ), Range( 0, 3 ), Tooltip( "Index into Phases" ) )
        int32_t Phase = 0;

        PROPERTY( DisplayName( "Mix Phase" ), Range( -1, 3 ),
                  Tooltip( "A second phase stirred into the box (-1: none): each particle is this phase with "
                           "probability Mix Fraction - a mixed start that the densities separate" ) )
        int32_t MixPhase = -1;

        PROPERTY( DisplayName( "Mix Fraction" ), Range( 0.0f, 1.0f ) )
        float MixFraction = 0.0f;
    };

    struct FluidVolumeData
    {
        REFLECT()

        PROPERTY( DisplayName( "Half Extents" ), Category( "Volume" ), Summary, Length,
                  Tooltip( "Half the size of the simulated box (world-aligned, centred on the entity). Its walls "
                           "are closed: the floor of a pour is the box's bottom" ) )
        glm::vec3 HalfExtents{ 200.0f, 100.0f, 200.0f };

        PROPERTY( DisplayName( "Cell Size" ), Category( "Volume" ), Length, Range( 1.0f, 200.0f ),
                  Tooltip( "The pressure grid's cell edge. Detail and cost both go with (size / cell)^3" ) )
        float CellSize = 10.0f;

        PROPERTY( DisplayName( "Particles Per Cell" ), Category( "Volume" ), Range( 1, 27 ),
                  Tooltip( "Fluid particles seeded per full cell (FLIP keeps 8 for a smooth surface)" ) )
        int32_t ParticlesPerCell = 8;

        PROPERTY( DisplayName( "Max Particles" ), Category( "Volume" ), Range( 1024, 4194304 ),
                  Tooltip( "The particle buffer's capacity; a source stops spawning (with a warning) when full" ) )
        int32_t MaxParticles = 262144;

        PROPERTY( DisplayName( "Steps Per Second" ), Category( "Solver" ), Range( 30.0f, 480.0f ),
                  Tooltip( "Fixed simulation rate; each step is one P2G, pressure solve and G2P" ) )
        float StepsPerSecond = 120.0f;

        PROPERTY( DisplayName( "Max Steps Per Frame" ), Category( "Solver" ), Range( 1, 16 ),
                  Tooltip( "Steps one frame may run; a slower frame drops the rest (logged) and the fluid runs "
                           "slow instead of the frame running away" ) )
        int32_t MaxStepsPerFrame = 4;

        PROPERTY( DisplayName( "Pressure Iterations" ), Category( "Solver" ), Range( 2, 400 ),
                  Tooltip( "Jacobi iterations of the variable-density pressure solve per step (rounded up to "
                           "even; warm-started from the previous step's pressure)" ) )
        int32_t PressureIterations = 60;

        PROPERTY( DisplayName( "FLIP Ratio" ), Category( "Solver" ), Range( 0.0f, 1.0f ),
                  Tooltip( "0 = PIC/APIC (damped, stable), 1 = pure FLIP (lively, noisy)" ) )
        float FlipRatio = 0.95f;

        PROPERTY( DisplayName( "Particle Radius Scale" ), Category( "Render" ), Range( 0.1f, 2.0f ),
                  Tooltip( "Drawn sphere radius as a fraction of the particle spacing (pass 1 particle view)" ) )
        float ParticleRadiusScale = 0.6f;

        PROPERTY( DisplayName( "Phases" ), Category( "Fluid" ),
                  Tooltip( "The liquids of the volume (at most 4)" ) )
        std::vector<FluidPhaseData> Phases{ FluidPhaseData{} };

        PROPERTY( DisplayName( "Initial Fill" ), Category( "Fluid" ),
                  Tooltip( "Boxes filled with fluid when the simulation starts" ) )
        std::vector<FluidFillData> InitialFill;
    };

    struct FluidVolumeComponent
    {
        COMPONENT( Key( "FluidVolume" ), Block( Data ), Run( ActorsAndUI ) )
        FluidVolumeData Data;
    };
} // namespace Desert::ECS
