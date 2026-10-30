#include "MultiEdit.hpp"

#include <cstring>
#include <vector>

namespace Desert::Editor
{
    namespace
    {
        const std::byte* At( const void* base, std::size_t offset )
        {
            return static_cast<const std::byte*>( base ) + offset;
        }
        std::byte* At( void* base, std::size_t offset )
        {
            return static_cast<std::byte*>( base ) + offset;
        }
    } // namespace

    bool FieldDiffers( const void* a, const void* b, std::size_t offset, std::size_t size )
    {
        if ( !a || !b || size == 0 )
            return false;
        return std::memcmp( At( a, offset ), At( b, offset ), size ) != 0;
    }

    bool AnyFieldDiffers( const void* base, const std::vector<void*>& others, std::size_t offset,
                          std::size_t size )
    {
        for ( const void* o : others )
            if ( o && FieldDiffers( base, o, offset, size ) )
                return true;
        return false;
    }

    void BroadcastField( const void* src, const std::vector<void*>& dst, std::size_t offset,
                         std::size_t size )
    {
        if ( !src || size == 0 )
            return;
        for ( void* d : dst )
            if ( d )
                std::memcpy( At( d, offset ), At( src, offset ), size );
    }
} // namespace Desert::Editor
