#pragma once

// Ported from UE 5.8 Engine/Source/Editor/FoliageEdit/Private/FoliageEdMode.cpp:1007-1030
// (GetRandomVectorInBrush), 1033-1045 (CheckLocationForPotentialInstance_ThreadSafe), 1192-1205
// (IsFilteredByWeight), 1247-1272 (LandscapeLayerCheck), 1273-1316 (CalculatePotentialInstances), 1468-1560
// (AddInstancesImp), 1571-1625 (AddInstancesForBrush), 2736-2745 (the paint branch of ApplyBrush),
// 1550-1568 (AddSingleInstanceForBrush), 1718-1760 (SelectInstanceAtLocation, SelectInstancesForBrush),
// 1980-2120 (TransformSelectedInstances, RemoveSelectedInstances), 2292-2560 (ReapplyInstancesForBrush) and
// 261-285 (FFoliagePaintingGeometryFilter), adapted: no UWorld / IFA / FHitResult — the world is two
// callbacks (a segment trace and a layer weight at a point) so the brush is a pure function of its inputs;
// instances are the field's InstancedStaticMesh transforms; Y is up and units are centimetres; FMath::FRand
// is a seeded PCG32 stream (one per stroke) so a stroke replays to the same instances on every platform;
// the geometry filter keeps Landscape and StaticMesh and is applied by the trace (no BSP in this engine;
// translucency is a material property Scene::Raycast does not see); no vertex-colour mask, no AlignMaxAngle, no
// overlap radius (UFoliageType fields this project's type does not carry); an instance is a bare transform, so
// Reapply recovers its offset, yaw and scale from the matrix instead of per-instance flags.

#include <Engine/Assets/Serialization/FoliageType.hpp>

