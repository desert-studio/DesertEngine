#include <Engine/Core/WorldParameterInstance.hpp>

#include <cmath>
#include <format>

namespace Desert::Core
{
    namespace
    {
        using Assets::Serialization::WorldParameterType;

        constexpr const char* TypeName( const WorldParameterType type )
        {
            return type == WorldParameterType::Scalar ? "scalar" : "vector";
        }
    } // namespace

    void WorldParameterInstance::Adopt( const Common::AssetHandle&                        source,
                                        const Assets::Serialization::WorldParametersData* data )
    {
        if ( data == nullptr )
        {
            m_Source  = Common::AssetHandle{};
            m_Adopted = {};
            m_Values.clear();
            return;
        }
        if ( source == m_Source && data->Parameters == m_Adopted.Parameters )
            return; // the same collection as adopted: the live values stand
        m_Source             = source;
        m_Adopted.Parameters = data->Parameters;
        m_Values.clear();
        m_Values.reserve( data->Parameters.size() );
        for ( const auto& row : data->Parameters )
            m_Values.push_back( { row.Name, Assets::Serialization::WorldParameterKey( row.Name ), row.Type,
                                  glm::vec4( row.Default[0], row.Default[1], row.Default[2], row.Default[3] ) } );
    }

    const WorldParameterValue* WorldParameterInstance::Find( const std::string_view   name,
                                                             const WorldParameterType type,
                                                             std::string&             why ) const
    {
        if ( m_Source == Common::AssetHandle{} )
        {
            why = std::format( "'{}': the scene names no world parameter collection", name );
            return nullptr;
        }
        for ( const auto& value : m_Values )
        {
            if ( value.Name != name )
                continue;
            if ( value.Type != type )
            {
                why = std::format( "'{}' is a {} parameter, not a {}", name, TypeName( value.Type ),
                                   TypeName( type ) );
                return nullptr;
            }
            return &value;
        }
        why = std::format( "the scene's collection has no parameter '{}'", name );
        return nullptr;
    }

    Common::BoolResultStr WorldParameterInstance::SetScalar( const std::string_view name, const float value )
    {
        if ( !std::isfinite( value ) )
            return Common::MakeFormattedError<bool>( "'{}': {} is not finite", name, value );
        std::string why;
        const auto* found = Find( name, WorldParameterType::Scalar, why );
        if ( found == nullptr )
            return Common::MakeError( why );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): Find is the one lookup; this object owns the row
        const_cast<WorldParameterValue*>( found )->Value = glm::vec4( value, 0.0f, 0.0f, 0.0f );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr WorldParameterInstance::SetVector( const std::string_view name, const glm::vec4& value )
    {
        for ( int c = 0; c < 4; ++c )
            if ( !std::isfinite( value[c] ) )
                return Common::MakeFormattedError<bool>( "'{}': component {} ({}) is not finite", name, c,
                                                         value[c] );
        std::string why;
        const auto* found = Find( name, WorldParameterType::Vector, why );
        if ( found == nullptr )
            return Common::MakeError( why );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): Find is the one lookup; this object owns the row
        const_cast<WorldParameterValue*>( found )->Value = value;
        return BOOLSUCCESS;
    }

    Common::ResultStr<float> WorldParameterInstance::GetScalar( const std::string_view name ) const
    {
        std::string why;
        const auto* found = Find( name, WorldParameterType::Scalar, why );
        if ( found == nullptr )
            return Common::MakeError<float>( why );
        return Common::MakeSuccess( found->Value.x );
    }

    Common::ResultStr<glm::vec4> WorldParameterInstance::GetVector( const std::string_view name ) const
    {
        std::string why;
        const auto* found = Find( name, WorldParameterType::Vector, why );
        if ( found == nullptr )
            return Common::MakeError<glm::vec4>( why );
        return Common::MakeSuccess( found->Value );
    }
} // namespace Desert::Core
