#pragma once

// VFX-02. The `.dfx` file: one VFX SYSTEM with its emitters EMBEDDED (analysis 02 §3.2, decision Р-B). Our
// own layout, not a port: UE keeps an emitter twice — a UNiagaraEmitter asset (NiagaraEmitter.h:636) and the
// system's FNiagaraEmitterHandle copy of it — and the two drift; here an emitter exists only inside its
// system, so it has one identity. The module stack is DATA (an ordered list of module uses with their input
// bindings) and the order is the array index, not a wire walk. Behaviour — compiling the stack, simulating,
// drawing — belongs to later cards (VFX-04..08); this file states what an author made and nothing else.

#include <Engine/Animation/KeyInterpolation.hpp>
#include <Engine/Assets/AssetGuidRef.hpp>
#include <Engine/Assets/TextAssetHeaderStamp.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <string>
#include <vector>

namespace Desert::Assets::Serialization
{
    /// The extension the Content Browser and the loader agree on.
    inline constexpr const char* kVFXSystemExtension = ".dfx";

    /**
     * @brief The FILE layout's generation, stated in the text asset header under `VFXS` from the first file.
     *
     *   1 - VFX-02: system (Category, Tags, Duration, Loop, Seed, Bounds, UserParams, LocalModules, Emitters),
     *       emitters with lifecycle, the three-group module stack and renderer rows.
     *   2 - VFX-RENDER: a renderer row names its Material ({Guid, Path}, UE UNiagaraSpriteRendererProperties::
     *       Material); absent = the engine's default sprite template. The header's Dependencies are exactly the
     *       materials and meshes the rows name (VFXSystemDependencies). Every enabled row is its own draw; a Mesh
     *       row carries Mesh {Mesh {Guid, Path}, Facing}, a Ribbon row Ribbon {Facing, UVTiling} (UE
     *       UNiagaraMeshRendererProperties / UNiagaraRibbonRendererProperties). Bounds moved from the system
     *       into every emitter (VFXEmitterData::Bounds, UE UNiagaraEmitter::FixedBounds): each emitter is culled
     *       by its own box. v1 also refused every Mesh / Ribbon row and a second Sprite row: the repository's
     *       files were raised in place (the system's box copied into each emitter); a v1 file is refused with
     *       its number.
     *
     * An unknown value is refused in both directions; a later layout raises its files through a SceneMigrator
     * step, not through defaults in the reader.
     */
    inline constexpr int32_t kVFXSystemVersion = static_cast<int32_t>( Assets::kVFXSystemSchemaVersion );

    [[nodiscard]] inline std::span<const Common::Content::SubsystemVersion> VFXSystemTextSubsystems()
    {
        static const std::array<Common::Content::SubsystemVersion, 1> versions = {
             Common::Content::SubsystemVersion{ Assets::kVFXSystemSchemaTag,
                                                static_cast<uint32_t>( kVFXSystemVersion ) } };
        return versions;
    }

    /// The spellings a stack row uses: a module reference is "engine:<Name>" or "local:<Id>", a binding is
    /// "User.<param>" or "Particles.<attribute>". The reader validates them and the stack compiler (VFX-04)
    /// resolves them, so both read these.
    inline constexpr std::string_view kVFXEnginePrefix    = "engine:";
    inline constexpr std::string_view kVFXLocalPrefix     = "local:";
    inline constexpr std::string_view kVFXUserPrefix      = "User.";
    inline constexpr std::string_view kVFXParticlesPrefix = "Particles.";
    /// "DataChannel.<channel>" names a scene data channel (a `.dfxch` by file stem),
    /// "DataChannel.<channel>.<field>" one of its payload fields (VFX-10, engine:SpawnFromChannel;
    /// VFX/VFXDataChannel.hpp).
    inline constexpr std::string_view kVFXDataChannelPrefix = "DataChannel.";

    /// The type of a parameter or a module input. Components used: Float 1, Vec2 2, Vec3 3, Vec4 4, Int 1,
    /// Bool 1; the unused components of a stored vec4 must be zero, so one value has one spelling.
    enum class VFXValueType
    {
        Float,
        Vec2,
        Vec3,
        Vec4,
        Int,
        Bool,
    };

    [[nodiscard]] uint32_t ComponentCount( VFXValueType type );

