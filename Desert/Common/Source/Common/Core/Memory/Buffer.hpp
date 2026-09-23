#pragma once

#include <Common/Core/Core.hpp>

#include <cstring>

namespace Common::Memory
{
    struct Buffer
    {
        void*       Data;
        std::size_t Size;
        std::size_t AllocatedSize;

        Buffer() : Data( nullptr ), Size( 0 ), AllocatedSize( 0 )
        {
        }

        Buffer( void* data, std::size_t size ) : Data( data ), Size( 0 ), AllocatedSize( size )
        {
        }

        void Allocate( std::size_t size )
        {
            Release();

            if ( size == 0 )
                return;

            Data          = new std::byte[size];
            AllocatedSize = size;
            Size = size;
        }

        // THE CAST IS THE WHOLE POINT, not tidiness. `Data` is `void*` and this said `delete[] Data;`,
        // which C++ does not define: with no type there is no array cookie to consult and no element
        // type to destroy, so what the deallocation does is up to the implementation. clang accepts it
        // as an extension and says so (`-Wdelete-incomplete`, on by DEFAULT) — the workspace's `-w`
        // was the only reason nobody heard it.
        //
        // `std::byte` is not a guess: Allocate() below is `new std::byte[size]` and Copy() goes
        // through Allocate(), so every Buffer that owns memory owns a `std::byte[]`. Restoring that
        // type is what makes this a defined array delete. (The `Buffer(void*, size)` constructor
        // adopts a pointer of unknown origin and has no user in the tree; if one ever appears, this
        // Release() is wrong for it and the ownership has to become explicit rather than assumed.)
        void Release()
        {
            if ( Data )
            {
                delete[] static_cast<std::byte*>( Data );
                Data = nullptr;

                Size          = 0;
                AllocatedSize = 0;
            }
        }

        void ZeroInitialize()
        {
            if ( Data )
                memset( Data, 0, AllocatedSize );
        }

        template <typename T>
        T& Read( std::size_t offset = 0 )
        {
            return *(T*)( (const char*)Data + offset );
        }

        inline void Write( const void* data, std::size_t size, std::size_t offset = 0 )
        {
            DESERT_VERIFY( size + offset <= AllocatedSize, "Buffer overflow!" );
            memcpy( (std::byte*)Data + offset, data, size );

            Size = size;
        }

        static Buffer Copy( const void* data, std::size_t size )
        {
            Buffer buffer;
            buffer.Allocate( size );

            memcpy( buffer.Data, data, size );
            return buffer;
        }

        template <typename T>
        T* As() const
        {
            return (T*)( Data );
        }

        operator bool() const
        {
            return Data;
        }

        std::byte& operator[]( int index )
        {
            return ( (std::byte*)Data )[index];
        }

        std::byte operator[]( int index ) const
        {
            return ( (std::byte*)Data )[index];
        }

        // const: a Buffer handed out by const reference (FieldProperty::GetLocalData) could not be
        // asked how big it was, which is exactly what a caller holding one wants to know.
        inline std::size_t GetSize() const
        {
            return Size;
        }
        inline std::size_t GetAllocatedSize() const
        {
            return AllocatedSize;
        }

        template <typename T>
        bool operator==( T& rhs )
        {
            return Data == rhs.Data;
        }
    };
} // namespace Common::Memory