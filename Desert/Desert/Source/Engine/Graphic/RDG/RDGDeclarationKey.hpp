#pragma once

#include <Engine/Graphic/RDG/RDGAccess.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <bit>
#include <cstdint>

namespace Desert::Graphic::RDG
{
    // RDG-RECORD-COMPILE-CACHE: the declaration key - an xxHash64 round per 64-bit word, its avalanche at the
    // end. CPU-COMPILE-KEY: a value (no allocation), so the builder keeps running ones while it is declared
    // (Builder::m_ResourceKey, m_PassKey) and Compile only finishes them (Compiler::HashDeclaration).
    class DeclarationHasher
    {
    public:
        void Add( uint64_t value ) noexcept
        {
            m_Hash ^= Round( value );
            m_Hash = std::rotl( m_Hash, 27 ) * kPrime1 + kPrime4;
        }
        void AddFloat( float value ) noexcept
        {
            Add( std::bit_cast<uint32_t>( value ) );
        }
        void AddRange( const SubresourceRange& range ) noexcept
        {
            Add( ( static_cast<uint64_t>( range.BaseMip ) << 32 ) | range.MipCount );
            Add( ( static_cast<uint64_t>( range.BaseLayer ) << 32 ) | range.LayerCount );
        }
        void AddState( const AccessState& state ) noexcept
        {
            Add( static_cast<uint64_t>( state.Stages ) );
            Add( static_cast<uint64_t>( state.Memory ) );
            Add( static_cast<uint64_t>( state.Layout ) );
        }
        [[nodiscard]] uint64_t Finish() const noexcept
        {
            uint64_t hash = m_Hash;
            hash ^= hash >> 33;
            hash *= kPrime2;
            hash ^= hash >> 29;
            hash *= kPrime3;
            hash ^= hash >> 32;
            return hash;
        }

    private:
        static constexpr uint64_t kPrime1 = 0x9E3779B185EBCA87ull;
        static constexpr uint64_t kPrime2 = 0xC2B2AE3D27D4EB4Full;
        static constexpr uint64_t kPrime3 = 0x165667B19E3779F9ull;
        static constexpr uint64_t kPrime4 = 0x85EBCA77C2B2AE63ull;
        static constexpr uint64_t kPrime5 = 0x27D4EB2F165667C5ull;

        static uint64_t Round( uint64_t value ) noexcept
        {
            return std::rotl( value * kPrime2, 31 ) * kPrime1;
        }

        uint64_t m_Hash = kPrime5;
    };
} // namespace Desert::Graphic::RDG
