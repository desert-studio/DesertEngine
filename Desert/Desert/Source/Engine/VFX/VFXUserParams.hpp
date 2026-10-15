#pragma once

// USER PARAMETERS OF A PLACED SYSTEM (UE: UNiagaraComponent::OverrideParameters over the system's exposed
// User.* store, FNiagaraUserRedirectionParameterStore).
//
// A `.dfx` declares its User.* rows (VFXSystemData::UserParams: name, type, default). A placed system reads one
// value per row, in the system's row order: the row's Default, unless the placing VFXComponent overrides it by
// name (VFXComponentData::UserParams). The component's override list is the ONE store every writer goes through
// - the Details panel, a script's VFX.setParam, a Sequencer VFX parameter track - via SetUserParamOverride, so
// a value is canonical (its type's unused components zero, an Int / Bool whole) wherever it came from.
//
// VFXWorld resolves the values once per tick per instance (SystemInstance::User); the CPU spawn plan and the
// GPU parameter buffer's User rows (VFXParamSlot::Kind::User) read that resolved array, never the defaults.

#include <Engine/Assets/Serialization/VFXSystem.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::ECS
{
    struct VFXUserParamOverride;
}

namespace Desert::VFX
{
    /// @p value with the components @p type does not use zeroed; Int rounded to a whole number, Bool 0 or 1 -
    /// one value has one spelling (the same rule the `.dfx` reader applies to a Default).
    [[nodiscard]] glm::vec4 CanonicalUserValue( Assets::Serialization::VFXValueType type, const glm::vec4& value );

    /// The index of the UserParams row named @p name (without "User."), or nullopt.
    [[nodiscard]] std::optional<std::size_t> FindUserParam( const Assets::Serialization::VFXSystemData& system,
                                                            std::string_view                            name );

    /// What a placed system's overrides resolve to.
    struct VFXResolvedUserParams
    {
        std::vector<glm::vec4>   Values;  ///< one per UserParams row, in the system's order
        std::vector<std::string> Unknown; ///< overrides naming no row of this system (renamed or removed there)
    };

    /// Every row's Default, each override that names a row replacing it (canonicalised by the row's type). An
    /// override naming no row changes nothing and is listed in Unknown so the caller reports it by name.
    [[nodiscard]] VFXResolvedUserParams ResolveUserParams( const Assets::Serialization::VFXSystemData& system,
                                                           std::span<const ECS::VFXUserParamOverride>  overrides );

    /// Sets (adds or replaces) the override of row @p name to @p value, canonicalised by the row's type. A name
    /// the system has no row for is refused by name and changes nothing.
    Common::BoolResultStr SetUserParamOverride( std::vector<ECS::VFXUserParamOverride>&     overrides,
                                                const Assets::Serialization::VFXSystemData& system,
                                                std::string_view name, const glm::vec4& value );

    /// Removes the override of @p name (the row reads its Default again); false when there was none.
    bool ClearUserParamOverride( std::vector<ECS::VFXUserParamOverride>& overrides, std::string_view name );

    /// One User row of a compiled emitter's parameter buffer: buffer row Row takes resolved value User.
    struct VFXUserParamRow
    {
        std::uint32_t Row  = 0;
        std::uint32_t User = 0;

        [[nodiscard]] bool operator==( const VFXUserParamRow& ) const = default;
    };

    struct VFXCompiledEmitter;

    /// The User slots of @p compiled, each tied to its UserParams row of @p system. A slot naming no row is an
    /// error: the stack's structure moved after the compile and the program is stale.
    Common::ResultStr<std::vector<VFXUserParamRow>>
    UserParamRowsOf( const VFXCompiledEmitter& compiled, const Assets::Serialization::VFXSystemData& system );

    /// Writes the resolved values into the parameter buffer's User rows.
    void ApplyUserParamRows( std::vector<glm::vec4>& paramRows, std::span<const VFXUserParamRow> rows,
                             std::span<const glm::vec4> values );
} // namespace Desert::VFX