    /// Where a module input takes its value from (UE: the stack input's mode — local value, curve, linked
    /// parameter, random range). Exactly the member of VFXModuleInput the source names is present.
    enum class VFXInputSource
    {
        Value,
        Curve,
        Binding,
        Random,
    };

    /// One key of an input curve. The time axis is the input's own (normalised particle age for a
    /// per-particle input, emitter time for a per-emitter one), so it is a float rather than the Timeline's
    /// FrameNumber; the interpolation and tangent rules are the animation curve's (KeyInterpolation.hpp),
    /// so one evaluator serves both (VFX-05 builds the LUT from them).
    struct VFXCurveKey
    {
        float                  Time          = 0.0f;
        float                  Value         = 0.0f;
        float                  ArriveTangent = 0.0f;
        float                  LeaveTangent  = 0.0f;
        Animation::KeyInterp   Interp        = Animation::KeyInterp::Linear;
        Animation::TangentMode Tangents      = Animation::TangentMode::Auto;

        [[nodiscard]] bool operator==( const VFXCurveKey& ) const = default;
    };

    /// A uniform random range, per component, Min <= Max.
    struct VFXRandomRange
    {
        glm::vec4 Min = glm::vec4( 0.0f );
        glm::vec4 Max = glm::vec4( 0.0f );

        [[nodiscard]] bool operator==( const VFXRandomRange& ) const = default;
    };

    struct VFXModuleInput
    {
        std::string    Name;
        VFXValueType   Type   = VFXValueType::Float;
        VFXInputSource Source = VFXInputSource::Value;
        /// Source == Value.
        std::optional<glm::vec4> Value{};
        /// Source == Curve: one channel per component of Type, each sorted by Time.
        std::optional<std::vector<std::vector<VFXCurveKey>>> Curve{};
        /// Source == Binding: "User.<param>" (a UserParams row of the same Type) or "Particles.<attribute>".
        std::optional<std::string> Binding{};
        /// Source == Random.
        std::optional<VFXRandomRange> Random{};

        [[nodiscard]] bool operator==( const VFXModuleInput& ) const = default;
    };

    /// One module in a stack group. Module is "engine:<Name>" (the engine module library) or "local:<Id>"
    /// (a LocalModules row of this system — UE's Scratch Pad, a module that lives inside the system).
    struct VFXModuleUse
    {
        std::string                 Module;
        bool                        Enabled = true;
        std::vector<VFXModuleInput> Inputs;

        [[nodiscard]] bool operator==( const VFXModuleUse& ) const = default;
    };

    /// The stack, in execution order inside each group (the array index IS the order).
    struct VFXEmitterStack
    {
        std::vector<VFXModuleUse> EmitterUpdate;
        std::vector<VFXModuleUse> ParticleSpawn;
        std::vector<VFXModuleUse> ParticleUpdate;

        [[nodiscard]] bool operator==( const VFXEmitterStack& ) const = default;
    };

    /// UE ENiagaraLoopBehavior.
    enum class VFXLoopBehavior
    {
        Once,
        Multiple,
        Infinite,
    };

    /// UE's emitter state: when the emitter starts, how long one loop lasts and how many loops it runs.
    struct VFXEmitterLifecycle
    {
        float           Delay        = 0.0f; ///< seconds after the system starts
        float           LoopDuration = 1.0f; ///< seconds, > 0
        VFXLoopBehavior Loop         = VFXLoopBehavior::Infinite;
        uint32_t        LoopCount    = 1; ///< read when Loop == Multiple, >= 1

        [[nodiscard]] bool operator==( const VFXEmitterLifecycle& ) const = default;
    };

    enum class VFXSimulationSpace
    {
        Local,
        World,
    };

    enum class VFXRendererKind
    {
        Sprite,
        Ribbon,
        Mesh,
    };

    /// UE ENiagaraMeshFacingMode: how a mesh particle's local axes are turned.
    enum class VFXMeshFacing
    {
        Default,        ///< the particle's MeshOrientation quaternion (identity when the stack declares none)
        Velocity,       ///< local +X along the particle's Velocity
        CameraPosition, ///< local +X toward the camera position
        CameraPlane,    ///< local +X against the camera's forward axis
    };

    /// UE UNiagaraMeshRendererProperties: every particle draws one instance of a static mesh.
    struct VFXMeshRendererData
    {
        /// The `.dmesh` static mesh, by its header GUID (`Path`, relative to the content root, only locates it).
        AssetGuidRef  Mesh;
        VFXMeshFacing Facing = VFXMeshFacing::Default;

