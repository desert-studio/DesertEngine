#include <Engine/Reflection/FieldValue.hpp>

#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <utility>

namespace Desert::Reflection
{
    namespace
    {
        template <typename X>
        X Load( const void* at )
        {
            X out{};
            std::memcpy( &out, at, sizeof( X ) );
            return out;
        }

        template <typename X>
        void Store( void* at, X value )
        {
            std::memcpy( at, &value, sizeof( X ) );
        }

        std::string Where( const FieldInfo& field )
        {
            return std::format( "field '{}' ({})", field.Name, field.TypeName );
        }

        Common::ResultStr<std::int64_t> LoadSigned( const FieldInfo& field, const void* at )
        {
            switch ( field.Size )
            {
                case 1:
                    return Common::MakeSuccess( static_cast<std::int64_t>( Load<std::int8_t>( at ) ) );
                case 2:
                    return Common::MakeSuccess( static_cast<std::int64_t>( Load<std::int16_t>( at ) ) );
                case 4:
                    return Common::MakeSuccess( static_cast<std::int64_t>( Load<std::int32_t>( at ) ) );
                case 8:
                    return Common::MakeSuccess( Load<std::int64_t>( at ) );
                default:
                    return Common::MakeError<std::int64_t>(
                         std::format( "{} is {} bytes wide: not an integer width", Where( field ), field.Size ) );
            }
        }

        Common::ResultStr<std::uint64_t> LoadUnsigned( const FieldInfo& field, const void* at )
        {
            switch ( field.Size )
            {
                case 1:
                    return Common::MakeSuccess( static_cast<std::uint64_t>( Load<std::uint8_t>( at ) ) );
                case 2:
                    return Common::MakeSuccess( static_cast<std::uint64_t>( Load<std::uint16_t>( at ) ) );
                case 4:
                    return Common::MakeSuccess( static_cast<std::uint64_t>( Load<std::uint32_t>( at ) ) );
                case 8:
                    return Common::MakeSuccess( Load<std::uint64_t>( at ) );
                default:
                    return Common::MakeError<std::uint64_t>(
                         std::format( "{} is {} bytes wide: not an integer width", Where( field ), field.Size ) );
            }
        }

        template <typename Narrow>
        Common::BoolResultStr StoreIfFits( const FieldInfo& field, void* at, auto wide )
        {
            if ( !std::in_range<Narrow>( wide ) )
                return Common::MakeError<bool>(
                     std::format( "{} cannot hold {}: it is {} bytes wide", Where( field ), wide, field.Size ) );
            Store<Narrow>( at, static_cast<Narrow>( wide ) );
            return Common::MakeSuccess( true );
        }

        Common::BoolResultStr StoreSigned( const FieldInfo& field, void* at, std::int64_t wide )
        {
            switch ( field.Size )
            {
                case 1:
                    return StoreIfFits<std::int8_t>( field, at, wide );
                case 2:
                    return StoreIfFits<std::int16_t>( field, at, wide );
                case 4:
                    return StoreIfFits<std::int32_t>( field, at, wide );
                case 8:
                    Store<std::int64_t>( at, wide );
                    return Common::MakeSuccess( true );
                default:
                    return Common::MakeError<bool>(
                         std::format( "{} is {} bytes wide: not an integer width", Where( field ), field.Size ) );
            }
        }

        Common::BoolResultStr StoreUnsigned( const FieldInfo& field, void* at, std::uint64_t wide )
        {
            switch ( field.Size )
            {
                case 1:
                    return StoreIfFits<std::uint8_t>( field, at, wide );
                case 2:
                    return StoreIfFits<std::uint16_t>( field, at, wide );
                case 4:
                    return StoreIfFits<std::uint32_t>( field, at, wide );
                case 8:
                    Store<std::uint64_t>( at, wide );
                    return Common::MakeSuccess( true );
                default:
                    return Common::MakeError<bool>(
                         std::format( "{} is {} bytes wide: not an integer width", Where( field ), field.Size ) );
            }
        }

        template <typename Array>
        Common::ResultStr<Value> LoadFloats( const FieldInfo& field, const void* at, Value ( *make )( Array ) )
        {
            if ( field.Size != sizeof( Array ) )
                return Common::MakeError<Value>(
                     std::format( "{} is {} bytes: not {} floats", Where( field ), field.Size, Array{}.size() ) );
            return Common::MakeSuccess( make( Load<Array>( at ) ) );
        }

        template <typename Array>
        Common::BoolResultStr StoreFloats( const FieldInfo& field, void* at, const Array* value )
        {
            if ( field.Size != sizeof( Array ) )
                return Common::MakeError<bool>(
                     std::format( "{} is {} bytes: not {} floats", Where( field ), field.Size, Array{}.size() ) );
            Store<Array>( at, *value );
            return Common::MakeSuccess( true );
        }
    } // namespace

