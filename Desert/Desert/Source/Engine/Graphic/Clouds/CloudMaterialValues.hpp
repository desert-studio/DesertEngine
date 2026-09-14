#pragma once

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Logger.hpp>

#include <Engine/Core/Formats/ShaderProgramMeta.hpp>
#include <Engine/ECS/VolumetricCloudComponent.hpp>
#include <Engine/Graphic/Materials/MaterialOverrides.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <string_view>

namespace Desert::Graphic
{
    /// The Volume-domain shader whose Properties block IS the cloud material schema. One spelling for its
    /// three readers — the renderer's pipelines, the renderer's schema lookup and the Cloud Layout
    /// panel's — because a name that three files spell for themselves is a name that can fork.
    inline constexpr const char* kCloudMaterialShaderName = "CloudRaymarch";

    /// The include the four cloud-field consumers resolve, and the one name a material's authored medium
    /// substitutes at compile time (Engine/Core/ShaderCompiler/ShaderVariant.hpp).
    ///
    /// ONE SPELLING FOR FIVE READERS — the cloud renderer's variant, the sky bake's variant, the shader
    /// graph's Volume emitter, the census suite and Common/CloudField.glslh's own include line. It is a
    /// string looked up by NAME at compile time, so two spellings do not fail to link or fail to compile:
    /// the substitution simply never happens, the default file is read instead, and the authored medium
    /// silently does nothing.
    inline constexpr const char* kCloudMediumInclude = "Generated/CloudMedium.glslh";

    /// The cloud material property that NAMES the authored medium — a `ShaderAsset` reference in
    /// CloudRaymarch's Properties block. One spelling for its three readers: the override reader below,
    /// the material window (which merges the named medium's own schema into its rows) and the shader file
    /// itself. It is NOT a prefix of a medium's own keys, which carry Core::kCloudMediumOverridePrefix —
    /// "Medium" and "Medium." are different keys and that is deliberate.
    inline constexpr const char* kCloudMediumSlotName = "Medium";

    /**
     * @brief The cloud LOOK, resolved from the layer's material — the thirty-three values O1 moved out of
     *        ECS::VolumetricCloudData.
     *
     * WHO FILLS IT AND FROM WHAT. VolumetricCloudRenderer::SetCloudSettings calls
     * BuildCloudMaterialValues once per frame: the CloudRaymarch shader's OWN schema supplies every
     * default, and the flattened `.demat` chain (Runtime::MaterialService::ResolveOverrides) overwrites
     * by name, last write winning. A null material handle therefore means exactly the schema defaults —
     * which the CloudMaterialSchema suite pins byte-for-byte against the member initializers below, so
     * this struct is a test-enforced MIRROR of the schema and never a second source of truth.
     *
     * WHY A TYPED STRUCT AND NOT name->vec4 LOOKUPS AT EVERY READ SITE. The packer and the bake read
     * these values dozens of times per frame between them; a typed field is one load, and — the real
     * reason — a typo in a name would compile and silently read zero, which is the "middle link drops a
     * property" defect this project has paid for seven times in one day. Field names equal schema names
     * equal old component names, and the census test asserts the correspondence in both directions.
     *
     * UNITS ARE THE COMPONENT'S OLD UNITS UNCHANGED: world units (centimetres) for every length,
     * per-kilometre for extinction — the migration copies scene numbers verbatim, and
     * Graphic::PackCloudParams keeps doing the one cm->km conversion it always did.
     */
    struct CloudMaterialValues
    {
        // ---- Cloud Types (species slots; empty slot = skipped, all empty = built-in congestus) -------
        Assets::AssetHandle CloudType1;
        Assets::AssetHandle CloudType2;
        Assets::AssetHandle CloudType3;
        Assets::AssetHandle CloudType4;

        // ---- Weather --------------------------------------------------------------------------------
        float   Coverage         = 0.45f;
        float   CoverageContrast = 1.0f;
        float   WeatherTileSize  = 1200000.0f; // cm; 12 km -> 3 km lattice cells
        int32_t Seed             = 1;