        [[nodiscard]] bool operator==( const VFXMeshRendererData& ) const = default;
    };

    /// UE ENiagaraRibbonFacingMode.
    enum class VFXRibbonFacing
    {
        Screen, ///< the width runs across the view: cross( tangent, toward the eye )
        Custom, ///< the width runs across the particle's RibbonFacing vector: cross( tangent, RibbonFacing )
    };

    /// UE UNiagaraRibbonRendererProperties: the emitter's particles, ordered by RibbonID then by age (newest
    /// first, UE's NormalizedAge order), are joined into strips; consecutive particles of one RibbonID form a
    /// segment. A stack without RibbonID draws one ribbon (Niagara's default).
    struct VFXRibbonRendererData
    {
        VFXRibbonFacing Facing   = VFXRibbonFacing::Screen;
        float           UVTiling = 1.0f; ///< U repeats this many times over the ribbon's normalised age; > 0

        [[nodiscard]] bool operator==( const VFXRibbonRendererData& ) const = default;
    };

    /// One renderer of an emitter (UE UNiagaraRendererProperties): what draws the emitter's particles and with
    /// which material. The particles are the emitter's; a renderer only reads them. Every enabled row is its own
    /// draw (UE FNiagaraRenderer per renderer properties), so an emitter has any number of rows of each kind.
    struct VFXRendererData
    {
        VFXRendererKind Kind    = VFXRendererKind::Sprite;
        bool            Enabled = true;
        /// The `.demat` the particles draw with, by its header GUID (the reference; `Path`, relative to the
        /// content root, is only a locator). Absent = the engine's default template of the row's kind
        /// (ParticleSpriteDefault / ParticleMeshDefault / ParticleRibbonDefault). The material's template must
        /// declare the kind's Usage (ParticleSprites / ParticleMeshes / ParticleRibbons); one that does not is
        /// refused by the renderer, by name, and the row draws with the kind's default template.
        std::optional<AssetGuidRef> Material{};
        /// Present exactly when Kind == Mesh.
        std::optional<VFXMeshRendererData> Mesh{};
        /// Present exactly when Kind == Ribbon.
        std::optional<VFXRibbonRendererData> Ribbon{};

        [[nodiscard]] bool operator==( const VFXRendererData& ) const = default;
    };

    /**
     * @brief An emitter's FIXED bounds, centimetres, in the frame the placing entity puts the system in (UE
     * UNiagaraEmitter::FixedBounds under ENiagaraEmitterCalculateBoundMode::Fixed).
     *
     * Every emitter of this engine simulates on the GPU, and nothing of a GPU emitter's particles comes back to
     * the CPU: the box is what the renderer knows of where they are. It culls the emitter's draws against the
     * view, the shadow views and the RSM, and decides which point / spot lights it casts into (ParticleRenderer).
     * The placement is the entity's world position (the simulation reads no rotation or scale:
     * ParticleSimPush::EmitterPos), so the box is drawn translated by it, whether the emitter simulates in Local
     * or World space (UE transforms the fixed bounds by the component for both). A particle outside its box is
     * culled with it: the box must hold them all.
     */
    struct VFXBounds
    {
        glm::vec3 Min = glm::vec3( -100.0f );
        glm::vec3 Max = glm::vec3( 100.0f );

        [[nodiscard]] bool operator==( const VFXBounds& ) const = default;
    };

    struct VFXEmitterData
    {
        std::string                  Name;
        bool                         Enabled  = true;
        VFXSimulationSpace           Space    = VFXSimulationSpace::World;
        uint32_t                     Capacity = 1024; ///< the emitter's slice of the GPU pool, particles
        VFXBounds                    Bounds;          ///< fixed, around the placement (VFXBounds)
        VFXEmitterLifecycle          Lifecycle;
        VFXEmitterStack              Stack;
        std::vector<VFXRendererData> Renderers;

        [[nodiscard]] bool operator==( const VFXEmitterData& ) const = default;
    };

    /// A `User.*` parameter: exposed to the placing component and to a Sequencer float track.
    struct VFXUserParam
    {
        std::string  Name; ///< without the "User." prefix
        VFXValueType Type    = VFXValueType::Float;
        glm::vec4    Default = glm::vec4( 0.0f );

        [[nodiscard]] bool operator==( const VFXUserParam& ) const = default;
    };

