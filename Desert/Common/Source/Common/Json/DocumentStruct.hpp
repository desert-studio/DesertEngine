#pragma once

#include <Common/Json/Document.hpp>

#include <rflcpp/rfl/Processors.hpp>
#include <rflcpp/rfl/generic/Parser.hpp>
#include <rflcpp/rfl/to_generic.hpp>

#include <type_traits>

// STRUCT <-> VALUE TREE (BUILD-MOD-4): the half of Document.hpp that needs reflect-cpp's whole parser. A file that
// reads a struct out of a Node (As, AsBlock, ReadInto/ReadValue of a struct) or turns one into a Value
// (FromStruct, ObjectBuilder::Set of a struct) includes this; a file that only walks nodes and scalars does not
// pay for the parser. Document.hpp declares FromValue/FromStruct with a deduced return type, so a missing include
// is a compile error at the call, not a link error.
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
    } // namespace Detail

    template <typename T>
    [[nodiscard]] auto FromStruct( const T& value )
    {
        return Value( rfl::to_generic( value ) );
    }
} // namespace Common::Json
