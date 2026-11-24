#pragma once

// THE GPU LAYOUT OF ONE FLUID VOLUME, C++ SIDE (FLUID-SIM; UE reference: Niagara Fluids' Grid3D FLIP solver - a
// particle set carrying the fluid's mass and a MAC grid that solves the pressure each step). The GLSL side is
// Editor/Resources/Shaders/Common/FluidSim.glslh: every constant here has its twin there, and the two are compared
// by name in the comments - a change is made to both or the solver reads garbage.
//
// Units: 1 world unit = 1 cm. Densities are carried in g/cm^3 (water 1.0, the component authors kg/m^3 and
// divides by 1000), so the pressure the solver produces is in barye (g / (cm s^2) = 0.1 Pa) and a force summed
// over cell faces is in dyn (g cm / s^2). PhysicsWorld::AddForce takes kg cm / s^2: dyn / 1000.

#include <glm/glm.hpp>

#include <cstdint>

namespace Desert::FluidSim
{
    // Particle (Common/FluidSim.glslh FluidParticle), std430, 80 bytes.
    struct FluidParticleGpu
    {
        glm::vec4 PositionPhase{}; // xyz world position (cm), w = phase index (< 0: never spawned)
        glm::vec4 Velocity{};      // xyz cm/s
        glm::vec4 AffineX{};       // APIC: the gradient of the x velocity component around the particle (1/s)
        glm::vec4 AffineY{};
        glm::vec4 AffineZ{};
    };
    static_assert( sizeof( FluidParticleGpu ) == 80u, "FluidParticle: std430 size" );

    // Grid fields (FLUID_FIELD_*): one float array of (field x cell count) in the volume's Grid buffer. A face
    // velocity of cell (i, j, k) is the one on the cell's LOW face along that axis (MAC staggering); the domain's
    // high walls are implicit and closed.
    enum class GridField : uint32_t
    {
        U = 0,      // x velocity at the low x face (cm/s)
        V,          // y velocity at the low y face
        W,          // z velocity at the low z face
        U0,         // the velocities P2G produced, before forces and projection (FLIP's delta base)
        V0,         //
        W0,         //
        Density,    // cell density (g/cm^3), mass-weighted mean of the phases in the cell
        Viscosity,  // cell kinematic viscosity (cm^2/s)
        CellType,   // 0 air, 1 fluid, 2 solid (FLUID_CELL_*)
        SolidBody,  // the body index + 1 a solid cell belongs to, 0 for the domain walls
        Divergence, // the divergence of the forced velocity (1/s)
        PressureA,  // Jacobi ping
        PressureB,  // Jacobi pong; PressureA holds the solution after an even iteration count (warm start)
        UTemp,      // the viscous step's output
        VTemp,      //
        WTemp,      //
        Count
    };
    inline constexpr uint32_t kGridFieldCount = static_cast<uint32_t>( GridField::Count );

    // Integer accumulators of P2G (FLUID_ACC_*): fixed point, kFixedScale.
    enum class AccumField : uint32_t
    {
        UMomentum = 0,
        UWeight,
        VMomentum,
        VWeight,
        WMomentum,
        WWeight,
        Mass, // sum of weight x phase density at cell centres
        MassWeight,
        Viscous,   // sum of weight x phase viscosity
        Particles, // particles whose position lies in the cell (the fluid mark)
        Count
    };
    inline constexpr uint32_t kAccumFieldCount = static_cast<uint32_t>( AccumField::Count );

    inline constexpr float kFixedScale = 4096.0f; // FLUID_FIXED_SCALE

    // Counters (FLUID_COUNTER_*): uint words of the persistent Counters buffer.
    inline constexpr uint32_t kCounterAlive = 0; // particles spawned so far (none die: they rest in the volume)
    inline constexpr uint32_t kCounterWords = 4;

    // The per-frame table (Frame buffer, vec4 rows): phases, then sources, then bodies.
    inline constexpr uint32_t kMaxPhases       = 4;  // FLUID_MAX_PHASES
    inline constexpr uint32_t kMaxSources      = 8;  // FLUID_MAX_SOURCES
    inline constexpr uint32_t kMaxBodies       = 16; // FLUID_MAX_BODIES
    inline constexpr uint32_t kPhaseRows       = 2;  // DensityViscosity, Colour
    inline constexpr uint32_t kSourceRows      = 3;  // PositionRadius, VelocityPhase, Spawn (uint bits)
    inline constexpr uint32_t kBodyRows        = 5;  // CentreDynamic, HalfExtents, Rotation, Linear, Angular
    inline constexpr uint32_t kFrameSourceBase = kMaxPhases * kPhaseRows;                    // FLUID_FRAME_SOURCES
    inline constexpr uint32_t kFrameBodyBase = kFrameSourceBase + kMaxSources * kSourceRows; // FLUID_FRAME_BODIES
    inline constexpr uint32_t kFrameRows     = kFrameBodyBase + kMaxBodies * kBodyRows;

    // Body forces (Forces readback buffer): per body force xyz then torque xyz, ints. Force in kg cm/s^2 times
    // kForceScale, torque in kg cm^2/s^2 times kTorqueScale (FLUID_FORCE_SCALE / FLUID_TORQUE_SCALE). The scales
    // keep a 2 m column of water on a 1 m box (~2e6 kg cm/s^2 summed) and its lever arms inside an int.
    inline constexpr uint32_t kForceWordsPerBody = 6;
    inline constexpr float    kForceScale        = 4.0f;
    inline constexpr float    kTorqueScale       = 0.0625f;

    inline constexpr uint32_t kLocalSize = 64; // every FluidSim compute program: LocalSize(64, 1, 1)

    // One volume's grid as the solver sees it: computed from the component (FluidGridOf), never stored apart.
    struct FluidGrid
    {
        glm::uvec3             Cells{ 0u };     // cells per axis
        glm::vec3              Origin{ 0.0f };  // the domain's min corner (cm)
        float                  CellSize = 0.0f; // cm
        [[nodiscard]] uint32_t CellCount() const
        {
            return Cells.x * Cells.y * Cells.z;
        }
    };
} // namespace Desert::FluidSim
