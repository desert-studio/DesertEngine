#include <Common/Core/UUID.hpp>

#include <cstdint>
#include <random>

namespace Common
{
    static std::random_device                      s_RandomDevice;
    static std::mt19937_64                         eng( s_RandomDevice() );
    static std::uniform_int_distribution<uint64_t> s_UniformDistribution;

    UUID::UUID( uint64_t uuid ) : m_UUID( uuid )
    {
    }

    UUID UUID::Generate()
    {
        // Never hand out 0: it is the null id, and a "fresh" identity that compares equal to "no identity"
        // is worse than no identity at all. One draw in 2^64 lands here.
        const uint64_t value = s_UniformDistribution( eng );
        return UUID( value ? value : 1ull );
    }

    UUID::UUID( const std::string& uuidStr )
    {
        try
        {
            m_UUID = std::stoull( uuidStr );
        }
        catch ( const std::exception& )
        {
            throw std::invalid_argument( "Invalid UUID string: " + uuidStr );
        }
    }

} // namespace Common