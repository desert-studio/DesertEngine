#pragma once

// Common::Json + DESERT_JSON_STRUCT + Common::ResultStr, as the collision profile register uses them.
#include <PhysicsCore/CollisionProfiles.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Physics
{
    /// UE EFrictionCombineMode / ERestitutionCombineMode (PhysicsCore/Public/PhysicsSettingsEnums.h): how the values
    /// of the two materials of a contact become the contact's. Ordered as UE's enum, because a pair combines with
    /// the HIGHER of its two modes (Chaos FChaosPhysicsMaterial::ChooseCombineMode): Max beats Multiply beats Min
    /// beats Average.
    enum class MaterialCombine : uint8_t
    {
        Average,
        Min,
        Multiply,
        Max,
    };

    /// A surface type: an index into the project's register (PhysicalSurfaces). 0 is "Default", UE's
    /// SurfaceType_Default; a project names up to kMaxSurfaceTypes more (UE SurfaceType1..SurfaceType62).
    using SurfaceTypeId                                 = uint8_t;
    inline constexpr SurfaceTypeId kDefaultSurface      = 0u;
    inline constexpr std::size_t   kMaxSurfaceTypes     = 62u;
    inline constexpr const char*   kDefaultSurfaceName = "Default";

    /**
     * @brief UE UPhysicalMaterial (Engine/Classes/PhysicalMaterials/PhysicalMaterial.h) as the solver reads it: how
     * a surface slides, bounces and weighs, and what it IS (its surface type) for whoever asks what was hit.
     *
     * The member defaults are UE's UPhysicalMaterial constructor values; the engine always fills one from a
     * `.dphysmat` asset. Not ported: StaticFriction (Jolt solves one friction coefficient for sticking and
     * sliding alike) and RaiseMassToPower (a UE-only mass fudge for large bodies).
     */
    struct PhysicalMaterialDesc
    {
        float           Friction           = 0.7f; ///< Coulomb coefficient, >= 0
        float           Restitution        = 0.3f; ///< [0, 1]: 0 = no bounce, 1 = elastic
        MaterialCombine FrictionCombine    = MaterialCombine::Average;
        MaterialCombine RestitutionCombine = MaterialCombine::Average;
        float           DensityGPerCm3     = 1.0f; ///< UE Density, g/cm^3: the mass of a body that states none
        SurfaceTypeId   Surface            = kDefaultSurface;
    };

    /// A physical material registered with a PhysicsWorld (PhysicsWorld::RegisterPhysicalMaterial): what a body
    /// slides with, and what a query or a contact reports it met (UE FHitResult::PhysMaterial). Id 0 is the
    /// world's default material, the one Init was given (UE GEngine->DefaultPhysMaterial).
    using PhysicalMaterialId                                = uint32_t;
    inline constexpr PhysicalMaterialId kDefaultPhysicalMaterial = 0u;

    /// The value of a contact between a surface of (@p a, @p modeA) and one of (@p b, @p modeB), by UE's rule: the
    /// higher mode of the two decides (Chaos FChaosPhysicsMaterial::CombineHelper).
    [[nodiscard]] float CombineMaterialValues( float a, MaterialCombine modeA, float b, MaterialCombine modeB );

    /// Every value inside UE's ClampMin/ClampMax (PhysicalMaterial.h): Friction >= 0, Restitution in [0, 1],
    /// Density > 0 (a zero density would derive a massless body), all finite.
    Common::BoolResultStr ValidatePhysicalMaterial( const PhysicalMaterialDesc& desc );

    /**
     * @brief The project's `Config/PhysicalMaterials.json`: UE's [/Script/Engine.PhysicsSettings] PhysicalSurfaces
     * (Project Settings > Physics > Physical Surface) and [/Script/Engine.Engine] DefaultPhysMaterialName in one
     * register. No file, no simulation, as with the collision profiles: there is no built-in answer to what a
     * body without a material slides on.
     */
    struct PhysicalMaterialsConfig
    {
        /// UE GEngine->DefaultPhysMaterial: the `.dphysmat` every body slides with when neither its own override nor
        /// its mesh's material names one. By header GUID, with the path as the locator (AssetGuidRef's shape).
        std::string DefaultMaterialGuid;
        std::string DefaultMaterialPath;
        /// The project's surface types after "Default" (UE SurfaceType1..SurfaceType62), by name.
        std::vector<std::string> SurfaceTypes;
    };
    DESERT_JSON_STRUCT( PhysicalMaterialsConfig, "PhysicalMaterials", 1 )

    /// The register's file name inside the project's Config directory.
    inline constexpr const char* kPhysicalMaterialsFileName = "PhysicalMaterials.json";

    /// The surface types of a project: "Default" first, then the register's names in their stated order.
    class PhysicalSurfaces
    {
    public:
        /// Refuses more than kMaxSurfaceTypes names, an empty name, and a name stated twice ("Default" included).
        static Common::ResultStr<PhysicalSurfaces> Build( const std::vector<std::string>& names );

        /// The id of @p name, or the refusal naming every type the project has.
        [[nodiscard]] Common::ResultStr<SurfaceTypeId> Resolve( std::string_view name ) const;

        /// The name of @p id, or nullptr for an id the register does not have.
        [[nodiscard]] const std::string* NameOf( SurfaceTypeId id ) const;

        [[nodiscard]] std::size_t Count() const
        {
            return m_Names.size();
        }

    private:
        std::vector<std::string> m_Names; ///< [0] = kDefaultSurfaceName
    };

    /// The register as read: the default material's reference and the surface types.
    struct PhysicalMaterialsSettings
    {
        std::string      DefaultMaterialGuid;
        std::string      DefaultMaterialPath;
        PhysicalSurfaces Surfaces;
    };

    /// Reads `Config/PhysicalMaterials.json` (through the VFS as well as the disk). Refused: a missing file, a
    /// file that names no default material, a surface list Build refuses.
    Common::ResultStr<PhysicalMaterialsSettings> ReadPhysicalMaterialsSettings( const std::filesystem::path& path );
} // namespace Desert::Physics
