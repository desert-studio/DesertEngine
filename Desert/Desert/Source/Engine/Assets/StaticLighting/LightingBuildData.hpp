#pragma once

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/UUID.hpp>
#include <Engine/Assets/AssetBase.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

// LIGHTMASS CONTRACT (owner backlog LIGHTMASS, 10-21) — THE BAKED DATA. Header contract only; no definitions yet.
//
// One asset per level: the LIGHTING BUILD (`.delightbuild`, AssetTypeID::LightingBuildData appended with the
// implementation), UE's UMapBuildDataRegistry (MapName_BuiltData.uasset). The scene names it by GUID; the bake
// writes it, nothing else does. Everything the renderer reads of the bake is in here, keyed by the entity's UUID
// — never stored back into the scene's components (one source of truth: the scene states what to bake, the build
// states what was baked).
//
// UE counterparts: FMeshMapBuildData (per-primitive lightmap + shadowmap), FLightComponentMapBuildData (per light
// shadow channel), FPrecomputedVolumetricLightmapData (the volume), FLightMap2D / FShadowMap2D (atlas placement).
//
// WHERE THIS ENGINE DIFFERS:
//  - Stale detection is a HASH, not a GUID compare: each record carries the hash of every input that shaped it,
//    computed by Assets::GatherStaticLightingInputs (StaticLightingInputs.hpp) — the one function the bake, the
//    overlay and the cook's gate (Q8: the cook packages this asset and refuses a stale one by name) share. A
//    record whose hash disagrees with the live scene is NOT drawn as if valid — the primitive is listed in the
//    viewport overlay "Lighting needs to be rebuilt (N objects)" and drawn with the stale data, exactly UE's
//    behaviour, but the count is exact (UE's is per-level-GUID).
//  - The atlas textures are cooked through the DDC like every texture (BC6H for the HDR coefficient, BC7 for
//    the directional and the shadow textures), never kept as raw floats in the shipping package.
namespace Desert::Assets
{
    // THE LIGHTMAP ENCODING (UE "high quality" lightmaps, FLightMapCoefficients / LightmapCommon.ush): L1
    // spherical harmonics of the incident irradiance, split into
    //   Coefficient0: RGB of the SH DC term (HDR, BC6H)
    //   Coefficient1: the L1 directionality, luminance-normalised (BC7), so a normal map shades a lightmap.
    // Plus SkyOcclusion (bent normal xyz + visible fraction; UE: bUseSkyOcclusion / the sky's "Stationary"
    // shadowing) and AO material mask, both in one BC7 texture.
    enum class LightmapTexture : uint8_t
    {
        Coefficient0 = 0,
        Coefficient1,
        SkyOcclusion,
        Count,
    };

    // A region of an atlas page: the primitive's lightmap UV (Assets::MeshLightmapUV channel) maps into it as
    // uv * Scale + Bias (UE FLightMap2D::CoordinateScale / CoordinateBias).
    struct AtlasPlacement
    {
        uint16_t  Page = 0;
        glm::vec2 Scale{ 0.0f };
        glm::vec2 Bias{ 0.0f };
    };

    // One lightmapped primitive LOD's bake (UE FMeshMapBuildData). For a non-foliage instanced static mesh, one
    // record per instance, in the component's instance order (UE: FMeshMapBuildData::PerInstanceLightmapData);
    // for a landscape, one per LandscapeTileComponent entity. Q6: one record PER LOD, each with its own region.
    // Q7: foliage fields and grass have no record — they read the Volume.
    struct BakedPrimitiveRecord
    {
        Common::UUID   Entity;
        uint32_t       InstanceIndex = 0; // 0 for a static mesh or a landscape tile
        uint32_t       Lod           = 0;
        uint64_t       InputHash     = 0;
        AtlasPlacement Lightmap;
        // The distance-field shadow map of the stationary lights (UE FShadowMap2D): channel i of the page is the
        // light whose BakedLightRecord::ShadowMapChannel is i. Absent (Page = kNoPage) when no stationary light
        // reaches the primitive.
        AtlasPlacement ShadowMap;
        // The baked-lighting multipliers UE applies per lightmap (FLightMap2D::ScaleVectors/AddVectors): the
        // coefficient textures are stored normalised to [0,1] by these.
        std::array<glm::vec4, 2> CoefficientScale{};
        std::array<glm::vec4, 2> CoefficientAdd{};
    };
    inline constexpr uint16_t kNoPage = 0xFFFFu;

