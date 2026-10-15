#include <Engine/VFX/VFXUserParams.hpp>

#include <Engine/ECS/Components.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/VFX/VFXSystemService.hpp>
#include <Engine/VFX/VFXStackCompiler.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace Desert::VFX
{
    namespace S = Assets::Serialization;

    glm::vec4 CanonicalUserValue( const S::VFXValueType type, const glm::vec4& value )
    {
        glm::vec4 out( 0.0f );
        switch ( type )
        {
            case S::VFXValueType::Int:
                out.x = std::round( value.x );
                return out;
            case S::VFXValueType::Bool:
                out.x = value.x != 0.0f ? 1.0f : 0.0f;
                return out;
            default:
                break;
        }
        const uint32_t used = S::ComponentCount( type );
        for ( uint32_t c = 0; c < used && c < 4; ++c )
            out[static_cast<glm::length_t>( c )] = value[static_cast<glm::length_t>( c )];
        return out;
    }

    std::optional<std::size_t> FindUserParam( const S::VFXSystemData& system, const std::string_view name )
    {
        for ( std::size_t i = 0; i < system.UserParams.size(); ++i )
            if ( system.UserParams[i].Name == name )
                return i;
        return std::nullopt;
    }

    VFXResolvedUserParams ResolveUserParams( const S::VFXSystemData&                    system,
                                             std::span<const ECS::VFXUserParamOverride> overrides )
    {
        VFXResolvedUserParams resolved;
        resolved.Values.reserve( system.UserParams.size() );
        for ( const S::VFXUserParam& row : system.UserParams )
            resolved.Values.push_back( row.Default );
        for ( const ECS::VFXUserParamOverride& o : overrides )
        {
            const std::optional<std::size_t> index = FindUserParam( system, o.Name );
            if ( !index )
            {
                resolved.Unknown.push_back( o.Name );
                continue;
            }
            resolved.Values[*index] = CanonicalUserValue( system.UserParams[*index].Type, o.Value );
        }
        return resolved;
    }

    Common::BoolResultStr SetUserParamOverride( std::vector<ECS::VFXUserParamOverride>& overrides,
                                                const S::VFXSystemData& system, const std::string_view name,
                                                const glm::vec4& value )
    {
        const std::optional<std::size_t> index = FindUserParam( system, name );
        if ( !index )
            return Common::MakeFormattedError<bool>( "the system has no user parameter '{}'", name );
        for ( float c : { value.x, value.y, value.z, value.w } )
            if ( !std::isfinite( c ) )
                return Common::MakeFormattedError<bool>( "user parameter '{}': the value is not finite", name );
        const glm::vec4 canonical = CanonicalUserValue( system.UserParams[*index].Type, value );
        const auto      it        = std::find_if( overrides.begin(), overrides.end(),
                                                  [&]( const ECS::VFXUserParamOverride& o ) { return o.Name == name; } );
        if ( it != overrides.end() )
            it->Value = canonical;
        else
            overrides.push_back( ECS::VFXUserParamOverride{ std::string( name ), canonical } );
        return Common::MakeSuccess( true );
    }

    bool ClearUserParamOverride( std::vector<ECS::VFXUserParamOverride>& overrides, const std::string_view name )
    {
        const auto it = std::find_if( overrides.begin(), overrides.end(),
                                      [&]( const ECS::VFXUserParamOverride& o ) { return o.Name == name; } );
        if ( it == overrides.end() )
            return false;
        overrides.erase( it );
        return true;
    }

    Common::BoolResultStr SetComponentUserParam( ECS::VFXComponentData& data, const std::string_view name,
                                                 const glm::vec4& value )
    {
        if ( static_cast<std::uint64_t>( data.System ) == 0 )
            return Common::MakeFormattedError<bool>( "User.{}: the VFX component plays no system", name );
        const auto system = Runtime::ResourceRegistry::GetVFXSystemService()->Get( data.System );
        if ( !system.IsSuccess() )
            return Common::MakeFormattedError<bool>( "User.{}: {}", name, system.GetError() );
        if ( !system.GetValue() )
            return Common::MakeFormattedError<bool>( "User.{}: the VFX system is still loading", name );
        return SetUserParamOverride( data.UserParams, *system.GetValue(), name, value );
    }

    Common::ResultStr<std::vector<VFXUserParamRow>> UserParamRowsOf( const VFXCompiledEmitter& compiled,
                                                                     const S::VFXSystemData&   system )
    {
        using Result = std::vector<VFXUserParamRow>;
        Result rows;
        for ( std::size_t s = 0; s < compiled.Slots.size(); ++s )
        {
            const VFXParamSlot& slot = compiled.Slots[s];
            if ( slot.SlotKind != VFXParamSlot::Kind::User )
                continue;
            const std::optional<std::size_t> index = FindUserParam( system, slot.User );
            if ( !index )
                return Common::MakeFormattedError<Result>( "slot {}: no UserParams row '{}' - recompile the stack",
                                                           s, slot.User );
            rows.push_back( { static_cast<std::uint32_t>( s ), static_cast<std::uint32_t>( *index ) } );
        }
        return Common::MakeSuccess( std::move( rows ) );
    }

    void ApplyUserParamRows( std::vector<glm::vec4>& paramRows, std::span<const VFXUserParamRow> rows,
                             std::span<const glm::vec4> values )
    {
        // Both spans come from the same system as the compile (UserParamRowsOf / ResolveUserParams), so every
        // index is in range by construction; the guard keeps a buffer that was padded for an empty stack safe.
        for ( const VFXUserParamRow& r : rows )
            if ( r.Row < paramRows.size() && r.User < values.size() )
                paramRows[r.Row] = values[r.User];
    }
} // namespace Desert::VFX