        // ---- Placement (bake-time) ------------------------------------------------------------------
        float PlacementDensity     = 1.75f;
        float PlacementScatter     = 1.0f;
        float PlacementSizeVariety = 0.75f;
        float PatchTileSize        = 2100000.0f; // cm; 21 km
        float PatchStrength        = 0.60f;

        // ---- Layout (bake-time; the painted sky) ----------------------------------------------------
        //
        // TWO INPUTS AND NOT ONE, which is decision O-4 and Unreal's own arrangement: its cloud material
        // takes `Layout_CloudGlobalPattern` and `Layout_GlobalCloudMask` as separate texture parameters, so
        // the placement and the add/remove regions are authored, swapped and reused independently. Both
        // normally name one `.dclayout` — the container holds both tables — and either may be empty.
        Assets::AssetHandle LayoutPattern;
        Assets::AssetHandle LayoutMask;
        float               LayoutPatternStrength = 1.0f;
        float               LayoutMaskStrength    = 1.0f;
        int32_t             LayoutRepeats         = 1;
        int32_t             LayoutRotation        = 0;
        glm::vec2           LayoutOffset          = { 0.0f, 0.0f }; // cm

        // ---- Detail (march-time) --------------------------------------------------------------------
        float DetailTileSize  = 100000.0f; // cm; 1 km
        float DetailStrength  = 0.65f;
        float DensityScale    = 1.0f;
        float ExtinctionScale = 8.0f; // per km

        // ---- Lighting -------------------------------------------------------------------------------
        // PER COLOUR since the Volume domain's output contract was implemented (O1_DESIGN §3.3, §9 п.2).
        // Grey is water and is the default; a tint makes the MEDIUM something else, and it compounds
        // through the scattering series rather than sitting on the frame as a filter.
        glm::vec3 ScatteringAlbedo         = { 0.98f, 0.98f, 0.98f };
        float     PhaseG                   = 0.8f;
        float     PhaseGBackward           = 0.1667f;
        float     PhaseBlend               = 0.575f;
        float     AmbientOcclusionStrength = 1.0f;
        int32_t   MultiScatterOctaves      = 3;
        float     MultiScatterContribution = 0.667f;
        float     MultiScatterOcclusion    = 0.25f;
        float     MultiScatterEccentricity = 0.18f;
        glm::vec3 AmbientScale             = { 1.0f, 1.0f, 1.0f };

        // ---- The authored medium ---------------------------------------------------------------------
        //
        // A Volume-domain shader carrying a `Medium { ... }` block — what a cloud IS at a point in space,
        // authored in the node graph. NULL IS THE NORMAL STATE and means the shipped chain in
        // Generated/CloudMedium.glslh, which is byte-for-byte what every scene drew before the slot
        // existed. It is not a number in the parameter block and never becomes one: it is a body of CODE,
        // substituted into the four programs that sample the field at COMPILE time
        // (Engine/Core/ShaderCompiler/ShaderVariant.hpp), which is why it costs nothing per sample.
        Assets::AssetHandle Medium;

        /// The species slots in their one canonical order (ECS::kCloudTypeSlots of them).
        void TypeSlots( Assets::AssetHandle ( &out )[ECS::kCloudTypeSlots] ) const
        {
            out[0] = CloudType1;
            out[1] = CloudType2;
            out[2] = CloudType3;
            out[3] = CloudType4;
        }
    };

