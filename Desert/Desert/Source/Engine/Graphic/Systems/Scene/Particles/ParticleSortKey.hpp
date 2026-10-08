#pragma once

#include <glm/glm.hpp>

#include <bit>
#include <cstdint>
#include <vector>

namespace Desert::Graphic::System
{
    // THE BACK-TO-FRONT SORT KEY of a translucent sprite emitter (VFX-08), device-free so a test can hold the
    // ordering the GPU sort produces. The key pass (Programs/Particles/ParticleSortKeys.shader) computes the
    // same function in GLSL — Common/ParticleSortKey.glslh — over each alive particle's view depth (centimetres
    // along the view's forward axis), and the bitonic sort orders keys ASCENDING; so the key is the view depth
    // mapped to an order-preserving unsigned integer and then inverted: the FARTHEST particle gets the smallest
    // key and is drawn first (UE: ENiagaraSortMode::ViewDepth, far to near for translucency).
    //
    // A negative depth (behind the camera) sorts after every positive one — it is nearest, and the GPU clips
    // it anyway. kParticleSortPadKey is what the key pass writes into the padding past the alive count (the
    // sort runs over a power of two): it is the largest key, so padding always lands after every real particle.

    // IEEE-754 float -> unsigned with the same order (negative floats flipped whole, positive ones get the sign
    // bit set): a < b  <=>  OrderedFloatBits(a) < OrderedFloatBits(b), for every non-NaN pair.
    [[nodiscard]] constexpr std::uint32_t OrderedFloatBits( const float value )
    {
        const std::uint32_t bits = std::bit_cast<std::uint32_t>( value );
        return ( bits & 0x80000000u ) != 0u ? ~bits : ( bits | 0x80000000u );
    }

    // Ascending key order == descending view depth (back to front).
    [[nodiscard]] constexpr std::uint32_t ParticleSortKey( const float viewDepthCm )
    {
        return ~OrderedFloatBits( viewDepthCm );
    }

    inline constexpr std::uint32_t kParticleSortPadKey = 0xFFFFFFFFu;

    // THE VIEW DEPTH the key is taken of, in cm: along the camera's forward axis from its position. The view basis
    // looks down its own -Z (the engine's camera, as glm::lookAt), so forward is -InvView[2]; ParticleSort.shader
    // computes dot( position - u_ViewOrigin, u_ViewForward ) from exactly these two vectors (ParticleRenderer
    // pushes them), and the test holds that a farther particle gets the smaller key under a real view matrix.
    struct ParticleSortView
    {
        glm::vec3 Origin{ 0.0f };  // cm, world
        glm::vec3 Forward{ 0.0f }; // unit, world
    };

    [[nodiscard]] inline ParticleSortView ParticleSortViewOf( const glm::mat4& invView )
    {
        return { glm::vec3( invView[3] ), -glm::normalize( glm::vec3( invView[2] ) ) };
    }

    [[nodiscard]] inline float ParticleSortDepth( const ParticleSortView& view, const glm::vec3& positionCm )
    {
        return glm::dot( positionCm - view.Origin, view.Forward );
    }

    // The bitonic network's length for a range of @p capacity particles: the next power of two (at least 2).
    [[nodiscard]] constexpr std::uint32_t ParticleSortLength( const std::uint32_t capacity )
    {
        return capacity <= 2u ? 2u : std::bit_ceil( capacity );
    }

    // THE STAGES of one emitter's sort, in order — one graph node each (Programs/Particles/ParticleSort.shader
    // states what each stage does; the values are its u_Step.x). The network runs every k <= 1024 inside one
    // workgroup's shared memory (Local), and each larger k as the global exchanges with j >= 1024 followed by one
    // Merge that finishes j < 1024 in shared memory.
    enum class ParticleSortStageKind : std::uint32_t
    {
        Keys   = 0,
        Local  = 1,
        Global = 2,
        Merge  = 3,
        Write  = 4
    };

    inline constexpr std::uint32_t kParticleSortBlock     = 1024; // keys a workgroup sorts in shared memory
    inline constexpr std::uint32_t kParticleSortLocalSize = 512;  // ParticleSort's LocalSize x (two keys a thread)

    struct ParticleSortStage
    {
        ParticleSortStageKind Kind   = ParticleSortStageKind::Keys;
        std::uint32_t         K      = 0;
        std::uint32_t         J      = 0;
        std::uint32_t         Groups = 1; // workgroups the stage dispatches
    };

    [[nodiscard]] inline std::vector<ParticleSortStage> ParticleSortStages( const std::uint32_t length )
    {
        const std::uint32_t block     = length < kParticleSortBlock ? length : kParticleSortBlock;
        const std::uint32_t perThread = ( length + kParticleSortLocalSize - 1 ) / kParticleSortLocalSize;
        const std::uint32_t perPair   = ( length / 2 + kParticleSortLocalSize - 1 ) / kParticleSortLocalSize;
        std::vector<ParticleSortStage> stages;
        stages.push_back( { ParticleSortStageKind::Keys, 0, 0, perThread } );
        stages.push_back( { ParticleSortStageKind::Local, 0, 0, length / block } );
        for ( std::uint32_t k = 2 * kParticleSortBlock; k <= length; k *= 2 )
        {
            for ( std::uint32_t j = k / 2; j >= kParticleSortBlock; j /= 2 )
                stages.push_back( { ParticleSortStageKind::Global, k, j, perPair } );
            stages.push_back( { ParticleSortStageKind::Merge, k, 0, length / block } );
        }
        stages.push_back( { ParticleSortStageKind::Write, 0, 0, perThread } );
        return stages;
    }
} // namespace Desert::Graphic::System
