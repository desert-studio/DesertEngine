#pragma once

#include <Common/Json/Document.hpp>

#include <rflcpp/rfl/Processors.hpp>
#include <rflcpp/rfl/enums.hpp>
#include <rflcpp/rfl/fields.hpp>
#include <rflcpp/rfl/generic/Parser.hpp>
#include <rflcpp/rfl/to_generic.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// THE REFLECT-CPP BRIDGE OF Document.hpp (BUILD-MOD-4, STUDIO-AUDIT B5): every part of the document facade that
// needs reflect-cpp's compile-time reflection — the struct parser (As, AsBlock, ReadInto/ReadValue of a struct,
// FromStruct, ObjectBuilder::Set of a struct), enum names (ReadValue/Set of an enum) and member lists
// (MemberNames). Document.hpp declares each of them with a deduced return type, so a file that reads or writes a
// struct or an enum without including this header is a compile error naming the function at the call, never a
// link error; a file that only walks nodes and scalars does not pay for the parser, enum scanning or glm.
// Budget: scripts/Dev/header_fanout_budget.txt (python scripts/Dev/arch_census.py --rule header-fanout).
namespace Common::Json
{
    namespace Detail
    {
        // A JSON NUMBER IS A NUMBER, WHATEVER IT IS SPELT AS (SCENE-INT-TRANSFORM). The value tree keeps an
        // integer as int64 and a real as double, exactly as the text spelt it, and reflect-cpp's own generic
        // reader hands a float member only the double arm: `[1, 0, 0, 0, ...]` in a transform was refused as
        // "Could not cast the underlying value to a double", and the wrong-type rule then dropped the WHOLE
        // block — FIELD-BARN-2 lost every instance of an instanced mesh to one integer. The text reader
        // (yyjson_is_num) and Node::AsNumber both already took either spelling; this was the one path that did
        // not. UE reads the same way (FJsonValue::TryGetNumber answers for an integer and a real alike).
        //
        // This is the reader every Value -> struct read goes through (As, AsBlock and so every ReadValue of a
        // struct), so every floating-point member of every mirror struct takes an integer, not one field. An
        // integer member still refuses a real: 2.5 is not an integer, and that is not this rule's to decide.
        struct ValueReader : rfl::generic::Reader
        {
            template <class T>
            rfl::Result<T> to_basic_type( const InputVarType& var ) const noexcept
            {
                if constexpr ( std::is_floating_point_v<std::remove_cvref_t<T>> )
                {
                    if ( const auto integer = var.to_int64() )
                        return static_cast<T>( *integer );
                }
                return rfl::generic::Reader::template to_basic_type<T>( var );
            }
        };

        // rfl::from_generic with ValueReader in place of reflect-cpp's generic reader.
        template <class T, class... Ps>
        auto FromValue( const Value& value )
        {
            using Parser = rfl::parsing::Parser<ValueReader, rfl::generic::Writer, T, rfl::Processors<Ps...>>;
            return Parser::read( ValueReader{}, value );
        }

        template <typename E>
        auto EnumName( E value )
        {
            return std::string( rfl::enum_to_string( value ) );
        }

        template <typename E>
        auto EnumFromName( std::string_view name )
        {
            const auto parsed = rfl::string_to_enum<E>( std::string( name ) );
            return parsed ? std::optional<E>( parsed.value() ) : std::optional<E>();
        }
    } // namespace Detail

    template <typename T>
    auto FromStruct( const T& value )
    {
        return Value( rfl::to_generic( value ) );
    }

    template <typename T>
    const auto& MemberNames()
    {
        static const std::vector<std::string> names = []
        {
            std::vector<std::string> out;
            for ( const auto& field : rfl::fields<T>() )
                out.push_back( field.name() );
            return out;
        }();
        return names;
    }
} // namespace Common::Json
