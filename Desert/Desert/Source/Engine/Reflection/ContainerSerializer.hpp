#pragma once

// THE CODEGEN'S JSON HALVES OF A std::vector FIELD (FieldInfo::SerializeContainer / DeserializeContainer /
// ContainerHandles / AssignHandles). DesertHeaderTool instantiates them per field with `decltype( T::field )` in
// the generated Reflection_*.gen.cpp (through ContainerAccess.hpp), so the JSON parser they read with
// (Common/Json/Detail/RflBridge.hpp) is compiled there and not in every includer of ReflectionSerializer.hpp.

#include <Common/Json/Document.hpp>
#include <Common/Json/Detail/RflBridge.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace Desert::Reflection
{
    // THE CODEGEN'S HALVES OF A std::vector FIELD (FieldInfo::SerializeContainer / DeserializeContainer).
    // Each element is written as JSON spells `Stored`. A 64-bit handle is stored as `std::uint64_t`, which
    // the value tree keeps in its int64 alternative (reinterpreted, so ids above 2^63 survive) and which
    // ReadContainer reinterprets back the same way.
    template <typename Vector, typename Stored = typename Vector::value_type>
    [[nodiscard]] Common::Json::Value WriteContainer( const void* field )
    {
        Common::Json::Value::Array array;
        for ( const auto& element : *static_cast<const Vector*>( field ) )
            array.push_back( Common::Json::Detail::ToValue( static_cast<Stored>( element ) ) );
        return { std::move( array ) };
    }

    // The handles of a std::vector of asset handles, and the vector rebuilt from handles
    // (FieldInfo::ContainerHandles / AssignHandles): what lets the serializer write each element in its asset
    // type's reference form.
    template <typename Vector>
    [[nodiscard]] std::vector<std::uint64_t> ContainerHandles( const void* field )
    {
        std::vector<std::uint64_t> out;
        for ( const auto& element : *static_cast<const Vector*>( field ) )
            out.push_back( static_cast<std::uint64_t>( element ) );
        return out;
    }

    template <typename Vector>
    void AssignHandles( void* field, const std::vector<std::uint64_t>& handles )
    {
        Vector assigned;
        for ( const std::uint64_t handle : handles )
            assigned.push_back( typename Vector::value_type( handle ) );
        *static_cast<Vector*>( field ) = std::move( assigned );
    }

    // Reads the array back under the wrong-type rule, and a vector is ONE field: any element of the wrong
    // type is an Issue (with its index in the path) and the WHOLE vector keeps its current value — a
    // vector with one element silently missing would shift every later index.
    template <typename Vector, typename Stored = typename Vector::value_type>
    void ReadContainer( void* field, const Common::Json::Node& src, Common::Json::Issues& issues )
    {
        if ( !src.ExpectKind( Common::Json::Kind::Array, issues ) )
            return;
        Vector            read;
        const std::size_t before = issues.size();
        src.ForEachElement(
             [&]( std::size_t, const Common::Json::Node& element )
             {
                 if constexpr ( std::is_same_v<Stored, std::uint64_t> )
                 {
                     std::int64_t raw = 0;
                     element.ReadValue( raw, issues );
                     read.push_back( typename Vector::value_type( static_cast<std::uint64_t>( raw ) ) );
                 }
                 else
                 {
                     Stored value{};
                     element.ReadValue( value, issues );
                     read.push_back( static_cast<typename Vector::value_type>( value ) );
                 }
             } );
        if ( issues.size() == before )
            *static_cast<Vector*>( field ) = std::move( read );
    }
} // namespace Desert::Reflection
