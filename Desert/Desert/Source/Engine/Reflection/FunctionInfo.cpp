#include <Engine/Reflection/ReflectionTypes.hpp>
#include <Engine/Reflection/Value.hpp>

#include <format>

namespace Desert::Reflection
{
    const char* FieldTypeName( FieldType type )
    {
        switch ( type )
        {
            case FieldType::Unknown:
                return "None";
            case FieldType::Bool:
                return "Bool";
            case FieldType::Int:
                return "Int";
            case FieldType::UInt:
                return "UInt";
            case FieldType::Float:
                return "Float";
            case FieldType::Double:
                return "Double";
            case FieldType::String:
                return "String";
            case FieldType::Vec2:
                return "Vec2";
            case FieldType::Vec3:
                return "Vec3";
            case FieldType::Vec4:
                return "Vec4";
            case FieldType::Enum:
                return "Enum";
            case FieldType::Struct:
                return "Struct";
            case FieldType::AssetHandle:
                return "AssetHandle";
        }
        return "Unknown";
    }

    Common::BoolResultStr FunctionInfo::Invoke( void* self, const Value* args, std::size_t argc,
                                                Value* rets ) const
    {
        if ( Thunk == nullptr )
            return Common::MakeError<bool>( std::format( "{}::{} has no thunk", Owner, Name ) );
        if ( IsStatic && self != nullptr )
            return Common::MakeError<bool>( std::format( "{}::{} is static: it takes no instance", Owner, Name ) );
        if ( !IsStatic && self == nullptr )
            return Common::MakeError<bool>( std::format( "{}::{} needs an instance of {}", Owner, Name, Owner ) );
        if ( argc != Params.size() )
            return Common::MakeError<bool>(
                 std::format( "{}::{} takes {} argument(s), got {}", Owner, Name, Params.size(), argc ) );
        for ( std::size_t i = 0; i < argc; ++i )
            if ( args[i].Type() != Params[i].Type )
                return Common::MakeError<bool>(
                     std::format( "{}::{} argument {} ('{}') is {}, got {}", Owner, Name, i, Params[i].Name,
                                  FieldTypeName( Params[i].Type ), FieldTypeName( args[i].Type() ) ) );
        if ( !Returns.empty() && rets == nullptr )
            return Common::MakeError<bool>(
                 std::format( "{}::{} returns a value: rets has no room", Owner, Name ) );

        if ( Common::BoolResultStr called = Thunk( self, args, rets ); !called.IsSuccess() )
            return Common::MakeError<bool>( std::format( "{}::{}: {}", Owner, Name, called.GetError() ) );
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Reflection
