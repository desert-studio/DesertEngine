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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::VFX
{
    using Assets::Serialization::VFXValueType;

    class VFXModuleResolver; // VFXModuleFormat.hpp

    /// The slider bounds an `Input` line states with `Range=<min>..<max>` (UE: the module input's UIMin / UIMax).
    struct VFXInputRange
    {
        float Min = 0.0f;
        float Max = 0.0f;

        [[nodiscard]] bool operator==( const VFXInputRange& ) const = default;
    };

    /// One `Attribute <Name> <type>` or `Input <Name> <type> [Units=<unit>] [Range=<min>..<max>]
    /// [Default=<c0>,<c1>...]` line of a module. Units, Range and Default are the input's EDITOR metadata (UE:
    /// the module input's DisplayUnit, UIMin / UIMax and default value): the VFX System editor labels and bounds
    /// its widgets with them and fills a newly added module's row from Default (one number per component of the
    /// type; absent = zero); the compiler ignores them.
    struct VFXModuleDecl
    {
        std::string                  Name;
        VFXValueType                 Type = VFXValueType::Float;
        std::string                  Units; ///< "cm", "cm/s", "deg", "s", "1/s"... empty: a plain number
        std::optional<VFXInputRange> Range{};
        std::optional<glm::vec4>     Default{}; ///< the unused components are zero

        [[nodiscard]] bool operator==( const VFXModuleDecl& ) const = default;
    };

    /**
     * @brief One stack module's block, read from its `.dfxmod` (VFXModuleFormat.hpp).
     *
     * The block opens with declaration lines — `Attribute <Name> <float|vec2|vec3|vec4|int|bool>` (a particle
     * attribute the module reads or writes as `p.<Name>`) and `Input <Name> <type>` (a value the stack row
     * supplies, read as `i.<Name>`; optionally followed by `Units=<unit>`, `Range=<min>..<max>` and
     * `Default=<c0>,<c1>...`, the editor's metadata, never on an Attribute) — and the rest is GLSL defining
     * `void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )`.
     */
    struct VFXParticleModule
    {
        std::vector<VFXModuleDecl> Attributes;
        std::vector<VFXModuleDecl> Inputs; ///< in declaration order (the slot order)
        std::string                Body;   ///< the GLSL after the declarations

        [[nodiscard]] bool operator==( const VFXParticleModule& ) const = default;
    };

    /// The inside of a `.dfxmod` module block (VFXModuleFormat.hpp): the `Attribute` / `Input` declaration
    /// lines, then the GLSL. @p requireModuleFunction is false only for a module with no particle stage, which
    /// declares inputs and has no GLSL.
    Common::ResultStr<VFXParticleModule> ParseModuleBlock( const std::string& source, std::string_view where,
                                                           bool requireModuleFunction );

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
        };

        Kind          SlotKind = Kind::Value;
        VFXStackGroup Group    = VFXStackGroup::ParticleSpawn;
        uint32_t      Module   = 0; ///< index into the group's array
        std::string   Input;        ///< the input's name (Value / RandomMin / RandomMax)
        std::string   User;         ///< the user parameter's name, without "User." (User)

        [[nodiscard]] bool operator==( const VFXParamSlot& ) const = default;
    };

    /**
     * WHEN IN THE FRAME AN EMITTER'S GPU SIMULATION RUNS (UE ENiagaraGpuComputeTickStage: PreInitViews /
     * PostOpaqueRender, chosen per emitter from what its stack reads - FNiagaraSystemInstance::CalculateTickStage
     * over the data interfaces' RequiresDepthBuffer()). PreOpaque: before the opaque passes, so its opaque rows
     * draw this frame's particles. PostOpaque: after the opaque passes, so a host function that reads the scene
     * (kVFXSceneReadHostFunctions) sees THIS frame's depth; its opaque rows, drawn before it, draw the previous
     * frame's particles (UE: the same one-frame lag of a PostOpaque emitter's opaque renderers).
     */
    enum class VFXTickStage : uint8_t
    {
        PreOpaque,
        PostOpaque,
    };

    /// The contract host functions (Common/VFXParticleContract.glslh) that read this frame's scene (UE: the data
    /// interface functions whose RequiresDepthBuffer() is true). A stack whose GLSL calls one ticks PostOpaque.
    inline constexpr std::array<std::string_view, 1> kVFXSceneReadHostFunctions = { "VFX_SceneDepthCollide" };

    /// One emitter's compiled stack.
    struct VFXCompiledEmitter
    {
        VFXTickStage              TickStage = VFXTickStage::PreOpaque; ///< from the host functions its GLSL calls
        uint64_t                  Key = 0;    ///< FNV-64 of the stack STRUCTURE + the layout; no input value
        std::string               ShaderName; ///< "VFX/Emitter/<Key as 16 hex digits>"
        std::string               ShaderText; ///< a whole `.shader`: `Domain Particle` + one Particle block
        VFXDataSetLayout          Layout;
        std::vector<VFXParamSlot> Slots; ///< the parameter buffer, row by row
    };

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
                                                               std::size_t              emitterIndex,
                                                               const VFXModuleResolver& modules );

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
