#include "ReflectionSerializer.hpp"

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/Logger.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace Desert::Reflection
{
    namespace
    {
        // Accepts a JSON number stored either as integer or floating point.
        //
        // FOR FLOATING-POINT FIELDS ONLY. It used to serve the integral cases as well, and a 64-bit
        // value does not survive that: `double` has 53 bits of mantissa, so every integer above 2^53
        // (9 007 199 254 740 992) is rounded to the nearest representable neighbour. Measured on a real
        // asset handle, which is a 64-bit FNV-1a hash and therefore lands in that range essentially
        // always: 5355760296319878840 came back as 5355760296319879168, off by 328 — 594 times past the
        // point where doubles stop counting. The corruption was OURS and not the JSON library's:
        // rfl::json writes and reads that same value exactly, including values above 2^63, and
        // rfl::Generic keeps integers in an int64_t alternative that never touches a double.
        double AsNumber( const rfl::Generic& g )
        {
            if ( auto d = g.to_double(); d.has_value() )
                return d.value();
            if ( auto i = g.to_int64(); i.has_value() )
                return static_cast<double>( i.value() );
            return 0.0;
        }

        // The integral counterpart: every bit of a stored integer, or nothing. Returning an optional
        // rather than a 0 default is deliberate — 0 is a MEANINGFUL handle value ("unset"), so a
        // silent 0 on a malformed field is DC §1.4's substitution, and the callers below log instead.
        std::optional<int64_t> AsInteger( const rfl::Generic& g )
        {
            if ( auto i = g.to_int64(); i.has_value() )
                return i.value();
            // A whole number written with a decimal point (`5.0`) is a legitimate JSON spelling of an
            // integer, and the code this replaces accepted it (it asked for a double FIRST), so a file
            // carrying one must keep loading. It is accepted only when the value is integral AND inside
            // int64's range — outside it the conversion is undefined behaviour, and a double that large
            // has already lost the exact integer it claims to be, which is the whole subject here.
            if ( auto d = g.to_double(); d.has_value() )
            {
                const double     v           = d.value();
                constexpr double kUpperBound = 9223372036854775808.0; // 2^63, the first value int64 lacks
                constexpr double kLowerBound = -9223372036854775808.0;
                if ( v >= kLowerBound && v < kUpperBound && v == static_cast<double>( static_cast<int64_t>( v ) ) )
                    return static_cast<int64_t>( v );
            }
            return std::nullopt;
        }

        void WriteVec( rfl::Generic::Object& out, const std::string& name, const float* v, int count )
        {
            rfl::Generic::Array arr;
            for ( int i = 0; i < count; ++i )
                arr.push_back( rfl::Generic( static_cast<double>( v[i] ) ) );
            out[name] = std::move( arr );
        }

        void ReadVec( const rfl::Generic& g, float* v, int count )
        {
            auto arr = g.to_array();
            if ( !arr.has_value() )
                return;
            const auto& a = arr.value();
            for ( int i = 0; i < count && i < static_cast<int>( a.size() ); ++i )
                v[i] = static_cast<float>( AsNumber( a[i] ) );
        }

        // Enums can have any integral underlying type (enum class : uint8_t, etc.). Read/write exactly
        // `size` bytes so we never touch adjacent fields.
        int64_t ReadIntBySize( const void* p, std::size_t size )
        {
            switch ( size )
            {
                case 1:  return *static_cast<const int8_t*>( p );
                case 2:  return *static_cast<const int16_t*>( p );
                case 8:  return *static_cast<const int64_t*>( p );
                default: return *static_cast<const int32_t*>( p );
            }
        }

        void WriteIntBySize( void* p, std::size_t size, int64_t value )
        {
            switch ( size )
            {
                case 1:  *static_cast<int8_t*>( p )  = static_cast<int8_t>( value ); break;
                case 2:  *static_cast<int16_t*>( p ) = static_cast<int16_t>( value ); break;
                case 8:  *static_cast<int64_t*>( p ) = value; break;
                default: *static_cast<int32_t*>( p ) = static_cast<int32_t>( value ); break;
            }
        }
    } // namespace

    rfl::Generic::Object SerializeReflected( const TypeInfo& type, const void* obj, const AssetResolver* resolver )
    {
        rfl::Generic::Object out;
        const auto* base = static_cast<const std::byte*>( obj );

        for ( const auto& field : type.Fields )
        {
            const void* p = base + field.Offset;

            // Containers route through the codegen-emitted typed lambda (the switch can't iterate vectors).
            if ( field.IsContainer && field.SerializeContainer )
            {
                out[field.Name] = field.SerializeContainer( p );
                continue;
            }

            switch ( field.Type )
            {
                case FieldType::Bool:
                    out[field.Name] = *static_cast<const bool*>( p );
                    break;
                case FieldType::Int:
                    out[field.Name] = static_cast<int64_t>( *static_cast<const int32_t*>( p ) );
                    break;
                case FieldType::UInt:
                    out[field.Name] = static_cast<int64_t>( *static_cast<const uint32_t*>( p ) );
                    break;
                case FieldType::Float:
                    out[field.Name] = static_cast<double>( *static_cast<const float*>( p ) );
                    break;
                case FieldType::Double:
                    out[field.Name] = *static_cast<const double*>( p );
                    break;
                case FieldType::String:
                    out[field.Name] = *static_cast<const std::string*>( p );
                    break;
                case FieldType::Vec2:
                    WriteVec( out, field.Name, static_cast<const float*>( p ), 2 );
                    break;
                case FieldType::Vec3:
                    WriteVec( out, field.Name, static_cast<const float*>( p ), 3 );
                    break;
                case FieldType::Vec4:
                    WriteVec( out, field.Name, static_cast<const float*>( p ), 4 );
                    break;
                case FieldType::Enum:
                    out[field.Name] = ReadIntBySize( p, field.Size );
                    break;
                case FieldType::AssetHandle:
                    if ( resolver && field.Meta.AssetType == "SkyboxAsset" )
                    {
                        // SCNE 29: the GUID is the identity, the project key only locates it. ToPath
                        // renders a skybox as a tagged key or not at all, so no absolute path reaches here.
                        const uint64_t       handle = *static_cast<const uint64_t*>( p );
                        rfl::Generic::Object ref;
                        ref["Guid"]     = resolver->ToGuid( handle, field.Meta.AssetType );
                        ref["Path"]     = resolver->ToPath( handle, field.Meta.AssetType );
                        out[field.Name] = std::move( ref );
                    }
                    else if ( resolver && resolver->ToPath )
                        out[field.Name] =
                             resolver->ToPath( *static_cast<const uint64_t*>( p ), field.Meta.AssetType );
                    else
                        out[field.Name] = static_cast<int64_t>( *static_cast<const uint64_t*>( p ) );
                    break;
                case FieldType::Struct:
                    if ( field.StructType )
                        out[field.Name] = SerializeReflected( *field.StructType, p, resolver );
                    break;
                default:
                    break;
            }
        }

        return out;
    }

    uint64_t ResolveGuidRef( const AssetResolver& resolver, const std::string& text, const std::string& path,
                             const char* type, const std::string& what )
    {
        if ( text.empty() )
        {
            if ( !path.empty() )
                LOG_ERROR( "[ComponentRegistry] {0} states no GUID but a path '{1}' - the path is a locator, "
                           "not an identity, so the reference stays empty",
                           what, path );
            return 0;
        }
        const auto guid = Common::Content::AssetGuidFromText( text );
        if ( !guid )
        {
            LOG_ERROR( "[ComponentRegistry] {0} states '{1}', which is not a GUID ({2}) - the reference stays "
                       "empty",
                       what, text, guid.GetError() );
            return 0;
        }
        if ( guid.GetValue().IsNull() )
        {
            LOG_ERROR( "[ComponentRegistry] {0} states the null GUID - the reference stays empty", what );
            return 0;
        }
        const uint64_t expected = static_cast<uint64_t>( Common::Content::HandleForGuid( guid.GetValue() ) );
        if ( const uint64_t known = resolver.FromGuid( expected, type ); known != 0 )
            return known;
        const uint64_t located = path.empty() ? 0 : resolver.FromPath( path, type );
        if ( located == expected )
            return located;
        LOG_ERROR( "[ComponentRegistry] {0} names GUID {1}, but its locator '{2}' {3} - the reference stays "
                   "empty",
                   what, text, path,
                   located == 0 ? std::string( "loads no " ) + type
                                : "holds a different asset (handle " + std::to_string( located ) + ")" );
        return 0;
    }

    void DeserializeReflected( const TypeInfo& type, void* obj, const rfl::Generic::Object& src,
                               const AssetResolver* resolver )
    {
        auto* base = static_cast<std::byte*>( obj );

        for ( const auto& field : type.Fields )
        {
            auto found = src.get( field.Name );
            if ( !found.has_value() )
                continue; // missing key — keep the field's default value

            const rfl::Generic& g = found.value();
            void*               p = base + field.Offset;

            if ( field.IsContainer && field.DeserializeContainer )
            {
                field.DeserializeContainer( p, g );
                continue;
            }

            switch ( field.Type )
            {
                case FieldType::Bool:
                    if ( auto b = g.to_bool(); b.has_value() )
                        *static_cast<bool*>( p ) = b.value();
                    break;
                case FieldType::Int:
                    if ( const auto v = AsInteger( g ) )
                        *static_cast<int32_t*>( p ) = static_cast<int32_t>( *v );
                    break;
                case FieldType::UInt:
                    if ( const auto v = AsInteger( g ) )
                        *static_cast<uint32_t*>( p ) = static_cast<uint32_t>( *v );
                    break;
                case FieldType::Float:
                    *static_cast<float*>( p ) = static_cast<float>( AsNumber( g ) );
                    break;
                case FieldType::Double:
                    *static_cast<double*>( p ) = AsNumber( g );
                    break;
                case FieldType::String:
                    if ( auto s = g.to_string(); s.has_value() )
                        *static_cast<std::string*>( p ) = s.value();
                    break;
                case FieldType::Vec2:
                    ReadVec( g, static_cast<float*>( p ), 2 );
                    break;
                case FieldType::Vec3:
                    ReadVec( g, static_cast<float*>( p ), 3 );
                    break;
                case FieldType::Vec4:
                    ReadVec( g, static_cast<float*>( p ), 4 );
                    break;
                case FieldType::Enum:
                    if ( const auto v = AsInteger( g ) )
                        WriteIntBySize( p, field.Size, *v );
                    break;
                case FieldType::AssetHandle:
                {
                    if ( field.Meta.AssetType == "SkyboxAsset" )
                    {
                        const auto ref = g.to_object();
                        if ( ref.has_value() && resolver )
                        {
                            const auto text = [&]( const char* key )
                            {
                                const auto v = ref.value().get( key );
                                return v.has_value() ? v.value().to_string().value_or( std::string() )
                                                     : std::string();
                            };
                            *static_cast<uint64_t*>( p ) =
                                 ResolveGuidRef( *resolver, text( "Guid" ), text( "Path" ), "SkyboxAsset",
                                                 "field '" + field.Name + "'" );
                        }
                        else if ( ref.has_value() ||
                                  ( g.to_string().has_value() && !g.to_string().value().empty() ) )
                            LOG_ERROR(
                                 "[Reflection] Field '{0}' is a skybox reference in a form this build does not "
                                 "read (a {{Guid, Path}} object with no resolver, or a pre-SCNE-29 bare "
                                 "string - run the SceneMigrator); the field keeps its default.",
                                 field.Name );
                        break;
                    }
                    if ( auto s = g.to_string(); s.has_value() )
                    {
                        // A path/key. Without a resolver there is nothing that can turn it into a handle,
                        // and quietly leaving the field at zero is what made a texture slot look like an
                        // empty slot (DC §1.4).
                        if ( resolver && resolver->FromPath )
                            *static_cast<uint64_t*>( p ) = resolver->FromPath( s.value(), field.Meta.AssetType );
                        else if ( !s.value().empty() )
                            LOG_ERROR( "[Reflection] Field '{0}' names the asset '{1}' but was deserialized "
                                       "with no asset resolver, so the reference cannot be turned into a "
                                       "handle; the field keeps its default.",
                                       field.Name, s.value() );
                        break;
                    }

                    // A RAW HANDLE, read as an exact 64-bit integer. It went through `double` until
                    // 2026-09-05, which silently rounded every handle above 2^53 — measured on a live
                    // one, 5355760296319878840 loaded back as 5355760296319879168. The int64 the file
                    // carries is reinterpreted rather than converted, so handles above 2^63 (which the
                    // path hash produces about half the time) survive as well.
                    if ( const auto v = AsInteger( g ) )
                        *static_cast<uint64_t*>( p ) = static_cast<uint64_t>( *v );
                    else
                        LOG_ERROR( "[Reflection] Field '{0}' holds neither an asset path nor an integer "
                                   "handle; the field keeps its default.",
                                   field.Name );
                    break;
                }
                case FieldType::Struct:
                    if ( field.StructType )
                    {
                        if ( auto o = g.to_object(); o.has_value() )
                            DeserializeReflected( *field.StructType, p, o.value(), resolver );
                    }
                    break;
                default:
                    break;
            }
        }
    }
} // namespace Desert::Reflection
