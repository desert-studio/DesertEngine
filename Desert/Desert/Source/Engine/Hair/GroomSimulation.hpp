#pragma once

// HAIR SEAMS — the guide simulation a backend implements, its factory, and the data contract the renderer
// reads. No renderer is declared here: only what it receives.
//
// UE counterpart: the groom's Niagara-driven guide solver (UGroomComponent + the HairStrands simulation
// system) and FHairGroupInstance, whose deformed-positions buffers are what the hair renderer reads.
// Pattern, not letter: UE runs guides on the GPU through Niagara and interpolates strands in compute
// shaders; here the seam says only WHAT flows (guide points in, strand points out), so HAIR1 may simulate
// on the CPU or the GPU without the renderer noticing.
//
// NOT HERE: HAIR1 — a backend (its factory), the interpolation of strands from guides, the renderer.

#include <Engine/Hair/GroomAsset.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace Desert::Hair
{
    // One fixed step. The skinned mesh's CURRENT triangle vertices (component space, cm) are what the roots
    // follow through GroomBinding; the simulation keeps no pointer past Step().
    struct GroomStepContext
    {
        float                      FixedDeltaSeconds = 0.0f;
        glm::mat4                  ComponentToWorld  = glm::mat4( 1.0f );
        std::span<const glm::vec3> SkinnedMeshPositions;
        std::span<const uint32_t>  SkinnedMeshTriangleIndices;
        glm::vec3                  Gravity      = { 0.0f, -980.0f, 0.0f }; // cm/s^2, from the physics world
        glm::vec3                  WindVelocity = { 0.0f, 0.0f, 0.0f };    // cm/s; source: see ClothStepContext
    };

    class IGroomSimulation
    {
    public:
        virtual ~IGroomSimulation() = default;

        // Deterministic in the sequence of contexts, like cloth. Fails on dt <= 0 or a mesh that does not
        // match the binding (triangle count), naming both numbers.
        virtual Common::BoolResultStr Step( const GroomStepContext& context ) = 0;

        // Simulated guide points of one group, component space, parallel to HairGroup::Guides.Points.
        [[nodiscard]] virtual std::span<const glm::vec3> GetGuidePositions( uint32_t group ) const = 0;

        // Back to the bound rest shape (after a teleport or a respawn).
        virtual void Reset() = 0;
    };

    class IGroomSimulationFactory
    {
    public:
        virtual ~IGroomSimulationFactory() = default;

        [[nodiscard]] virtual std::string_view GetName() const = 0;

        // Fails when the binding was built for another groom (handle mismatch) or its per-group arrays do not
        // match the asset's curve counts.
        [[nodiscard]] virtual Common::ResultStr<std::unique_ptr<IGroomSimulation>>
        CreateSimulation( const GroomAsset& asset, const GroomBinding& binding ) = 0;
    };

    // WHAT THE RENDERER RECEIVES for one group in one frame: deformed render strands in world space. The
    // curve layout (offsets/counts/radius) is the asset's and does not change per frame, so only positions
    // are per-frame. Spans are valid until the source's next update; the renderer copies what it keeps.
    struct HairRenderGroup
    {
        std::span<const glm::vec3> Positions;        // parallel to HairGroup::Strands.Points
        std::span<const float>     PointRadius;      // = HairGroup::Strands.PointRadius
        std::span<const uint32_t>  CurvePointOffset; // = HairGroup::Strands.CurvePointOffset
        std::span<const uint32_t>  CurvePointCount;  // = HairGroup::Strands.CurvePointCount
        Common::AssetHandle        Material;
    };

    struct HairRenderFrame
    {
        uint64_t Revision = 0; // bumps whenever Positions change; the renderer skips uploads on equal
        std::vector<HairRenderGroup> Groups;
    };

    // Implemented by the groom instance (simulated or static); read by the hair renderer once per frame.
    class IGroomRenderDataSource
    {
    public:
        virtual ~IGroomRenderDataSource() = default;

        [[nodiscard]] virtual HairRenderFrame GetRenderFrame() const = 0;
    };
} // namespace Desert::Hair
