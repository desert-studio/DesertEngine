#pragma once

// Ported from UE 5.8 Engine/Source/Editor/FoliageEdit/Private/FoliageEdMode.cpp:1007-1030
// (GetRandomVectorInBrush), 1033-1045 (CheckLocationForPotentialInstance_ThreadSafe), 1192-1205
// (IsFilteredByWeight), 1247-1272 (LandscapeLayerCheck), 1273-1316 (CalculatePotentialInstances), 1468-1560
// (AddInstancesImp), 1571-1625 (AddInstancesForBrush), 2736-2745 (the paint branch of ApplyBrush) and
// 261-285 (FFoliagePaintingGeometryFilter), adapted: no UWorld / IFA / FHitResult — the world is two
// callbacks (a segment trace and a layer weight at a point) so the brush is a pure function of its inputs;
// instances are the field's InstancedStaticMesh transforms; Y is up and units are centimetres; FMath::FRand
// is a seeded PCG32 stream (one per stroke) so a stroke replays to the same instances on every platform;
// the geometry filter keeps Landscape and StaticMesh and is applied by the trace (no BSP in this engine;
// translucency is a material property Scene::Raycast does not see); no vertex-colour mask, no AlignMaxAngle, no
// overlap radius (UFoliageType fields this project's type does not carry).

#include <Engine/Assets/Serialization/FoliageType.hpp>

#include <Common/Core/UUID.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace Desert::Editor::Tools
{
    /**
     * @brief PCG32 (O'Neill, pcg-random.org, XSH-RR): the brush's only source of chance.
     *
     * std::uniform_real_distribution is implementation-defined, so libc++ and MSVC would scatter the same seed
     * differently; this stream is defined bit for bit.
     */
    class FoliageRandom
    {
    public:
        explicit FoliageRandom( uint64_t seed );

        uint32_t NextU32();
        /// Uniform in [0, 1), 24 bits.
        float Next01();

    private:
        uint64_t m_State = 0u;
    };

    /// What a trace hit (UE: the component class FFoliagePaintingGeometryFilter tests).
    enum class FoliageSurface : uint8_t
    {
        Landscape,
        StaticMesh,
    };

    /// UE FFoliageUISettings' bFilterLandscape / bFilterStaticMesh: the surfaces the brush may place on.
    struct FoliageSurfaceFilter
    {
        bool Landscape  = true;
        bool StaticMesh = true;

        [[nodiscard]] bool Allows( FoliageSurface surface ) const
        {
            return surface == FoliageSurface::Landscape ? Landscape : StaticMesh;
        }
    };

    struct FoliageTraceHit
    {
        glm::vec3      Point   = glm::vec3( 0.0f );
        glm::vec3      Normal  = glm::vec3( 0.0f, 1.0f, 0.0f );
        FoliageSurface Surface = FoliageSurface::Landscape;
        /// The largest weight, 0..1, any of the type's LandscapeLayers has at Point (UE GetMaxHitWeight);
        /// nullopt when the hit is not a landscape or the type lists no layer.
        std::optional<float> LayerWeight;
    };

    /// The world the brush reads: the nearest surface on a segment THAT THE DAB'S FILTER ALLOWS (a refused
    /// surface is traced through, UE FFoliagePaintingGeometryFilter), and the type's layer weight at a point
    /// (for the instances already under the brush). Supplied by the tool; a test supplies planes.
    struct FoliageBrushWorld
    {
        std::function<std::optional<FoliageTraceHit>( const glm::vec3& start, const glm::vec3& end,
                                                      const FoliageSurfaceFilter& filter )>
                                                                      Trace;
        std::function<std::optional<float>( const glm::vec3& point )> LayerWeightAt;
    };

    /// One application of the brush (UE: one ApplyBrush tick).
    struct FoliageBrushDab
    {
        glm::vec3            Center       = glm::vec3( 0.0f );
        glm::vec3            Normal       = glm::vec3( 0.0f, 1.0f, 0.0f );
        float                Radius       = 300.0f; ///< cm
        float                PaintDensity = 1.0f;   ///< UE UISettings PaintDensity, 0..1
        float                Pressure     = 1.0f;
        FoliageSurfaceFilter Filter;
    };

    /// UE: BrushArea * Density * PaintDensity / (1000 * 1000) — instances the brush disk should hold.
    float FoliageBrushDesiredCount( float density, float radius, float paintDensity );

    /// Where one dab's time went, stage by stage (the dab's log line; FO-3b found a 22 s stroke with it).
    struct FoliageBrushStats
    {
        int    Candidates      = 0; ///< segments traced through the brush sphere
        int    Hits            = 0; ///< segments the world answered on an allowed surface
        int    Passed          = 0; ///< hits that passed height, slope and layer
        int    Placed          = 0;
        double ExistingLayerMs = 0.0; ///< layer weight under the instances already in the sphere
        double GenerateMs      = 0.0; ///< candidate segments
        double TraceMs         = 0.0; ///< world.Trace, including the layer weight at each hit
        double FilterMs        = 0.0; ///< height, slope and layer-weight rules
        double PlaceMs         = 0.0; ///< transforms of the placed instances
    };

    /**
     * @brief The instances one dab adds (UE AddInstancesForBrush): none when the brush sphere already holds
     *        the desired count, otherwise candidates traced through the brush, filtered by surface, height,
     *        slope and landscape layer, and placed per weight bucket up to what is missing.
     *
     * Deterministic: the same @p rng state, inputs and world give the same transforms.
     */
    std::vector<glm::mat4> FoliageBrushAdd( const Assets::Serialization::FoliageTypeData& type,
                                            const FoliageBrushDab& dab, std::span<const glm::mat4> existing,
                                            FoliageRandom& rng, const FoliageBrushWorld& world,
                                            FoliageBrushStats* stats = nullptr );

    /// One field's instances before and after a stroke.
    struct FoliageStrokeField
    {
        Common::UUID           Entity;
        std::vector<glm::mat4> Before;
        std::vector<glm::mat4> After;
    };

    /**
     * @brief A press-to-release stroke: one random stream (so the stroke replays from its seed) and each
     *        touched field as it was at press, so the whole stroke is ONE undo step (UE: one FScopedTransaction).
     */
    class FoliageStroke
    {
    public:
        FoliageStroke( uint64_t seed, bool erase ) : m_Random( seed ), m_Erase( erase )
        {
        }

        FoliageRandom& Random()
        {
            return m_Random;
        }
        [[nodiscard]] bool Erase() const
        {
            return m_Erase;
        }

        /// Remembers @p current as the field's state at press, the first time the stroke touches it.
        void Touch( const Common::UUID& entity, const std::vector<glm::mat4>& current );

        /// The touched fields whose instances differ from press, in the order first touched. @p current
        /// answers a field's instances now, or null when the field is gone (it is then left out).
        std::vector<FoliageStrokeField>
        Finish( const std::function<const std::vector<glm::mat4>*( const Common::UUID& )>& current ) const;

    private:
        FoliageRandom                   m_Random;
        bool                            m_Erase = false;
        std::vector<FoliageStrokeField> m_Touched; // After unused until Finish
    };

    /// UE RemoveInstancesForBrush at UnpaintDensity 0: every instance whose origin lies in the brush sphere.
    /// Returns how many were removed.
    size_t FoliageBrushErase( std::vector<glm::mat4>& instances, const glm::vec3& center, float radius );
} // namespace Desert::Editor::Tools