#include <Common/Core/UUID.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <utility>
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

    /// A field's selected instances: indices into its InstanceTransforms, ascending, each once (UE
    /// FFoliageInfo::SelectedIndices).
    using FoliageSelection = std::vector<uint32_t>;

    /// One field's instances and selection before and after a stroke.
    struct FoliageStrokeField
    {
        Common::UUID           Entity;
        std::vector<glm::mat4> Before;
        std::vector<glm::mat4> After;
        FoliageSelection       SelectedBefore;
        FoliageSelection       SelectedAfter;
    };

    /**
     * @brief A press-to-release stroke: one random stream (so the stroke replays from its seed) and each
     *        touched field as it was at press, so the whole stroke is ONE undo step (UE: one FScopedTransaction).
     */
    class FoliageStroke
    {
    public:
        explicit FoliageStroke( uint64_t seed ) : m_Random( seed )
        {
        }

        FoliageRandom& Random()
        {
            return m_Random;
        }

        /// Remembers @p current (and its selection) as the field's state at press, the first time the stroke
        /// touches it.
        void Touch( const Common::UUID& entity, const std::vector<glm::mat4>& current,
                    const FoliageSelection& selected = {} );

        /// The instance origins Reapply already readjusted in this stroke, per field (UE FOLIAGE_Readjusted: a
        /// dab passing over an instance twice must not re-roll it twice).
        std::vector<glm::vec3>& Readjusted( const Common::UUID& entity );

        /// The touched fields whose instances or selection differ from press, in the order first touched.
        /// @p current answers a field's instances now, or null when the field is gone (it is then left out);
        /// @p selected its selection now (none given: the selection is not part of this stroke).
        std::vector<FoliageStrokeField>
        Finish( const std::function<const std::vector<glm::mat4>*( const Common::UUID& )>& current,
                const std::function<FoliageSelection( const Common::UUID& )>&              selected = {} ) const;

    private:
        FoliageRandom                                                m_Random;
        std::vector<FoliageStrokeField>                              m_Touched; // After unused until Finish
        std::vector<std::pair<Common::UUID, std::vector<glm::vec3>>> m_Readjusted;
    };

    /// UE's Remove tool (RemoveInstancesForBrush at a desired count of 0): every instance whose origin lies in
    /// the brush sphere. @p selected, when given, is renumbered to the instances that remain. Returns how many
    /// were removed.
    size_t FoliageBrushRemove( std::vector<glm::mat4>& instances, const glm::vec3& center, float radius,
                               FoliageSelection* selected = nullptr );

    /**
     * @brief UE's Single tool (AddSingleInstanceForBrush): one instance where the brush centre lies, traced
     *        through a 1 cm segment along the brush normal, kept only when its surface and the type's height,
     *        slope and layer rules pass. No density limit.
     */
    std::optional<glm::mat4> FoliageBrushSingle( const Assets::Serialization::FoliageTypeData& type,
                                                 const FoliageBrushDab& dab, FoliageRandom& rng,
                                                 const FoliageBrushWorld& world );

    /// UE SelectInstancesForBrush (the Lasso tool): the instances whose origin lies in the sphere join the
    /// selection (@p select) or leave it. Returns how many changed state.
    size_t FoliageSelectInSphere( std::span<const glm::mat4> instances, const glm::vec3& center, float radius,
                                  bool select, FoliageSelection& selected );

    /// What the Select tool's ray hit: the instance and the distance along the ray to its box, cm.
    struct FoliagePick
    {
        uint32_t Index    = 0;
        float    Distance = 0.0f;
    };

    /// UE SelectInstanceAtLocation (the Select tool's click), without hit proxies: the instance whose mesh box
    /// - the mesh's local bounds @p localMin..@p localMax carried by the instance transform - the ray enters
    /// first. nullopt when the ray misses every box.
    std::optional<FoliagePick> FoliagePickInstance( std::span<const glm::mat4> instances,
                                                    const glm::vec3& rayOrigin, const glm::vec3& rayDirection,
                                                    const glm::vec3& localMin, const glm::vec3& localMax );

    /// A world-space triangle of the mesh the Fill tool covers (UE FFoliagePaintBucketTriangle, no vertex colour).
    struct FoliageFillTriangle
    {
        glm::vec3      A       = glm::vec3( 0.0f );
        glm::vec3      B       = glm::vec3( 0.0f );
        glm::vec3      C       = glm::vec3( 0.0f );
        FoliageSurface Surface = FoliageSurface::StaticMesh;
    };

    /**
     * @brief UE's Fill tool (ApplyPaintBucket_Add): every triangle the filter allows and whose normal is within
     *        the type's slope range gets Area * Density * PaintDensity / (1000 * 1000) instances at uniform
     *        random points (a fraction below one is a chance of one), each kept when the type's height rule
     *        passes, placed like a brush instance. Area-weighted by construction; deterministic in @p rng.
     */
    std::vector<glm::mat4> FoliageFill( const Assets::Serialization::FoliageTypeData& type,
                                        std::span<const FoliageFillTriangle> triangles, float paintDensity,
                                        const FoliageSurfaceFilter& filter, FoliageRandom& rng );

    /// UE RemoveInstancesForBrush at a desired count: while the sphere holds more than @p desired instances, a
    /// random choice of them (from @p rng) leaves the field. @p selected is renumbered. Returns how many left.
    size_t FoliageBrushThin( std::vector<glm::mat4>& instances, const glm::vec3& center, float radius, int desired,
                             FoliageRandom& rng, FoliageSelection* selected = nullptr );

    /// UE RemoveSelectedInstances: the selected instances leave the field and the selection is emptied.
    size_t FoliageRemoveSelected( std::vector<glm::mat4>& instances, FoliageSelection& selected );

    /// UE TransformSelectedInstances (a drag): the selected instances move by @p offset cm.
    void FoliageMoveSelected( std::vector<glm::mat4>& instances, const FoliageSelection& selected,
                              const glm::vec3& offset );

    /// UFoliageType's Reapply* switches, as tool settings (the pattern, not the letter: this project keeps them
    /// with the brush so the `.defoliage` format does not grow for an editor-only choice). Each set switch
    /// re-rolls or re-checks that property from the type's CURRENT numbers; a clear one keeps the instance's.
    struct FoliageReapplySettings
    {
        bool Scale           = true;  ///< UE ReapplyScaling: a new uniform scale from ScaleX
        bool ZOffset         = false; ///< UE ReapplyZOffset: a new offset from ZOffset
        bool AlignToNormal   = true;  ///< UE ReapplyAlignToNormal: align to the ground, or stand upright
        bool RandomYaw       = false; ///< UE ReapplyRandomYaw: a new yaw, or yaw 0 when the type has none
        bool RandomPitch     = false; ///< UE ReapplyRandomPitchAngle: a new tilt up to RandomPitchAngle
        bool GroundSlope     = true; ///< UE ReapplyGroundSlope: remove where the ground is outside the slope range
        bool Height          = true; ///< UE ReapplyHeight: remove where the ground is outside the height range
        bool LandscapeLayers = true; ///< UE ReapplyLandscapeLayers: remove where the layer weight filters it out
        bool Density = false; ///< UE ReapplyDensity: thin or top up the sphere to the type's current Density
    };

    struct FoliageReapplyResult
    {
        size_t Updated = 0; ///< rebuilt from the current settings
        size_t Removed = 0; ///< failed a re-checked filter
        size_t Skipped = 0; ///< no ground under the instance along its up axis: left as it was
        size_t Added   = 0; ///< placed to reach the type's density (Density switch)
        size_t Thinned = 0; ///< taken out to come down to the type's density (Density switch)
    };

    /**
     * @brief UE's Reapply tool (ReapplyInstancesForBrush): each instance in the sphere not yet readjusted this
     *        stroke finds its ground along its own up axis, is removed when a re-checked filter refuses the
     *        ground, and is otherwise rebuilt with the switched properties re-rolled from @p type and the rest
     *        kept (scale, yaw, Z offset above the ground, up axis).
     *
     * With the Density switch the sphere is first brought to the type's CURRENT Density (UE
     * ReapplyInstancesDensityForBrush, which scales by a DensityAdjustmentFactor instead): thinned by a random
     * choice, or topped up by FoliageBrushAdd; the new instances count as readjusted.
     *
     * @p readjusted holds the origins already rebuilt this stroke (FoliageStroke::Readjusted); @p selected, when
     * given, is renumbered past removals. Deterministic in @p rng.
     */
    FoliageReapplyResult FoliageBrushReapply( const Assets::Serialization::FoliageTypeData& type,
                                              const FoliageReapplySettings& settings, const FoliageBrushDab& dab,
                                              std::vector<glm::mat4>& instances,
                                              std::vector<glm::vec3>& readjusted, FoliageRandom& rng,
                                              const FoliageBrushWorld& world,
                                              FoliageSelection*        selected = nullptr );
} // namespace Desert::Editor::Tools