    namespace Detail
    {
        // ── A NUMBER THAT CANNOT BE READ LEAVES THE FIELD ALONE ─────────────────────────────────────────
        //
        // ONE STATEMENT FOR ALL TWENTY-NINE VALUE PARAMETERS, and the arity comes from the FIELD'S TYPE so
        // there is no second table of component counts to drift from the struct. Graphic::MaterialValueIsReadable
        // carries the argument for why the check is here rather than at each clamp downstream; the short
        // version is that `std::clamp` propagates a NaN and every reader of these values clamps.
        //
        // LEAVING THE FIELD ALONE IS THE DECISION. The field already holds the schema default at this
        // point (BuildCloudMaterialValues applies the schema first and the `.demat` chain over it), so a
        // refused override means "this parameter as the shader authored it", named in the log — not a
        // zero, and not a NaN the GPU has to make sense of.
        inline void AssignCloudValue( float& field, std::string_view name, const glm::vec4& p )
        {
            if ( MaterialValueIsReadable( name, p, MaterialValueLanes::One ) )
                field = p.x;
        }

        inline void AssignCloudValue( int32_t& field, std::string_view name, const glm::vec4& p )
        {
            if ( MaterialValueIsReadableAsInt( name, p.x ) )
                field = static_cast<int32_t>( p.x );
        }

        inline void AssignCloudValue( glm::vec2& field, std::string_view name, const glm::vec4& p )
        {
            if ( MaterialValueIsReadable( name, p, MaterialValueLanes::Two ) )
                field = { p.x, p.y };
        }

        inline void AssignCloudValue( glm::vec3& field, std::string_view name, const glm::vec4& p )
        {
            if ( MaterialValueIsReadable( name, p, MaterialValueLanes::Three ) )
                field = { p.x, p.y, p.z };
        }

        // One override application. Values by name from the params list, handles by name from the
        // textures list (asset references share that map — it is name -> uint64, nothing texture-specific).
        inline void ApplyCloudOverride( CloudMaterialValues& v, std::string_view name, const glm::vec4& p )
        {
            if ( name == "Coverage" )
                AssignCloudValue( v.Coverage, name, p );
            else if ( name == "CoverageContrast" )
                AssignCloudValue( v.CoverageContrast, name, p );
            else if ( name == "WeatherTileSize" )
                AssignCloudValue( v.WeatherTileSize, name, p );
            else if ( name == "Seed" )
                AssignCloudValue( v.Seed, name, p );
            else if ( name == "PlacementDensity" )
                AssignCloudValue( v.PlacementDensity, name, p );
            else if ( name == "PlacementScatter" )
                AssignCloudValue( v.PlacementScatter, name, p );
            else if ( name == "PlacementSizeVariety" )
                AssignCloudValue( v.PlacementSizeVariety, name, p );
            else if ( name == "PatchTileSize" )
                AssignCloudValue( v.PatchTileSize, name, p );
            else if ( name == "PatchStrength" )
                AssignCloudValue( v.PatchStrength, name, p );
            else if ( name == "LayoutPatternStrength" )
                AssignCloudValue( v.LayoutPatternStrength, name, p );
            else if ( name == "LayoutMaskStrength" )
                AssignCloudValue( v.LayoutMaskStrength, name, p );
            else if ( name == "LayoutRepeats" )
                AssignCloudValue( v.LayoutRepeats, name, p );
            else if ( name == "LayoutRotation" )
                AssignCloudValue( v.LayoutRotation, name, p );
            else if ( name == "LayoutOffset" )
                AssignCloudValue( v.LayoutOffset, name, p );
            else if ( name == "DetailTileSize" )
                AssignCloudValue( v.DetailTileSize, name, p );
            else if ( name == "DetailStrength" )
                AssignCloudValue( v.DetailStrength, name, p );
            else if ( name == "DensityScale" )
                AssignCloudValue( v.DensityScale, name, p );
            else if ( name == "ExtinctionScale" )
                AssignCloudValue( v.ExtinctionScale, name, p );
            // THREE COMPONENTS, AND A `.demat` WRITTEN BEFORE THE CHANGE CARRIES ONE. `[0.98, 0, 0, 0]` read
            // as a colour is a RED cloud, which is why Migration::MigrateCloudMaterialAlbedoToColour exists
            // and why it is content-detected: this reader deliberately does NOT paper over the old shape by
            // broadcasting p.x when p.y and p.z are zero. Doing so would make (0.98, 0, 0) — a legal
            // authored colour once the slot is three-component — unexpressible, and it would hide an
            // unmigrated file for ever instead of letting the migrator find it once.
            else if ( name == "ScatteringAlbedo" )
                AssignCloudValue( v.ScatteringAlbedo, name, p );
            else if ( name == "PhaseG" )
                AssignCloudValue( v.PhaseG, name, p );
            else if ( name == "PhaseGBackward" )
                AssignCloudValue( v.PhaseGBackward, name, p );
            else if ( name == "PhaseBlend" )
                AssignCloudValue( v.PhaseBlend, name, p );
            else if ( name == "AmbientOcclusionStrength" )
                AssignCloudValue( v.AmbientOcclusionStrength, name, p );
            else if ( name == "MultiScatterOctaves" )
                AssignCloudValue( v.MultiScatterOctaves, name, p );
            else if ( name == "MultiScatterContribution" )
                AssignCloudValue( v.MultiScatterContribution, name, p );
            else if ( name == "MultiScatterOcclusion" )
                AssignCloudValue( v.MultiScatterOcclusion, name, p );
            else if ( name == "MultiScatterEccentricity" )
                AssignCloudValue( v.MultiScatterEccentricity, name, p );
            else if ( name == "AmbientScale" )
                AssignCloudValue( v.AmbientScale, name, p );
            // An unknown name is NOT an error here: a `.demat` may carry params for a shader revision
            // ahead of or behind this binary, and the schema census — not this switch — is what pins the
            // live set. It is skipped, and the material editor shows the value it stored.
        }

