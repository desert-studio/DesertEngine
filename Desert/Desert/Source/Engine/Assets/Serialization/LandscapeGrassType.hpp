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

    /**
     * @brief The shape of a procedural blade (Kind Blade). Per-instance size comes from the variety's scale ranges
     * (ScaleX the width, ScaleZ the height, as for a mesh), the facing from RandomRotation and the clump.
     *
     * Colours are linear RGB in 0..1.
     */
    struct GrassBlade
    {
        /// Blade height, cm, at scale 1.
        float Height = 45.0f;
        /// Width at the root, cm, at scale 1.
        float Width = 1.2f;
        /// The share of the root width lost by the tip: 0 = a ribbon, 1 = a point.
        float Taper = 0.9f;
        /// How far the tip leans forward at rest, as a share of the height (0 = upright).
        float Curvature = 0.35f;
        /// 0..1: the share of the wind's sway the blade resists (1 = it does not move).
        float                Stiffness = 0.3f;
        std::array<float, 3> BaseColor{ 0.06f, 0.10f, 0.03f };
        std::array<float, 3> TipColor{ 0.25f, 0.33f, 0.09f };
        /// The colour a dry tip turns to.
        std::array<float, 3> DryTipColor{ 0.45f, 0.38f, 0.20f };
        /// 0..1: the share of blades with a dry tip.
        float DryTipShare = 0.25f;
        /// 0..1: how much of a dry blade, from the tip down, is dry.
        float DryTipLength = 0.3f;
        /// 0..1: how strongly blades follow their clump - its facing, its height and its tint (0 = independent).
        float Clumping = 0.5f;
        /// The clump cell's side, cm.
        float ClumpSize = 35.0f;
        /// 0..1: how far a blade moves from its grid place towards its clump's point (0 = stays on the jittered
        /// grid, 1 = all of a clump's blades stand on its point). Gathers the field into tufts with open ground between.
        float ClumpPull = 0.0f;
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