    /// A module that lives inside the system (UE Scratch Pad): its DSL fragment in the Particle domain.
    struct VFXLocalModule
    {
        std::string Id;
        std::string DisplayName;
        std::string Source;

        [[nodiscard]] bool operator==( const VFXLocalModule& ) const = default;
    };

    /// One row of the PROJECT's category register (`<project>/Config/VFXCategories.json`). Categories are
    /// project data, edited without a rebuild; the engine knows only "a category is an id in that register".
    struct VFXCategory
    {
        std::string Id;
        std::string DisplayName;
        glm::vec4   Color = glm::vec4( 1.0f ); ///< the browser swatch, linear RGBA

        [[nodiscard]] bool operator==( const VFXCategory& ) const = default;
    };

    struct VFXCategoryRegister
    {
        std::vector<VFXCategory> Categories;

        [[nodiscard]] bool Contains( std::string_view id ) const;
    };

    /// The register's file name inside the project's Config directory.
    inline constexpr const char* kVFXCategoriesFileName = "VFXCategories.json";

    /// Reads a category register. A missing file, an empty or repeated id is an error naming the path.
    Common::ResultStr<VFXCategoryRegister> ReadVFXCategories( const std::filesystem::path& path );

    /**
     * @brief One `.dfx` (UE: UNiagaraSystem with its emitters embedded).
     *
     * Category (an id of the project's VFXCategoryRegister) and Tags are browser metadata — what the picker
     * groups and filters by — and carry no behaviour.
     */
    struct VFXSystemData
    {
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;

        std::string                 Category;
        std::vector<std::string>    Tags;
        float                       Duration = 1.0f; ///< seconds of one system loop, >= 0; > 0 when Loop
        bool                        Loop     = true;
        uint32_t                    Seed     = 0;
        std::vector<VFXUserParam>   UserParams;
        std::vector<VFXLocalModule> LocalModules;
        std::vector<VFXEmitterData> Emitters;

        [[nodiscard]] bool operator==( const VFXSystemData& ) const = default;
    };

    /// The GUIDs (AssetGuidToText) of every material and mesh the renderer rows name, sorted and unique: what the
    /// header's Dependencies state, so a registry walk sees the edges without reading the body.
    [[nodiscard]] std::vector<std::string> VFXSystemDependencies( const VFXSystemData& data );

    /// The runtime handle of a renderer's material (Common::Content::HandleForGuid of its GUID); Null when the row
    /// names none. The GUID was validated on load (ValidateVFXSystemData).
    [[nodiscard]] Common::AssetHandle VFXRendererMaterialHandle( const VFXRendererData& renderer );

    /// The runtime handle of a Mesh row's static mesh (Common::Content::HandleForGuid); Null for another kind.
    [[nodiscard]] Common::AssetHandle VFXRendererMeshHandle( const VFXRendererData& renderer );

    /// Rejects what no later stage could honour, naming the emitter, group, module and input.
    Common::BoolResultStr ValidateVFXSystemData( const VFXSystemData& data );

    /// Parses a `.dfx`. A file without a header, of another version or kind, whose Dependencies are not exactly
    /// VFXSystemDependencies, or that fails validation is an error naming why.
    /// The Category must be an id of @p categories (no fallback: an unknown id is an error naming it).
    Common::ResultStr<VFXSystemData> ParseVFXSystem( const std::string&         text,
                                                     const VFXCategoryRegister& categories );

    /// Canonical text; stamps the header (keeping a loaded GUID, minting one otherwise).
    std::string WriteVFXSystem( const VFXSystemData& data );

    Common::BoolResultStr SaveVFXSystemFile( const std::filesystem::path& path, const VFXSystemData& data );
} // namespace Desert::Assets::Serialization

namespace rfl::config
{
    template <typename T>
    struct enum_range;

    /// The curve key's interpolation and tangent rule are persisted BY NAME. Their underlying type is
    /// uint8_t and reflect-cpp's default scan range is `int`, so the range is given in their own type (the
    /// LoopMode precedent, Timeline/Player.hpp).
    template <>
    struct enum_range<Desert::Animation::KeyInterp>
    {
        static constexpr uint8_t min = 0;
        static constexpr uint8_t max = 2;
    };
    template <>
    struct enum_range<Desert::Animation::TangentMode>
    {
        static constexpr uint8_t min = 0;
        static constexpr uint8_t max = 2;
    };
} // namespace rfl::config
