#pragma once

// Ported from UE 5.8 Engine/Source/Runtime/Landscape/Private/LandscapeGrass.cpp:2044-2070 (SqrtMaxInstances from
// GrassDensity and the extent), 2398-2437 (the Halton placement loop: keep a sample when its weight lies in
// AllowedDensityRange and beats a random fraction; scale, yaw and surface alignment per kept instance) and
// Engine/Source/Runtime/Core/Public/Math/Halton.h (Halton), adapted: the random fraction comes from this
// project's PCG32 (Common/Core/Math/Pcg32.hpp) rather than FRandomStream; UE generates per landscape component in
// async tasks and keys the Halton base index to the component; here the grid is a fixed world grid of
// kLandscapeGrassCellCm cells, the seed is a hash of the cell's integer coordinate (so the same cell grows the
// same grass on every machine and every visit), Y is up
// and the landscape is sampled through a callback, so the generator knows nothing of tiles or ECS.
//
// UE's ALandscapeProxy::UpdateGrass (LandscapeGrass.cpp) streams components in and out around the view with
// a per-frame task budget; GrassCellStreamer is that pattern over the fixed grid.

#include <Engine/Assets/Serialization/LandscapeGrassType.hpp>
#include <Engine/Graphic/InstanceCullDistance.hpp>
#include <Engine/World/Landscape/LandscapeData.hpp>

#include <glm/glm.hpp>

#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace Desert::World::Landscape
{
    /// Edge of one grass cell, cm. A world grid rather than the tile grid: a cell's identity is its integer
    /// coordinate alone, whatever the landscape's tiling, so the seed never depends on layout.
    inline constexpr float kLandscapeGrassCellCm = 2000.0f;

    struct GrassCellCoord
    {
        int32_t X = 0;
        int32_t Z = 0;

        [[nodiscard]] auto operator<=>( const GrassCellCoord& ) const = default;
    };

    /// The cell containing world (x, z).
    GrassCellCoord GrassCellAt( float worldX, float worldZ );

    /// The seed a cell's grass is grown from: a hash of the coordinate and @p salt (which layer and variety),
    /// integer-only so every compiler gives the same bits.
    uint32_t GrassCellSeed( GrassCellCoord cell, uint32_t salt );

    /// UE Halton(Index, Base): the radical inverse of @p index in @p base.
    float GrassHalton( uint32_t index, uint32_t base );

    /// What the landscape is at one point, as the generator needs it. Weight is the layer's, 0..1.
    struct GrassSurfaceSample
    {
        float     HeightCm = 0.0f;
        glm::vec3 Normal   = glm::vec3( 0.0f, 1.0f, 0.0f );
        float     Weight   = 0.0f;
    };

    /// nullopt = no landscape under (x, z) — the sample is skipped, never treated as weight 0 at height 0.
    using GrassSurfaceSampler = std::function<std::optional<GrassSurfaceSample>( float worldX, float worldZ )>;

    /// UE SqrtMaxInstances: the side of the Halton candidate square for one cell of @p variety.
    uint32_t GrassCandidatesPerSide( const Assets::Serialization::GrassVariety& variety );

    /// The distance fade @p variety's instances are drawn with: the foliage path (FO-5), Start to End.
    Graphic::InstanceCullDistance GrassCullDistance( const Assets::Serialization::GrassVariety& variety );

    /**
     * @brief Every instance @p variety grows in @p cell, as world matrices. Deterministic: the same cell, salt,
     * variety and surface give bit-identical matrices.
     */
    std::vector<glm::mat4> GenerateGrassCell( const Assets::Serialization::GrassVariety& variety,
                                              GrassCellCoord cell, uint32_t salt,
                                              const GrassSurfaceSampler& surface );

    /**
     * @brief Layer @p layer's weight at world (x, z), 0..1, bilinear between its four samples; nullopt outside
     * the tile (both edges inclusive, as SampleLandscapeHeight).
     */
    std::optional<float> SampleLandscapeWeight( const LandscapeTileData& tile, const LandscapeFrame& frame,
                                                size_t layer, float worldX, float worldZ );

    /// The cells whose square comes within @p radiusCm of @p cameraXZ, nearest first (ties by coordinate).
    std::vector<GrassCellCoord> GrassCellsInRange( glm::vec2 cameraXZ, float radiusCm );

    /**
     * @brief The cells of one variety that exist around the camera (UE: the grass components of one proxy).
     *
     * Tick evicts every cell that left the radius, then generates the missing ones nearest first — at most
     * @p cellBudget per call, so a camera cut costs a few frames of filling in rather than one long frame.
     */
    class GrassCellStreamer
    {
    public:
        using Generator = std::function<std::vector<glm::mat4>( GrassCellCoord )>;

        struct TickResult
        {
            uint32_t Generated = 0; ///< cells generated this call (<= the budget)
            uint32_t Evicted   = 0; ///< cells dropped because they left the radius
            uint32_t Missing   = 0; ///< cells in range still not generated after this call
        };

        TickResult Tick( glm::vec2 cameraXZ, float radiusCm, uint32_t cellBudget, const Generator& generate );

        /// Drops every cell that overlaps the world rectangle [min, max] (x, z): its surface changed.
        void Invalidate( glm::vec2 worldMin, glm::vec2 worldMax );

        [[nodiscard]] const std::map<GrassCellCoord, std::vector<glm::mat4>>& Cells() const
        {
            return m_Cells;
        }

        /// Every instance of every cell, concatenated in cell order; rebuilt only when the cell set changed.
        std::shared_ptr<const std::vector<glm::mat4>> Instances();

    private:
        std::map<GrassCellCoord, std::vector<glm::mat4>> m_Cells;
        std::shared_ptr<const std::vector<glm::mat4>>    m_Instances;
        bool                                             m_Changed = true;
    };
} // namespace Desert::World::Landscape
