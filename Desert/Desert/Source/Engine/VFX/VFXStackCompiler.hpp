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

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::VFX
{
    using Assets::Serialization::VFXValueType;

    /// One `Attribute <Name> <type>` or `Input <Name> <type>` line of a module.
    struct VFXModuleDecl
    {
        std::string  Name;
        VFXValueType Type = VFXValueType::Float;

        [[nodiscard]] bool operator==( const VFXModuleDecl& ) const = default;
    };

    /**
     * @brief One stack module, read from its `.shader` (`Domain Particle`, a `Particle { }` block).
     *
     * The block opens with declaration lines — `Attribute <Name> <float|vec2|vec3|vec4|int|bool>` (a particle
     * attribute the module reads or writes as `p.<Name>`) and `Input <Name> <type>` (a value the stack row
     * supplies, read as `i.<Name>`) — and the rest is GLSL defining
     * `void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )`.
     */
    struct VFXParticleModule
    {
        std::vector<VFXModuleDecl> Attributes;
        std::vector<VFXModuleDecl> Inputs; ///< in declaration order (the slot order)
        std::string                Body;   ///< the GLSL after the declarations
    };

    /// Parses one module's whole `.shader` text. @p where names it in every error.
    Common::ResultStr<VFXParticleModule> ParseParticleModule( const std::string& shaderText,
                                                              std::string_view   where );

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
        };

        Kind          SlotKind = Kind::Value;
        VFXStackGroup Group    = VFXStackGroup::ParticleSpawn;
        uint32_t      Module   = 0; ///< index into the group's array
        std::string   Input;        ///< the input's name (Value / RandomMin / RandomMax)
        std::string   User;         ///< the user parameter's name, without "User." (User)

        [[nodiscard]] bool operator==( const VFXParamSlot& ) const = default;
    };

    /// One emitter's compiled stack.
    struct VFXCompiledEmitter
    {
        uint64_t                  Key = 0;    ///< FNV-64 of the stack STRUCTURE + the layout; no input value
        std::string               ShaderName; ///< "VFX/Emitter/<Key as 16 hex digits>"
        std::string               ShaderText; ///< a whole `.shader`: `Domain Particle` + one Particle block
        VFXDataSetLayout          Layout;
        std::vector<VFXParamSlot> Slots; ///< the parameter buffer, row by row
    };

    /// Where the engine module library lives: ShaderDir()/VFX/Modules; `engine:<Name>` is `<Name>.shader` there.
    [[nodiscard]] std::filesystem::path EngineModuleDir();

    /**
     * @brief Compiles emitter @p emitterIndex of @p system.
     *
     * `engine:<Name>` modules are read from @p engineModuleDir (a missing file is an error naming its path),
     * `local:<Id>` from the system's LocalModules. A disabled module contributes nothing. An input the module
     * does not declare, a declared input the row leaves out, a type that disagrees, an attribute declared with
     * two types and a Curve source (its LUT is VFX-05) are errors naming the emitter, group, module and input.
     */
    Common::ResultStr<VFXCompiledEmitter> CompileEmitterStack( const Assets::Serialization::VFXSystemData& system,
                                                               std::size_t                  emitterIndex,
                                                               const std::filesystem::path& engineModuleDir );

    /// The parameter buffer of a compiled emitter: one vec4 per slot, from the stack's values and the system's
    /// user parameter defaults (a placed component's overrides replace User rows later). A slot whose input is
    /// gone or changed source is an error: the stack's structure moved and @p compiled is stale.
    Common::ResultStr<std::vector<glm::vec4>>
    BuildEmitterParams( const VFXCompiledEmitter& compiled, const Assets::Serialization::VFXSystemData& system,
                        std::size_t emitterIndex );
} // namespace Desert::VFX
