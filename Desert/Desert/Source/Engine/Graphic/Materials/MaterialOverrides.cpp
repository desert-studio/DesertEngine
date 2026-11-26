#include <Engine/Graphic/Materials/MaterialOverrides.hpp>

namespace Desert::Graphic
{
    void LogRefusedTextureOverride( std::string_view shaderName, std::string_view sampler )
    {
        static std::mutex            said;
        static std::set<std::string> alreadySaid;
        {
            const std::lock_guard<std::mutex> lock( said );
            std::string                       key( shaderName );
            key.append( 1, '/' ).append( sampler );
            if ( !alreadySaid.insert( std::move( key ) ).second )
                return;
        }

        LOG_ERROR( "[Material] '{}' has no Properties texture '{}' — the override is ignored and the "
                   "surface draws the shader's own default. Only textures declared in the shader's "
                   "Properties are material parameters; every other sampler is bound by its pass.",
                   shaderName, sampler );
    }

    bool MaterialValueIsReadableAsInt( std::string_view name, float value )
    {
        constexpr float kLowest  = -2147483648.0f; // exactly representable
        constexpr float kLargest = 2147483520.0f;  // the largest float below 2^31

        if ( std::isfinite( value ) && value >= kLowest && value <= kLargest )
            return true;

        static std::mutex            said;
        static std::set<std::string> alreadySaid;
        {
            const std::lock_guard<std::mutex> lock( said );
            if ( !alreadySaid.insert( std::string( name ) ).second )
                return false;
        }

        LOG_ERROR( "[Material] parameter '{}' carries {}, which no 32-bit integer can hold — the cast "
                   "alone would be undefined before any range check could see it. The parameter keeps "
                   "its shader default for the rest of this session.",
                   name, value );
        return false;
    }

    bool MaterialValueIsReadable( std::string_view name, const glm::vec4& value, MaterialValueLanes lanes )
    {
        for ( int lane = 0; lane < static_cast<int>( lanes ); ++lane )
        {
            if ( std::isfinite( value[lane] ) )
                continue;

            static std::mutex            said;
            static std::set<std::string> alreadySaid;
            {
                const std::lock_guard<std::mutex> lock( said );
                if ( !alreadySaid.insert( std::string( name ) ).second )
                    return false;
            }

            LOG_ERROR( "[Material] parameter '{}' carries {} in component {}, which is not a number the "
                       "renderer can use — a clamp does not repair it, every comparison against it is "
                       "false. The parameter keeps its shader default for the rest of this session.",
                       name, value[lane], lane );
            return false;
        }
        return true;
    }
}