    Common::ResultStr<Value> ReadField( const FieldInfo& field, const void* object )
    {
        const char* at = static_cast<const char*>( object ) + field.Offset;
        if ( field.IsContainer )
            return Common::MakeError<Value>(
                 std::format( "{} is a container: it has no Value form", Where( field ) ) );

        switch ( field.Type )
        {
            case FieldType::Bool:
                return Common::MakeSuccess( Value::Bool( Load<bool>( at ) ) );
            case FieldType::Int:
            {
                Common::ResultStr<std::int64_t> const bits = LoadSigned( field, at );
                if ( !bits.IsSuccess() )
                    return Common::MakeError<Value>( bits.GetError() );
                return Common::MakeSuccess( Value::Int( bits.GetValue() ) );
            }
            case FieldType::Enum:
            {
                Common::ResultStr<std::int64_t> const bits = LoadSigned( field, at );
                if ( !bits.IsSuccess() )
                    return Common::MakeError<Value>( bits.GetError() );
                return Common::MakeSuccess( Value::Enum( bits.GetValue() ) );
            }
            case FieldType::UInt:
            {
                Common::ResultStr<std::uint64_t> const bits = LoadUnsigned( field, at );
                if ( !bits.IsSuccess() )
                    return Common::MakeError<Value>( bits.GetError() );
                return Common::MakeSuccess( Value::UInt( bits.GetValue() ) );
            }
            case FieldType::Float:
                return Common::MakeSuccess( Value::Float( Load<float>( at ) ) );
            case FieldType::Double:
                return Common::MakeSuccess( Value::Double( Load<double>( at ) ) );
            case FieldType::String:
                return Common::MakeSuccess(
                     Value::String( *static_cast<const std::string*>( static_cast<const void*>( at ) ) ) );
            case FieldType::Vec2:
                return LoadFloats<Value::Float2>( field, at, &Value::Vec2 );
            case FieldType::Vec3:
                return LoadFloats<Value::Float3>( field, at, &Value::Vec3 );
            case FieldType::Vec4:
                return LoadFloats<Value::Float4>( field, at, &Value::Vec4 );
            case FieldType::AssetHandle:
            {
                // The 64-bit handle id as a UInt Value — the form the serializer stores and a container element
                // of handles travels as (ContainerAccess.hpp).
                Common::ResultStr<std::uint64_t> const bits = LoadUnsigned( field, at );
                if ( !bits.IsSuccess() )
                    return Common::MakeError<Value>( bits.GetError() );
                return Common::MakeSuccess( Value::UInt( bits.GetValue() ) );
            }
            case FieldType::Unknown:
            case FieldType::Struct:
                break;
        }
        return Common::MakeError<Value>(
             std::format( "{} is {}: it has no Value form", Where( field ), FieldTypeName( field.Type ) ) );
    }

    Common::BoolResultStr WriteField( const FieldInfo& field, void* object, const Value& value )
    {
        if ( field.Meta.ReadOnly )
            return Common::MakeError<bool>( std::format( "{} is read-only", Where( field ) ) );
        if ( field.IsContainer )
            return Common::MakeError<bool>(
                 std::format( "{} is a container: it has no Value form", Where( field ) ) );
        // An asset handle is written as its UInt id (the form ReadField gives it).
        const FieldType carried = field.Type == FieldType::AssetHandle ? FieldType::UInt : field.Type;
        if ( value.Type() != carried )
            return Common::MakeError<bool>( std::format( "{} is {}, got {}", Where( field ),
                                                         FieldTypeName( field.Type ),
                                                         FieldTypeName( value.Type() ) ) );

        char* at = static_cast<char*>( object ) + field.Offset;
        switch ( field.Type )
        {
            case FieldType::Bool:
                Store<bool>( at, *value.Get<bool>() );
                return Common::MakeSuccess( true );
            case FieldType::Int:
                return StoreSigned( field, at, *value.Get<std::int64_t>() );
            case FieldType::Enum:
                return StoreSigned( field, at, value.Get<Value::EnumBits>()->Bits );
            case FieldType::UInt:
                return StoreUnsigned( field, at, *value.Get<std::uint64_t>() );
            case FieldType::Float:
                Store<float>( at, *value.Get<float>() );
                return Common::MakeSuccess( true );
            case FieldType::Double:
                Store<double>( at, *value.Get<double>() );
                return Common::MakeSuccess( true );
            case FieldType::String:
                *static_cast<std::string*>( static_cast<void*>( at ) ) = *value.Get<std::string>();
                return Common::MakeSuccess( true );
            case FieldType::Vec2:
                return StoreFloats( field, at, value.Get<Value::Float2>() );
            case FieldType::Vec3:
                return StoreFloats( field, at, value.Get<Value::Float3>() );
            case FieldType::Vec4:
                return StoreFloats( field, at, value.Get<Value::Float4>() );
            case FieldType::AssetHandle:
                return StoreUnsigned( field, at, *value.Get<std::uint64_t>() );
            case FieldType::Unknown:
            case FieldType::Struct:
                break;
        }
        return Common::MakeError<bool>(
             std::format( "{} is {}: it has no Value form", Where( field ), FieldTypeName( field.Type ) ) );
    }
} // namespace Desert::Reflection
