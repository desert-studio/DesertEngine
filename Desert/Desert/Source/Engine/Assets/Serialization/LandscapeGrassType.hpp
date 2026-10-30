#pragma once

// Ported from UE 5.8 Engine/Source/Runtime/Landscape/Classes/LandscapeGrassType.h:20-190 (EGrassScaling,
// FGrassVariety, ULandscapeGrassType::GrassVarieties), adapted: a plain struct read by reflect-cpp instead of a
// UObject with UPROPERTY metadata; the mesh named by {Guid, Path} like every other reference in a text asset;
// per-platform / per-quality numbers collapsed to one value. The mesh shades with its own section materials (UE
// without OverrideMaterials). Refused, because nothing here could consume them (a field nothing reads is a dead
// setting): bUseGrid=false (Halton placement; the GPU generator places on the grid), MinLOD, the lightmap / decal
// / lighting-channel / distance-field flags, ExcludedLandscapes and bEnableDensityScaling. Added: `Kind` (what a
// variety draws), the procedural blade (GRASS-BLADE; UE has none - Ghost of Tsushima's GDC 2021 "Procedural
// Grass": the blade is built in the vertex stage from a few numbers, no mesh) and the variety's wind (UE's is the
// grass material's SimpleGrassWind WPO; here the WPO parameters live with the instanced type, as FoliageWind).

