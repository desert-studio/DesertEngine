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
#include <Engine/ECS/VFXUserOverride.hpp>
#include <Common/Core/Math/AABB.hpp>
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
     *   2 - VFX-09: a renderer row states its Material, a Ribbon row its Ribbon settings, a Mesh row its Mesh;
     *       the header's Dependencies are those rows' assets.
     *   3 - VFX-11: the system names its EffectType (a `.dfxtype` stem; older files get `Default`).
     *   4 - VFX-PORT-1 (int/vfx VFX-MODULE-ASSET): a stack row names its module as {Guid, Path} of a `.dfxmod`
     *       (the engine's module library or the project's own), the module GUIDs join the header's Dependencies,
     *       and LocalModules is gone (a system-local module is a project `.dfxmod`). Raised by
     *       Tools/SceneMigrator (MigrateVFXSystem): engine:<Name> becomes the GUID of
     *       <Name>.dfxmod in the engine module library.
     *   5 - VFX-PORT-2 (int/vfx de1d65125): Bounds moved from the system into every emitter
     *       (VFXEmitterData::Bounds, UE UNiagaraEmitter::FixedBounds); each emitter is culled by its own box in
     *       every view and every shadow view. Raised by Tools/SceneMigrator (MigrateVFXSystem): the system's
     *       box is copied into each emitter.
     *   6 - PORT-VFX-MODULES (int/vfx 476c85211, there VFXS 4): every emitter states EventHandlers (UE emitter
     *       Event Handlers, Spawned Particles): particles spawned from another emitter's Death / Collision /
     *       Location events. Raised by Tools/SceneMigrator (MigrateVFXSystem): each emitter gets an empty list.
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

    /// The spellings a stack row's binding uses: "User.<param>" or "Particles.<attribute>". The reader validates
    /// them and the stack compiler (VFX-04) resolves them, so both read these.
    inline constexpr std::string_view kVFXUserPrefix      = "User.";
    inline constexpr std::string_view kVFXParticlesPrefix = "Particles.";
    /// "DataChannel.<channel>" names a scene data channel (a `.dfxch` by file stem),
    /// "DataChannel.<channel>.<field>" one of its payload fields (VFX-10, the SpawnFromChannel module;
    /// VFX/VFXDataChannel.hpp).
    inline constexpr std::string_view kVFXDataChannelPrefix = "DataChannel.";
    /// "PointSet.<name>" names a point set of the PLACEMENT (VFXComponentData::PointSets, painted in VFX Paint
    /// mode - VFX-15; UE UNiagaraDataInterfaceArrayPosition on a User parameter the component overrides): a Vec3
    /// input bound to it reads one of the set's points, chosen by weight, per particle (engine:ShapePointSet).
    inline constexpr std::string_view kVFXPointSetPrefix = "PointSet.";
    /// The GUID of the engine module that collides particles with the scene depth
    /// (Editor/Resources/Engine/VFX/Modules/Collision.dfxmod): a Collision event handler's source emitter must run
    /// it. A GUID, not the "engine:Collision" spelling - a stack row names its module by {Guid, Path} since VFXS 4.
    inline constexpr std::string_view kVFXCollisionModuleGuid = "e9f40e7b3dae4463a5a47d725c82a1bb";
    /// The engine module an emitter with EventHandlers spawns its event newborns through
    /// (Editor/Resources/Engine/VFX/Modules/SpawnFromEvent.dfxmod, UE's Receive Event modules): the stack compiler
    /// appends it as the emitter's last ParticleSpawn row and no authored row may name it; the system's header
    /// Dependencies state it (VFXSystemDependencies), since no row does.
    inline constexpr std::string_view kVFXSpawnFromEventModuleGuid = "ad3f8448557840fc952c72444b16e4e6";
    inline constexpr std::string_view kVFXSpawnFromEventModulePath = "engine:Engine/VFX/Modules/SpawnFromEvent.dfxmod";

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

    /// One module in a stack group: the `.dfxmod` it runs, by its header GUID (UE: the module script a
    /// FNiagaraModuleScript node names), with the path it had when the row was written - relative to the content
    /// root the module was found under (the engine mount or the project's assets) - for the reader and messages.
    struct VFXModuleUse
    {
        Assets::AssetGuidRef        Module;
        bool                        Enabled = true;
        std::vector<VFXModuleInput> Inputs;

        [[nodiscard]] bool operator==( const VFXModuleUse& ) const = default;
    };

    /// The stack, in execution order inside each group (the array index IS the order).
    struct VFXEmitterStack
    {
        /// UE's Emitter Spawn group: runs once when the emitter instance starts. A file written before the group
        /// existed states no key and reads as the empty group it was.
        std::vector<VFXModuleUse> EmitterSpawn;
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
        Light, ///< draws nothing: casts a local light from the emitter's particles (VFX-LIGHT)
    };

    /// How a ribbon lays its U coordinate along its length (UE ENiagaraRibbonUVDistributionMode).
    enum class VFXRibbonUVMode
    {
        Stretch,        ///< U runs 0..1 over the whole ribbon (UE ScaledUniformly)
        TileByDistance, ///< U repeats every UVTileDistance centimetres of length (UE TiledOverRibbonLength)
    };

    /// A Ribbon row's settings (UE UNiagaraRibbonRendererProperties): the emitter's particles joined into strips,
    /// one strip per ribbon id, its particles linked in age order (oldest at the tail).
    struct VFXRibbonRendererData
    {
        /// "Particles.<attribute>" naming the integer attribute that splits the particles into ribbons (UE
        /// RibbonIdBinding); empty = the whole emitter is one ribbon.
        std::string     RibbonIdBinding;
        float           Width  = 10.0f; ///< centimetres, > 0 (UE RibbonWidth)
        VFXRibbonUVMode UVMode = VFXRibbonUVMode::Stretch;
        float UVTileDistance   = 100.0f; ///< centimetres per U repeat, > 0; read when UVMode == TileByDistance

        [[nodiscard]] bool operator==( const VFXRibbonRendererData& ) const = default;
    };

    /// A Mesh row's settings (UE UNiagaraMeshRendererProperties): every particle draws this static mesh.
    struct VFXMeshRendererData
    {
        AssetGuidRef Mesh;
        glm::vec3    Scale = glm::vec3( 1.0f ); ///< per-axis, > 0, on top of the particle's own scale

        [[nodiscard]] bool operator==( const VFXMeshRendererData& ) const = default;
    };

    /// How a sprite's quad is turned (UE ENiagaraSpriteAlignment).
    enum class VFXSpriteAlignment
    {
        Unaligned,       ///< faces the camera, turned by the particle's SpriteRotation (UE Unaligned)
        VelocityAligned, ///< its height runs along the particle's Velocity, its width across it, still facing the
                         ///< camera as far as that allows (UE VelocityAligned): sparks, streaks, rain
    };

    /// A Sprite row's settings (UE UNiagaraSpriteRendererProperties). Absent on a Sprite row = UE's defaults
    /// (Unaligned), as an absent Material is the engine's sprite material.
    struct VFXSpriteRendererData
    {
        VFXSpriteAlignment Alignment = VFXSpriteAlignment::Unaligned;

        [[nodiscard]] bool operator==( const VFXSpriteRendererData& ) const = default;
    };

    /// The most lights one Light row casts per emitter (its MaxLights is 1 .. this).
    inline constexpr uint32_t kVFXLightMaxPerEmitter = 64u;

    /// A Light row's settings (UE UNiagaraLightRendererProperties): up to MaxLights of the emitter's alive
    /// particles each cast a point light into the scene's local-light path (the light grid, the deferred
    /// composite, the forward passes) - no light pass of its own. Per light: position = the particle's world
    /// position, radius = the larger SpriteSize axis x RadiusScale (UE RadiusScale over Particles.LightRadius),
    /// colour = the particle's Color rgb (x its alpha when AlphaScalesBrightness, UE bAlphaScalesBrightness) +
    /// ColorAdd (UE ColorAdd), brightness = Intensity. The lit particles are spread evenly over the alive list.
    struct VFXLightRendererData
    {
        float     RadiusScale = 1.0f;              ///< > 0, on the particle's larger SpriteSize axis (cm)
        glm::vec3 ColorAdd    = glm::vec3( 0.0f ); ///< added to every light's colour, >= 0
        /// Each light's brightness: candelas under inverse-square falloff (UE bUseInverseSquaredFalloff, the
        /// default), else the renderer's unitless multiplier with exponent falloff. >= 0.
        float    Intensity                = 20.0f;
        bool     UseInverseSquaredFalloff = true;
        bool     AlphaScalesBrightness    = true;
        uint32_t MaxLights                = 8u; ///< 1 .. kVFXLightMaxPerEmitter

        [[nodiscard]] bool operator==( const VFXLightRendererData& ) const = default;
    };

    /**
     * @brief One renderer of an emitter (UE UNiagaraRendererProperties): the emitter's particles drawn once per
     *        enabled row.
     *
     * Material: Sprite/Ribbon - the material the quads/strips draw with, absent = the engine's particle
     * material (UE's DefaultSpriteMaterial / DefaultRibbonMaterial); Mesh - an override of the mesh's own
     * materials, absent = the mesh's. Ribbon is stated exactly when Kind == Ribbon, Mesh exactly when Kind ==
     * Mesh. Every Material and Mesh GUID is one of the header's Dependencies.
     */
    struct VFXRendererData
    {
        VFXRendererKind                      Kind    = VFXRendererKind::Sprite;
        bool                                 Enabled = true;
        std::optional<AssetGuidRef>          Material{};
        std::optional<VFXSpriteRendererData> Sprite{}; ///< stated only when Kind == Sprite; absent = UE defaults
        std::optional<VFXRibbonRendererData> Ribbon{};
        std::optional<VFXMeshRendererData>   Mesh{};
        std::optional<VFXLightRendererData> Light{}; ///< stated exactly when Kind == Light (which has no Material)

        [[nodiscard]] bool operator==( const VFXRendererData& ) const = default;
    };

    /**
     * @brief An emitter's FIXED bounds, centimetres, around its placement (UE UNiagaraEmitter::FixedBounds under
     * ENiagaraEmitterCalculateBoundMode::Fixed).
     *
     * Every emitter of this engine simulates on the GPU and nothing of its particles comes back to the CPU: the box
     * is all the renderer knows of where they are. It culls the emitter's draws against every view (sprite and
     * ribbon rows, Mesh rows in the geometry passes) and every shadow view (Mesh rows in the cascades), and decides
     * whether the view's emitters sample the shadow maps (ParticleRenderer). The box is stated in the system's
     * PLACEMENT FRAME (the entity's local space) and goes to the world through the entity's FULL transform -
     * translation, rotation and scale (VFXBoundsAt; UE: FixedBounds are component-space and the component's
     * LocalToWorld places them). One rule for both spaces, because the simulation places both through the same
     * transform (ParticleSimPush::LocalToWorld): a Local emitter's particles live in the placement frame and are
     * drawn through it; a World emitter's newborns are placed through it and then stay where they were born, so its
     * box (as in UE) must also hold the trail a moving placement leaves inside it. A particle outside its box is
     * culled with it: the box must hold them all.
     */
    struct VFXBounds
    {
        glm::vec3 Min = glm::vec3( -100.0f );
        glm::vec3 Max = glm::vec3( 100.0f );

        [[nodiscard]] bool operator==( const VFXBounds& ) const = default;
    };

    /// The world box (cm) of @p bounds through the entity's full transform @p localToWorld (VFXBounds: its 8
    /// corners transformed, Arvo). THE rule the renderer's culling, the effects world's scalability box, the
    /// debugger's "Draw bounds" and the editor preview's framing share.
    [[nodiscard]] Common::Math::AABB VFXBoundsAt( const VFXBounds& bounds, const glm::mat4& localToWorld );

    /// The event a source emitter's particles raise (UE: the Generate Death / Collision / Location Event modules).
    /// Death: the step's stack killed the particle. Collision: the Collision module touched the scene this step.
    /// Location: every step the particle lives through (UE Location Event, the classic trail source).
    enum class VFXEventKind
    {
        Death,
        Collision,
        Location,
    };

    /// One Event Handler of an emitter (UE FNiagaraEventScriptProperties, execution mode Spawned Particles): each
    /// event SourceEmitter's particles raised of kind Event in the previous fixed step spawns SpawnCount particles
    /// of THIS emitter at the event's position (its ParticleSpawn group, then engine:SpawnFromEvent, which the stack
    /// compiler appends). InheritVelocity scales the event particle's velocity added to the newborn's (0: none).
    struct VFXEventHandler
    {
        std::string  SourceEmitter;
        VFXEventKind Event           = VFXEventKind::Death;
        uint32_t     SpawnCount      = 1; ///< >= 1
        float        InheritVelocity = 0.0f;

        [[nodiscard]] bool operator==( const VFXEventHandler& ) const = default;
    };

    struct VFXEmitterData
    {
        std::string                  Name;
        bool                         Enabled  = true;
        VFXSimulationSpace           Space    = VFXSimulationSpace::World;
        uint32_t                     Capacity = 1024; ///< the emitter's slice of the GPU pool, particles
        VFXBounds                    Bounds;          ///< fixed, around the placement (VFXBounds), VFXS 5
        VFXEmitterLifecycle          Lifecycle;
        VFXEmitterStack              Stack;
        std::vector<VFXRendererData> Renderers;
        std::vector<VFXEventHandler> EventHandlers; ///< VFXS 6

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


    /// What User.<row.Name> is for a placement with @p overrides: its override when it has one, else the row's
    /// Default. The one rule both the CPU spawn plan and the GPU parameter rows read.
    [[nodiscard]] glm::vec4 UserParamValue( const VFXUserParam&                 row,
                                            const std::vector<ECS::VFXUserOverride>& overrides );
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
        // VFXS 3: the stem of the system's effect type (Serialization/VFXEffectType.hpp; UE
        // UNiagaraSystem::EffectType), resolved in the project's VFX folder. Optional only so a VFXS 2 body reads
        // for the migrator; ValidateVFXSystemData requires it.
        std::optional<std::string>  EffectType;
        std::vector<std::string>    Tags;
        float                       Duration = 1.0f; ///< seconds of one system loop, >= 0; > 0 when Loop
        bool                        Loop     = true;
        uint32_t                    Seed     = 0;
        std::vector<VFXUserParam>   UserParams;
        std::vector<VFXEmitterData> Emitters;

        [[nodiscard]] bool operator==( const VFXSystemData& ) const = default;
    };

    /// The union of the ENABLED emitters' Bounds, in the system's placement frame (UE: a system's bounds are its
    /// emitters' when it states none of its own); nullopt when no emitter is enabled. What the effects world's
    /// scalability candidate and the editor's preview framing read - never a box of its own in the file.
    [[nodiscard]] std::optional<VFXBounds> VFXSystemExtent( const VFXSystemData& data );

    /// Rejects what no later stage could honour, naming the emitter, group, module and input.
    Common::BoolResultStr ValidateVFXSystemData( const VFXSystemData& data );

    /// What the header's Dependencies state: the GUID of every module a stack row names, each once, sorted.
    [[nodiscard]] std::vector<std::string> VFXSystemDependencies( const VFXSystemData& data );

    /// Parses a `.dfx`. A file without a header, of another version or kind, that states any Dependencies
    /// (a v1 system references no other asset), or that fails validation is an error naming why.
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
