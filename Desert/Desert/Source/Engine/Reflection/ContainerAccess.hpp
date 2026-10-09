#pragma once

// THE CODEGEN'S ELEMENT ACCESSORS OF A std::vector FIELD (FieldInfo::ContainerSize / Resize / Get / Set).
//
// A vector cannot be walked through `Offset` alone — its element type is a compile-time fact — so, exactly as
// for its JSON halves (WriteContainer/ReadContainer in ReflectionSerializer.hpp), DesertHeaderTool instantiates
// these per field with `decltype( T::field )`. An element travels as a Value of its kind through the same
// ValueTraits a FUNCTION(...) parameter does (FunctionThunk.hpp): one C++ <-> Value rule for every caller. A
// 64-bit asset handle is stored as `std::uint64_t` (a UInt Value), as the serializer stores it.

#include <Engine/Reflection/FunctionThunk.hpp>
#include <Engine/Reflection/Value.hpp>

#include <cstddef>
#include <cstdint>

namespace Desert::Reflection
{
    template <typename Vector>
    [[nodiscard]] std::size_t ContainerSize( const void* field )
    {
        return static_cast<const Vector*>( field )->size();
    }

    /// Grows with default-constructed elements, or drops the tail.
    template <typename Vector>
    void ContainerResize( void* field, std::size_t count )
    {
        static_cast<Vector*>( field )->resize( count );
    }

    /// The element at `index` as a Value of the element's kind; past the end, an empty (Unknown) Value.
    template <typename Vector, typename Stored = typename Vector::value_type>
    [[nodiscard]] Value ContainerGet( const void* field, std::size_t index )
    {
        const auto& vector = *static_cast<const Vector*>( field );
        if ( index >= vector.size() )
            return {};
        return ValueTraits<Stored>::To( static_cast<Stored>( vector[index] ) );
    }

    /// Writes the element at `index`. Refused, with nothing written: an index past the end (growing is
    /// ContainerResize — a write never resizes by accident), a Value of another kind (no conversion here:
    /// converting is the caller's language rule), an integer outside the element's range.
    template <typename Vector, typename Stored = typename Vector::value_type>
    [[nodiscard]] bool ContainerSet( void* field, std::size_t index, const Value& value )
    {
        using Traits = ValueTraits<Stored>;
        auto& vector = *static_cast<Vector*>( field );
        if ( index >= vector.size() || value.Type() != Traits::Kind )
            return false;
        if constexpr ( requires { Traits::Fits( value ); } )
        {
            if ( !Traits::Fits( value ) )
                return false;
        }
        vector[index] = typename Vector::value_type( Traits::From( value ) );
        return true;
    }
} // namespace Desert::Reflection