#include <Engine/Assets/AssetGuidRef.hpp>
#include <Engine/Assets/Serialization/FoliageType.hpp>
#include <Engine/Assets/TextAssetHeaderStamp.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Desert::Assets::Serialization
{
    /// The extension the Content Browser and the loader agree on.
    inline constexpr const char* kLandscapeGrassTypeExtension = ".degrasstype";

    /**
     * @brief The FILE layout's generation.
     *
     *   1, 2 - GR-1 / GR-2's layout, removed with the CPU grass streamer in LS-16b; no such file is tracked.
     *   3    - the text asset header (Kind "LandscapeGrassType", the GUID that IS the type's identity and handle,
     *          this number under `LGRT`, one Dependency per distinct variety mesh or blade material) and
     *          GrassVarieties, each with a Kind (GRASS-GPU) - a Mesh variety names its Mesh, a Blade variety its
     *          Material and its Blade shape (GRASS-BLADE; still unreleased, so the same number).
     *
     * An unknown value is refused in both directions; there is no migration step in the runtime.
     */
    inline constexpr int32_t kLandscapeGrassTypeVersion =
         static_cast<int32_t>( Assets::kLandscapeGrassTypeSchemaVersion );

    [[nodiscard]] inline std::span<const Common::Content::SubsystemVersion> LandscapeGrassTypeTextSubsystems()
    {
        static const std::array<Common::Content::SubsystemVersion, 1> versions = {
             Common::Content::SubsystemVersion{ Assets::kLandscapeGrassTypeSchemaTag,
                                                static_cast<uint32_t>( kLandscapeGrassTypeVersion ) } };
        return versions;
    }

    /// What a variety draws. Mesh: instances of a static mesh (UE GrassMesh). Blade: one procedural blade per
    /// instance, its vertices placed by the vertex stage (Mesh/Surface/Vertex_GrassBlade.glslh) from GrassBlade.
    enum class GrassVarietyKind
    {
        Mesh,
        Blade,
    };

    /// The upper bound of GrassBlade::Segments: the strip's level chain halves it down to one segment, so four
    /// levels at most (15, 7, 3, 1) - the size of the generator's bucket table.
    inline constexpr uint32_t kGrassBladeMaxSegments = 15;

    /// The most cards a Blade variety draws from (Mesh/Surface/GrassBlade.glslh GRASS_BLADE_MAX_CARDS).
    inline constexpr size_t kGrassBladeMaxCards = 8;

    /**
     * @brief How a blade card stands (Kind Blade). Its look - outline, colour, veins, dry tips - is the variety's
     * material: a scanned blade atlas (UE's Landscape Grass Type draws a foliage material too). The blade names
     * which rectangles of that atlas are its cards; the vertex stage bends one card per instance, as wide as the
     * card is at the blade's height. Per-instance size comes from the variety's scale ranges (ScaleX the width,
     * ScaleZ the height, as for a mesh), the facing from RandomRotation and the clump.
     */
    struct GrassBlade
    {
        /// Blade height, cm, at scale 1.
        float Height = 45.0f;
        /// The cards on the material's atlas, each (u0, vTip, u1, vRoot) in texture space (v down the image, the
        /// tip above the root): 1..kGrassBladeMaxCards, each instance draws one uniformly (repeat a card to draw
        /// it more often).
        std::vector<std::array<float, 4>> Cards;
        /// How far the tip leans forward at rest, as a share of the height (0 = upright).
        float Curvature = 0.35f;
        /// 0..1: how far each blade's own lean strays from Curvature, either way (0.4: from 0.6 to 1.4 times it).
        float CurvatureSpread = 0.4f;
        /// 0..1: how far each blade's bend gathers toward its tip (0 = every blade one even arc; 1 = some stay a
        /// straight stalk that bends only near the tip), drawn per blade.
        float BendSpread = 0.0f;
        /// Degrees, 0..60: the largest angle a blade's root stands off its up axis, drawn per blade in a direction
        /// of its own (0 = every blade rises upright and only its curvature leans it).
        float Tilt = 0.0f;
        /// Degrees, 0..180: how far each blade's facing turns, either way, after it follows its clump (0 = the
        /// clump's share of the facing alone; restores the per-blade facing a strong Clumping takes away).
        float FacingJitter = 0.0f;
        /// 0..1: the share of the wind's sway the blade resists (1 = it does not move).
        float Stiffness = 0.3f;
        /// 0..1: how strongly blades follow their clump - its facing, its height and its tint (0 = independent).
        float Clumping = 0.5f;
        /// The clump cell's side, cm.
        float ClumpSize = 35.0f;
        /// 0..1: how far a blade moves from its grid place towards its clump's point (0 = stays on the jittered
        /// grid, 1 = all of a clump's blades stand on its point). Gathers the field into tufts with open ground
        /// between.
        float ClumpPull = 0.0f;
        /// 0..1: how far, in grid cells times two, a blade strays past its own cell (0 = stays in its cell, one
        /// evenly spaced stalk per cell; 1 = up to two cells off, an uneven carpet).
        float Scatter = 0.0f;
        /// 0..1: the largest share of blades thinned by broad world patches (0 = even; 0.5 = the sparsest patch
        /// keeps half). Denser and sparser patches without bare ground.
        float Patchiness = 0.0f;
        /// The patch noise's cell, cm.
        float PatchSize = 800.0f;
        /// Segments of a near blade (1..kGrassBladeMaxSegments); each farther level halves them.
        uint32_t Segments = 6;
        /// The distance, cm, over which a level is drawn before the next, coarser one.
        float LodDistance = 700.0f;

        [[nodiscard]] bool operator==( const GrassBlade& ) const = default;
    };

    /// UE EGrassScaling: how a variety's three scale ranges combine.
    enum class GrassScaling
    {
        Uniform, ///< one draw of ScaleX for all three axes
        Free,    ///< each axis its own draw
        LockXY,  ///< X and Y share ScaleX's draw, Z (up) draws ScaleZ
    };

    /**
     * @brief Where in the world a variety grows: broad noise patches with open ground between (wildflowers in
     * drifts, tussocks in clumps). UE draws this with a noise term in the landscape material's grass output
     * (ULandscapeGrassOutput's per-type weight); here the mask belongs to the variety, so a type authors it with
     * the meshes it shapes. It multiplies the layer weight before the instance's keep draw, so a patch's rim
     * thins instead of ending in a line.
     */
    struct GrassPatches
    {
        /// 0..1: about the share of the ground the variety grows on (1 = everywhere, no mask).
        float Coverage = 1.0f;
        /// The patch noise's cell, cm (a patch is about half of it across).
        float Size = 1500.0f;
        /// 0..1 of the noise range over which the density rises at a patch's rim (0 = a hard rim).
        float Softness = 0.15f;
        /// Varieties with the same non-zero Group share one noise (the same drifts: a flower's
        /// several meshes); 0 = the variety's own.
        uint32_t Group = 0;

        [[nodiscard]] bool operator==( const GrassPatches& ) const = default;
    };

    /**
     * @brief One kind of grass a type grows (UE: FGrassVariety, LandscapeGrassType.h:30-158).
     *
     * Units are UE's: Density is instances per 1000 x 1000 cm (as FoliageTypeData::Density), distances are cm,
     * Z is the up axis (the engine's Y).
     */
    struct GrassVariety
    {
        GrassVarietyKind Kind = GrassVarietyKind::Mesh;
        /// The static mesh every instance draws: required for Kind Mesh, absent for Blade.
        std::optional<AssetGuidRef> Mesh;
        /// The material a blade shades with, whose template declares `Usage GrassBlades`: required for Kind
        /// Blade, absent for Mesh (a mesh shades with its own section materials).
        std::optional<AssetGuidRef> Material;
        /// The blade's shape: required for Kind Blade, absent for Mesh.
        std::optional<GrassBlade> Blade;
        float                     Density = 400.0f;
        /// 0..1 of a grid cell the instance may wander from the cell centre (UE PlacementJitter, clamped to .99).
        float PlacementJitter   = 1.0f;
        float StartCullDistance = 0.0f;
        float EndCullDistance   = 10000.0f;
        /// The layer weight an instance needs: kept when Min < weight <= Max and weight beats its random draw.
        FoliageFloatInterval AllowedDensityRange{ 0.0f, 1.0f };
        /// The world patches the variety grows in (Coverage 1: everywhere the layer is).
        GrassPatches         Patches;
        GrassScaling         Scaling = GrassScaling::Uniform;
        FoliageFloatInterval ScaleX{ 1.0f, 1.0f };
        FoliageFloatInterval ScaleY{ 1.0f, 1.0f };
        FoliageFloatInterval ScaleZ{ 1.0f, 1.0f };
        bool                 WeightAttenuatesMaxScale  = false;
        float                MaxScaleWeightAttenuation = 0.5f;
        bool                 RandomRotation            = true;
        bool                 AlignToSurface            = true;
        bool                 CastDynamicShadow         = true;
        /// The response to the scene's wind (ECS::WindAt), as a foliage type's (FoliageWind).
        FoliageWind Wind;

        [[nodiscard]] bool operator==( const GrassVariety& ) const = default;
    };

    /**
     * @brief One `.degrasstype` (UE ULandscapeGrassType): the varieties a landscape layer grows. Any number of
     * them; each is its own generated instance buffer and its own indirect draws.
     */
    struct LandscapeGrassTypeData
    {
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;

        std::vector<GrassVariety> GrassVarieties;

        [[nodiscard]] bool operator==( const LandscapeGrassTypeData& ) const = default;
    };

    /// Rejects numbers the generator cannot honour, naming the variety, the field and the values.
    Common::BoolResultStr ValidateLandscapeGrassTypeData( const LandscapeGrassTypeData& data );

    /// Parses a `.degrasstype`. A file without a header, of another version or kind, whose Dependencies do not
    /// state exactly its meshes' GUIDs, or with invalid numbers is an error naming why.
    Common::ResultStr<LandscapeGrassTypeData> ParseLandscapeGrassType( const std::string& text );

    /// Canonical text; stamps the header (keeping a loaded GUID, minting one otherwise).
    std::string WriteLandscapeGrassType( const LandscapeGrassTypeData& data );

    Common::BoolResultStr SaveLandscapeGrassTypeFile( const std::filesystem::path&  path,
                                                      const LandscapeGrassTypeData& data );
} // namespace Desert::Assets::Serialization