        inline void ApplyCloudAssetRef( CloudMaterialValues& v, std::string_view name, uint64_t handle )
        {
            if ( name == "CloudType1" )
                v.CloudType1 = Assets::AssetHandle( handle );
            else if ( name == "CloudType2" )
                v.CloudType2 = Assets::AssetHandle( handle );
            else if ( name == "CloudType3" )
                v.CloudType3 = Assets::AssetHandle( handle );
            else if ( name == "CloudType4" )
                v.CloudType4 = Assets::AssetHandle( handle );
            else if ( name == "LayoutPattern" )
                v.LayoutPattern = Assets::AssetHandle( handle );
            else if ( name == "LayoutMask" )
                v.LayoutMask = Assets::AssetHandle( handle );
            else if ( name == kCloudMediumSlotName )
                v.Medium = Assets::AssetHandle( handle );
        }
    } // namespace Detail

    /**
     * @brief Schema defaults + `.demat` overrides -> the frame's cloud look.
     *
     * @param schema    CloudRaymarch's parsed Properties (null = shader not loaded; the member
     *                  initializers stand in and the caller has already logged the missing shader).
     * @param overrides The flattened material chain from MaterialService::ResolveOverrides; empty for a
     *                  null material handle.
     *
     * The schema pass exists so that the shader file is the ONE runtime source of defaults: if a default
     * is retuned in the Properties block, every scene with an empty slot follows it without this file
     * being touched. The member initializers are the pinned mirror (CloudMaterialSchema asserts equality),
     * kept so a missing shader degrades to the same sky loudly rather than to a zeroed one silently.
     */
    inline CloudMaterialValues BuildCloudMaterialValues( const Core::Formats::ShaderProgramMeta* schema,
                                                         const MaterialOverrides&                overrides )
    {
        CloudMaterialValues values;

        if ( schema )
        {
            for ( const auto& param : schema->Params )
            {
                if ( param.IsAssetRef() )
                    continue; // an asset reference's "default" is null, which the fields already are
                Detail::ApplyCloudOverride( values, param.Name, param.Default );
            }
        }

        for ( const auto& [name, value] : overrides.Params )
            Detail::ApplyCloudOverride( values, name, value );
        for ( const auto& [name, handle] : overrides.Textures )
            Detail::ApplyCloudAssetRef( values, name, handle );

        return values;
    }
} // namespace Desert::Graphic
