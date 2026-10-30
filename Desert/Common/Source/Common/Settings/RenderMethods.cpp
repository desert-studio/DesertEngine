#include "RenderMethods.hpp"

#include <cstdint>
#include <string_view>

namespace Common::Scalability
{
    namespace
    {
        constexpr std::string_view kNotImplemented = "declared, but this build does not implement it yet";

        template <typename E>
        std::string_view RefuseUnimplemented( std::int64_t value )
        {
            return IsImplemented( static_cast<E>( value ) ) ? std::string_view{} : kNotImplemented;
        }
    } // namespace

    std::string_view RefuseGIMethod( std::int64_t value )
    {
        return RefuseUnimplemented<GIMethod>( value );
    }

    std::string_view RefuseReflectionMethod( std::int64_t value )
    {
        return RefuseUnimplemented<ReflectionMethod>( value );
    }

    std::string_view RefuseShadowMethod( std::int64_t value )
    {
        return RefuseUnimplemented<ShadowMethod>( value );
    }

    std::string_view RefuseContactShadows( std::int64_t value )
    {
        return ( value != 0 && !kContactShadowsImplemented ) ? kNotImplemented : std::string_view{};
    }

    std::string_view RefuseDistanceFieldShadows( std::int64_t value )
    {
        return ( value != 0 && !kDistanceFieldShadowsImplemented ) ? kNotImplemented : std::string_view{};
    }
} // namespace Common::Scalability
