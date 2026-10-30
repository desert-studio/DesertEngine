#pragma once

// VFX-04. The emitter STACK COMPILER: an emitter's module stack (data, `.dfx`) becomes one `Domain Particle`
// shader fragment — attribute reads, the spawn modules for a newborn particle, the update modules, the
// attribute writes — that the simulation program includes (plan 02 §3.3-5).
//
// Our own generator, not UE's HLSL translator: a module is a GLSL function in a `Particle { }` block, the
// stack order is the array index, and an input reads its value from the emitter's PARAMETER BUFFER by slot,
// so the value is never in the text and editing it never rebuilds SPIR-V (01 §3.6-1). The attribute layout
// is a port of UE FNiagaraDataSetCompiledData::BuildLayout (NiagaraDataSet.cpp:1863-1884,
// NiagaraDataSetCompiledData.h:9-45): every attribute is split into components of its class (float or
// int32) and its start in each class is the prefix sum of the attributes before it.
//
// EmitterUpdate is CPU administration (spawn budgets, lifecycle — plan 02 §3.3-3) and is not compiled here.

#include <Engine/Assets/Serialization/VFXSystem.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::VFX
{
    using Assets::Serialization::VFXValueType;

    /// The four stack groups of an emitter, in the stack's order (UE ENiagaraScriptUsage: EmitterSpawnScript,
    /// EmitterUpdateScript, ParticleSpawnScript, ParticleUpdateScript). The two Emitter groups are CPU
    /// administration (EmitterSpawn runs once when the emitter instance starts, EmitterUpdate every step), the
    /// other two the GPU program.
    enum class VFXStage : uint8_t
    {
        EmitterSpawn,
        EmitterUpdate,
        ParticleSpawn,
        ParticleUpdate,
    };

    /// The stack keys of the stages, indexed by VFXStage ("EmitterSpawn", "EmitterUpdate", "ParticleSpawn",
    /// "ParticleUpdate"); a `.dfxmod`'s `Usage` line spells them so.
    [[nodiscard]] std::string_view StageName( VFXStage stage );

    /// One stage's bit in a module's Usage mask (UE UNiagaraScript::ModuleUsageBitmask).
    [[nodiscard]] constexpr uint8_t UsageBit( VFXStage stage ) { return static_cast<uint8_t>( 1u << static_cast<unsigned>( stage ) ); }

    /// The Usage bits of the two CPU (Emitter) groups.
    inline constexpr uint8_t kVFXCpuUsage = UsageBit( VFXStage::EmitterSpawn ) | UsageBit( VFXStage::EmitterUpdate );
    /// The Usage bits of the two GPU (Particle) groups.
    inline constexpr uint8_t kVFXParticleUsage = UsageBit( VFXStage::ParticleSpawn ) | UsageBit( VFXStage::ParticleUpdate );

    class VFXModuleResolver; // VFXModuleFormat.hpp

    /// One `Attribute <Name> <type>` line of a module.
    struct VFXModuleDecl
    {
        std::string  Name;
        VFXValueType Type = VFXValueType::Float;

        [[nodiscard]] bool operator==( const VFXModuleDecl& ) const = default;
    };

    /// One `Input <Name> <type> = <default>` line of a module: the value a newly added stack row starts from (UE: the
    /// module script's input default), one number per component of Type (an int / bool is one number, 0 or 1 for
    /// bool).
    struct VFXModuleInputDecl
    {
        std::string  Name;
        VFXValueType Type    = VFXValueType::Float;
        glm::vec4    Default = glm::vec4( 0.0f );
        /// The input has no Value default: a new row binds it to what the user picks (UE: Spawn From Data
        /// Channel's Channel, an asset reference that starts unset). Default is then unread.
        bool BindingRequired = false;

        [[nodiscard]] bool operator==( const VFXModuleInputDecl& ) const = default;
    };

    /**
     * @brief One stack module's block, read from its `.dfxmod` (VFXModuleFormat.hpp).
     *
     * The block opens with declaration lines — exactly one `Usage <Stage>...` (the groups the module may sit in:
     * ParticleSpawn and/or ParticleUpdate — UE's Module Usage), `Attribute <Name> <float|vec2|vec3|vec4|int|bool>`
     * (a particle attribute the module reads or writes as `p.<Name>`) and `Input <Name> <type> = <default>` (a value
     * the stack row supplies, read as `i.<Name>`) — and the rest is GLSL defining
     * `void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )`.
     */
    struct VFXParticleModule
    {
        uint8_t                         Usage = 0; ///< UsageBit of every stage it may sit in
        std::vector<VFXModuleDecl>      Attributes;
        std::vector<VFXModuleInputDecl> Inputs; ///< in declaration order (the slot order)
        std::string                     Body;   ///< the GLSL after the declarations

        [[nodiscard]] bool operator==( const VFXParticleModule& ) const = default;
    };

    /// The inside of a `.dfxmod` module block (VFXModuleFormat.hpp): the `Usage` line, the `Attribute` / `Input`
    /// declaration lines, then the GLSL. A module whose Usage has no particle stage declares inputs and no GLSL.
    Common::ResultStr<VFXParticleModule> ParseModuleBlock( const std::string& source, std::string_view where );

    /// The GLSL spelling of a value type ("float", "vec3", "int", "bool"...).
    [[nodiscard]] std::string_view GlslTypeName( VFXValueType type );

    /// The component class an attribute is stored in (UE: float / int32; half is not used).
    enum class VFXComponentClass
    {
        Float,
        Int,
    };

    [[nodiscard]] VFXComponentClass ComponentClassOf( VFXValueType type );

    /// UE FNiagaraVariableLayoutInfo: where one attribute's components start in each class.
    struct VFXAttributeLayout
    {
        std::string  Name;
        VFXValueType Type       = VFXValueType::Float;
        uint32_t     FloatStart = 0;
        uint32_t     IntStart   = 0;
        uint32_t     FloatCount = 0;
        uint32_t     IntCount   = 0;

        [[nodiscard]] bool operator==( const VFXAttributeLayout& ) const = default;
    };

    /// UE FNiagaraDataSetCompiledData's layout half.
    struct VFXDataSetLayout
    {
        std::vector<VFXAttributeLayout> Attributes; ///< in the order given to BuildLayout
        uint32_t                        TotalFloatComponents = 0;
        uint32_t                        TotalIntComponents   = 0;

        [[nodiscard]] const VFXAttributeLayout* Find( std::string_view name ) const;
        [[nodiscard]] bool                      operator==( const VFXDataSetLayout& ) const = default;
    };

    /// Port of FNiagaraDataSetCompiledData::BuildLayout: the variables in the given order, each start the prefix
    /// sum of its class.
    [[nodiscard]] VFXDataSetLayout BuildLayout( const std::vector<VFXModuleDecl>& variables );

    /// The stack group a parameter slot's input belongs to (only the two GPU groups have slots).
    enum class VFXStackGroup
    {
        ParticleSpawn,
        ParticleUpdate,
    };

    /// What fills one row (a vec4) of the emitter's parameter buffer.
    struct VFXParamSlot
    {
        enum class Kind
        {
            Value,     ///< the input's Value
            RandomMin, ///< the input's Random.Min
            RandomMax, ///< the input's Random.Max
            User,      ///< the system's UserParams row named User
            Curve,     ///< the input's LUT in the system's curve atlas: CurveParamRow (VFXCurveLUT.hpp)
            PointSet,  ///< the placement's point set named User: (first, count) in the emitter's point-set buffer
                       ///< (VFX-15)
        };

        Kind          SlotKind = Kind::Value;
        VFXStackGroup Group    = VFXStackGroup::ParticleSpawn;
        uint32_t      Module   = 0; ///< index into the group's array
        std::string   Input;        ///< the input's name (Value / RandomMin / RandomMax)
        std::string   User;         ///< the user parameter's name, without "User." (User)

        [[nodiscard]] bool operator==( const VFXParamSlot& ) const = default;
    };

    /// One emitter's compiled stack.
    /**
     * WHEN IN THE FRAME AN EMITTER'S GPU SIMULATION RUNS (UE ENiagaraGpuComputeTickStage: PreInitViews /
     * PostOpaqueRender, chosen per emitter from what its stack reads - FNiagaraSystemInstance::CalculateTickStage
     * over the data interfaces' RequiresDepthBuffer()). Derived, never stated in the file: a stack cannot ask for a
     * stage its modules do not need, and one that reads the scene cannot forget it. PreOpaque: before the opaque
     * passes, so its opaque rows draw this frame's particles. PostOpaque: after the opaque passes and the scene depth
     * resolve, so a host function that reads the scene (kVFXSceneReadHostFunctions) sees THIS frame's depth; its
     * draws recorded before it (G-buffer rows, Mesh instances, shadow casts) draw the previous tick's particles (UE:
     * the same one-frame lag of a PostOpaque emitter's opaque renderers).
     */
    enum class VFXTickStage : uint8_t
    {
        PreOpaque,
        PostOpaque,
    };

    /// The contract host functions (Common/VFXParticleContract.glslh) that read this frame's scene (UE: the data
    /// interface functions whose RequiresDepthBuffer() is true). A stack whose GLSL calls one ticks PostOpaque.
    inline constexpr std::array<std::string_view, 1> kVFXSceneReadHostFunctions = { "VFX_SceneDepthCollide" };

    struct VFXCompiledEmitter
    {
        VFXTickStage              TickStage = VFXTickStage::PreOpaque; ///< from the host functions its GLSL calls
        uint64_t                  Key = 0;    ///< FNV-64 of the stack STRUCTURE + the layout; no input value
        std::string               ShaderName; ///< "VFX/Emitter/<Key as 16 hex digits>"
        std::string               ShaderText; ///< a whole `.shader`: `Domain Particle` + one Particle block
        VFXDataSetLayout          Layout;
        std::vector<VFXParamSlot> Slots; ///< the parameter buffer, row by row
    };

    /// What the stack editor needs of any module, CPU or GPU (UE: the module script's usage + input defaults), read
    /// from its `.dfxmod`.
    struct VFXModuleDescription
    {
        Assets::AssetGuidRef            Module; ///< {Guid, Path} a new stack row states
        std::string                     Name;   ///< the module's DisplayName
        uint8_t                         Usage = 0;
        std::vector<VFXModuleInputDecl> Inputs;
    };

    /// Describes the module @p module names through @p modules. An unknown / unparsable module is an error, not an
    /// empty description.
    Common::ResultStr<VFXModuleDescription> DescribeModule( const Assets::AssetGuidRef& module,
                                                            const VFXModuleResolver&    modules );
    /**
     * @brief Compiles emitter @p emitterIndex of @p system.
     *
     * Every row's module is the `.dfxmod` @p modules resolves its GUID to (the engine answers from the
     * ContentRegistry, Assets::RegistryVFXModuleResolver), and it must state the row's group as a Stage. A
     * module that also states EmitterUpdate gives its GLSL no inputs: the row's inputs are its CPU half's. A
     * disabled module contributes nothing. An input the module
     * does not declare, a declared input the row leaves out, a type that disagrees, an attribute declared with
     * two types are errors naming the emitter, group, module and input.
     */
    Common::ResultStr<VFXCompiledEmitter> CompileEmitterStack( const Assets::Serialization::VFXSystemData& system,
                                                               std::size_t                  emitterIndex,
                                                               const VFXModuleResolver&     modules );

    struct VFXCurveAtlas;

    /// The parameter buffer of a compiled emitter: one vec4 per slot, from the stack's values, the system's
    /// user parameter defaults (a placed component's overrides replace User rows later) and, for a Curve input,
    /// where its table sits in @p curves (BuildCurveAtlas of the same system). A slot whose input is gone or
    /// changed source, or whose table is missing, is an error: the stack's structure moved and @p compiled is
    /// stale.
    Common::ResultStr<std::vector<glm::vec4>>
    BuildEmitterParams( const VFXCompiledEmitter& compiled, const Assets::Serialization::VFXSystemData& system,
                        std::size_t emitterIndex, const VFXCurveAtlas& curves );
} // namespace Desert::VFX
