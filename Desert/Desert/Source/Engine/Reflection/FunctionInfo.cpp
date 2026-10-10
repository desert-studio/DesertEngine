#include <Engine/Reflection/ReflectionTypes.hpp>
#include <Engine/Reflection/Value.hpp>

#include <format>
#include <vector>

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
            case FieldType::Entity:
                return "Entity";
            case FieldType::Any:
                return "Any";
            case FieldType::Array:
                return "Array";
            case FieldType::Map:
                return "Map";
            case FieldType::Callable:
                return "Callable";
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
        // Arguments left out at the end take their parameters' defaults (UE's CPP_Default_); a parameter with
        // no default is required, and C++ already guarantees every parameter after a defaulted one has one too.
        std::size_t required = Params.size();
        while ( required > 0 && Params[required - 1].Default != nullptr )
            --required;
        if ( argc < required || argc > Params.size() )
            return Common::MakeError<bool>( std::format(
                 "{}::{} takes {} argument(s), got {}", Owner, Name,
                 required == Params.size() ? std::format( "{}", required )
                                           : std::format( "{} to {}", required, Params.size() ),
                 argc ) );
        std::vector<Value> filled;
        if ( argc < Params.size() )
        {
            filled.assign( args, args + argc );
            for ( std::size_t i = argc; i < Params.size(); ++i )
                filled.push_back( *Params[i].Default );
            args = filled.data();
            argc = filled.size();
        }
        for ( std::size_t i = 0; i < argc; ++i )
            if ( Params[i].Type != FieldType::Any && args[i].Type() != Params[i].Type )
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