    // One light's bake (UE FLightComponentMapBuildData).
    struct BakedLightRecord
    {
        Common::UUID Entity;
        uint64_t     InputHash = 0;
        // 0..3: the stationary light's channel in the shadow maps and the volume's directional shadow; -1: Static
        // (fully baked, nothing at runtime) or a stationary light past the 4-overlap limit, which the renderer
        // then draws as Movable and the overlay lists (UE: the red X over the light).
        int8_t ShadowMapChannel = -1;
    };

    // THE VOLUMETRIC LIGHTMAP (UE FPrecomputedVolumetricLightmapData, r.VolumetricLightmap): the indirect light at
    // points in space, for whatever is not lightmapped (Movable primitives, foliage fields and landscape grass —
    // Q7 —, skinned meshes, particles, hair, fog, translucency). A sparse brick tree: an indirection texture over
    // the level's lighting bounds points into a brick atlas of 4^3-texel bricks (+1 border texel for hardware
    // filtering), finer where geometry is dense (refined down to DetailCellSize near Static surfaces, coarse in
    // empty air).
    struct VolumetricLightmapData
    {
        glm::vec3  BoundsMin{ 0.0f }; // cm, world
        glm::vec3  BoundsSize{ 0.0f };
        glm::uvec3 IndirectionSize{ 0u };
        glm::uvec3 BrickAtlasSize{ 0u }; // in texels
        uint32_t   BrickSize = 4;        // UE's
        // The atlas layers (UE FVolumetricLightmapBasicBrickDataLayers): AmbientVector (SH L0 + L1 luminance),
        // SH L1 colour coefficients x6, SkyBentNormal, DirectionalLightShadowing (the stationary sun's shadow for
        // Movable objects). Stored as cooked 3D textures; the raw layers live only until the cook.
        std::vector<uint8_t>                Indirection;
        std::vector<uint8_t>                AmbientVector;
        std::array<std::vector<uint8_t>, 6> SHCoefficients;
        std::vector<uint8_t>                SkyBentNormal;
        std::vector<uint8_t>                DirectionalLightShadowing;
    };

    class LightingBuildDataAsset final : public AssetBase
    {
    public:
        explicit LightingBuildDataAsset( const Common::Filepath& filepath );

        Common::BoolResultStr LoadFromFile() override;
        Common::BoolResultStr Unload() override;
        bool                  IsReadyForUse() const override;

        static AssetTypeID GetTypeID(); // AssetTypeID::LightingBuildData, appended with the implementation

        [[nodiscard]] const Common::Content::AssetGuid& Guid() const;

        // Hash of the bake settings the build was made with (LightmassSettings): a changed setting stales every
        // record at once.
        [[nodiscard]] uint64_t SettingsHash() const;

        // Lookups the renderer makes once per primitive registration, not per frame. Null: never baked.
        [[nodiscard]] const BakedPrimitiveRecord* FindPrimitive( const Common::UUID& entity,
                                                                 uint32_t instanceIndex, uint32_t lod ) const;
        [[nodiscard]] const BakedLightRecord*     FindLight( const Common::UUID& entity ) const;

        // The atlas pages, as cooked texture assets this asset owns (one per LightmapTexture per page, and one
        // RGBA shadow map texture per shadow page).
        [[nodiscard]] uint32_t            LightmapPageCount() const;
        [[nodiscard]] uint32_t            ShadowMapPageCount() const;
        [[nodiscard]] Common::AssetHandle LightmapPage( uint32_t page, LightmapTexture texture ) const;
        [[nodiscard]] Common::AssetHandle ShadowMapPage( uint32_t page ) const;

        // Null when the bake built no volume (LightmassSettings::BuildVolumetricLightmap off).
        [[nodiscard]] const VolumetricLightmapData* Volume() const;
    };
} // namespace Desert::Assets
