#pragma once

#include <Engine/Reflection/ReflectionTypes.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace Desert::Reflection
{
    /// ONE ARGUMENT OR RESULT OF A REFLECTED FUNCTION, IN NO LANGUAGE'S TERMS.
    ///
    /// The public layer every caller of a FUNCTION(...) goes through: Lua, a future VM, the editor's
    /// "call in editor" button and a test all build Values and read Values, and none of them reaches the
    /// C++ signature. The tag is the property layer's own FieldType, so a parameter and a field of the same
    /// C++ type carry the same category: one vocabulary for "what kind of value is this", not two.
    ///
    /// The construction is by named factory, never by an implicit constructor: `Value( 1 )` would have to
    /// guess Int, UInt, Float or Bool, and a guess here is exactly a type a caller did not mean.
    ///
    /// The vector kinds are spelt as float arrays, not glm types, so the reflection core stays free of
    /// glm (FunctionThunk.hpp converts at the C++ boundary). An enum travels as its integer, tagged apart
    /// from Int so a caller can tell an enumerator slot from a number slot.
    class Value
    {
    public:
        using Float2 = std::array<float, 2>;
        using Float3 = std::array<float, 3>;
        using Float4 = std::array<float, 4>;

        struct EnumBits
        {
            std::int64_t Bits = 0;
        };

        /// No value: FieldType::Unknown. What a void function leaves in no slot, and what an unset slot holds.
        Value() = default;

        static Value Bool( bool v )
        {
            return Value( Storage( std::in_place_type<bool>, v ) );
        }
        static Value Int( std::int64_t v )
        {
            return Value( Storage( std::in_place_type<std::int64_t>, v ) );
        }
        static Value UInt( std::uint64_t v )
        {
            return Value( Storage( std::in_place_type<std::uint64_t>, v ) );
        }
        static Value Float( float v )
        {
            return Value( Storage( std::in_place_type<float>, v ) );
        }
        static Value Double( double v )
        {
            return Value( Storage( std::in_place_type<double>, v ) );
        }
        static Value String( std::string v )
        {
            return Value( Storage( std::in_place_type<std::string>, std::move( v ) ) );
        }
        static Value Vec2( Float2 v )
        {
            return Value( Storage( std::in_place_type<Float2>, v ) );
        }
        static Value Vec3( Float3 v )
        {
            return Value( Storage( std::in_place_type<Float3>, v ) );
        }
        static Value Vec4( Float4 v )
        {
            return Value( Storage( std::in_place_type<Float4>, v ) );
        }
        static Value Enum( std::int64_t bits )
        {
            return Value( Storage( std::in_place_type<EnumBits>, EnumBits{ bits } ) );
        }

        [[nodiscard]] FieldType Type() const
        {
            // In the order of Storage's alternatives; a new alternative without a row here fails to compile.
            static constexpr std::array<FieldType, std::variant_size_v<Storage>> kTypes = {
                 FieldType::Unknown, FieldType::Bool,   FieldType::Int,    FieldType::UInt,
                 FieldType::Float,   FieldType::Double, FieldType::String, FieldType::Vec2,
                 FieldType::Vec3,    FieldType::Vec4,   FieldType::Enum,
            };
            return kTypes[m_Data.index()];
        }

        /// The held value as X, or nullptr when it holds another kind. X is one of the storage types above.
        template <typename X>
        [[nodiscard]] const X* Get() const
        {
            return std::get_if<X>( &m_Data );
        }

    private:
        using Storage = std::variant<std::monostate, bool, std::int64_t, std::uint64_t, float, double, std::string,
                                     Float2, Float3, Float4, EnumBits>;

        explicit Value( Storage data ) : m_Data( std::move( data ) )
        {
        }

        Storage m_Data;
    };

    /// The spelling of a FieldType in a diagnostic ("Float", "Vec3"), so an argument mismatch names both kinds.
    [[nodiscard]] const char* FieldTypeName( FieldType type );
} // namespace Desert::Reflection
