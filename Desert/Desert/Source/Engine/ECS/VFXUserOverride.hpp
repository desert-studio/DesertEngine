#pragma once

#include <CoreReflection/ReflectionMacros.hpp>

#include <glm/glm.hpp>

#include <string>

namespace Desert::ECS
{
    /// A PLACED system's own value for one of its User parameters (UE UNiagaraComponent::OverrideParameters, one
    /// entry of its FNiagaraUserRedirectionParameterStore): stored on the VFXComponent, not in the `.dfx`, so two
    /// placements of one system differ by it (an impact's SurfaceNormal per hit). Value is packed like the row's
    /// Default (the type's components first, the rest 0); the row's Type says how many are read. A ScriptStruct,
    /// so the component's list of them serializes, and travels to a script, as any reflected container.
    struct VFXUserOverride
    {
        REFLECT( ScriptStruct )

        PROPERTY( DisplayName( "Name" ), Tooltip( "The system's User parameter, without the \"User.\" prefix" ) )
        std::string Name;

        PROPERTY( DisplayName( "Value" ), Tooltip( "Packed like the parameter's Default: its components first" ) )
        glm::vec4 Value = glm::vec4( 0.0f );

        [[nodiscard]] bool operator==( const VFXUserOverride& ) const = default;
    };
} // namespace Desert::ECS
