#include "ReflectionSerializer.hpp"

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/Logger.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

#include <glm/glm.hpp>

namespace Desert::Reflection
{
    namespace
    {
        // The asset types a reflected field stores as {"Guid", "Path"}: skyboxes since SCNE 29, textures
        // (the UI sprites and the splash) since SCNE 30. Every other type still stores its key alone.
        bool IsStoredByGuid( const std::string& assetType )
        {
            return assetType == "SkyboxAsset" || assetType == "TextureAsset";
        }

        void WriteVec( Common::Json::Object& out, const std::string& name, const float* v, int count )
        {
            Common::Json::Value::Array arr;
            for ( int i = 0; i < count; ++i )
                arr.emplace_back( static_cast<double>( v[i] ) );
            out[name] = std::move( arr );
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

        // An enum's stored integer fits the field when ReadIntBySize could have produced it.
        bool FitsIntBySize( std::size_t size, int64_t value )
        {
            switch ( size )
            {
                case 1:
                    return value >= INT8_MIN && value <= INT8_MAX;
                case 2:
                    return value >= INT16_MIN && value <= INT16_MAX;
                case 8:
                    return true;
                default:
                    return value >= INT32_MIN && value <= INT32_MAX;
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

    Common::Json::Object SerializeReflected( const TypeInfo& type, const void* obj, const AssetResolver* resolver )
    {
        Common::Json::Object out;
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
                    if ( resolver != nullptr && IsStoredByGuid( field.Meta.AssetType ) )
                    {
                        // SCNE 29 (skybox) / 30 (texture): the GUID is the identity, the key only locates it.
                        const uint64_t       handle = *static_cast<const uint64_t*>( p );
                        Common::Json::Object ref;
                        ref["Guid"]     = resolver->ToGuid( handle, field.Meta.AssetType );
                        ref["Path"]     = resolver->ToPath( handle, field.Meta.AssetType );
                        out[field.Name] = Common::Json::Value( std::move( ref ) );
                    }
                    else if ( resolver != nullptr && resolver->ToPath )
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

    void DeserializeReflected( const TypeInfo& type, void* obj, const Common::Json::Node& src,
                               Common::Json::Issues& issues, const AssetResolver* resolver )
    {
        using Common::Json::Kind;
        if ( !src.ExpectKind( Kind::Object, issues ) )
            return;
        auto* base = static_cast<std::byte*>( obj );

        for ( const auto& field : type.Fields )
        {
            const auto found = src.Find( field.Name );
            if ( !found )
                continue; // missing key — keep the field's current value

            const Common::Json::Node& g = *found;
            void*                     p = base + field.Offset;

            if ( field.IsContainer && field.DeserializeContainer )
            {
                field.DeserializeContainer( p, g, issues );
                continue;
            }

            switch ( field.Type )
            {
                case FieldType::Bool:
                    g.ReadValue( *static_cast<bool*>( p ), issues );
                    break;
                case FieldType::Int:
                    g.ReadValue( *static_cast<int32_t*>( p ), issues );
                    break;
                case FieldType::UInt:
                    g.ReadValue( *static_cast<uint32_t*>( p ), issues );
                    break;
                case FieldType::Float:
                    g.ReadValue( *static_cast<float*>( p ), issues );
                    break;
                case FieldType::Double:
                    g.ReadValue( *static_cast<double*>( p ), issues );
                    break;
                case FieldType::String:
                    g.ReadValue( *static_cast<std::string*>( p ), issues );
                    break;
                case FieldType::Vec2:
                    g.ReadValue( *static_cast<glm::vec2*>( p ), issues );
                    break;
                case FieldType::Vec3:
                    g.ReadValue( *static_cast<glm::vec3*>( p ), issues );
                    break;
                case FieldType::Vec4:
                    g.ReadValue( *static_cast<glm::vec4*>( p ), issues );
                    break;
                case FieldType::Enum:
                {
                    // Stored as the enumerator's integer, at the width of the field (SerializeReflected).
                    const auto v = g.AsInteger();
                    if ( v && FitsIntBySize( field.Size, v.GetValue() ) )
                        WriteIntBySize( p, field.Size, v.GetValue() );
                    else
                        g.Report( issues,
                                  "integer enumerator of a " + std::to_string( field.Size ) + "-byte enum" );
                    break;
                }
                case FieldType::AssetHandle:
                {
                    // A raw integer is what the writer emits with no resolver, for every type alike, so
                    // it takes the raw-handle route below; only a reference form reaches the GUID branch.
                    const auto integer = g.AsInteger();
                    if ( IsStoredByGuid( field.Meta.AssetType ) && !integer )
                    {
                        const auto text = g.AsString();
                        if ( g.GetKind() == Kind::Object && resolver != nullptr )
                        {
                            std::string       guid;
                            std::string       path;
                            const std::size_t before = issues.size();
                            g.ReadInto( "Guid", guid, issues );
                            g.ReadInto( "Path", path, issues );
                            // A wrong-typed Guid or Path is an Issue already; resolving the half that did
                            // read would name a different reference than the file states.
                            if ( issues.size() == before )
                                *static_cast<uint64_t*>( p ) =
                                     ResolveGuidRef( *resolver, guid, path, field.Meta.AssetType.c_str(),
                                                     "field '" + g.Where().ToString() + "'" );
                        }
                        else if ( g.GetKind() == Kind::Object || ( text && !text.GetValue().empty() ) )
                        {
                            LOG_ERROR( "[Reflection] Field '{0}' is a {1} reference in a form this build does not "
                                       "read (a {{Guid, Path}} object with no resolver, or a pre-SCNE-30 bare "
                                       "string - run the SceneMigrator); the field keeps its default.",
                                       g.Where().ToString(), field.Meta.AssetType );
                        }
                        else if ( !text )
                            g.Report( issues, "{Guid, Path} object or integer handle" );
                        break;
                    }
                    if ( const auto s = g.AsString() )
                    {
                        // A path/key. Without a resolver there is nothing that can turn it into a handle,
                        // and quietly leaving the field at zero is what made a texture slot look like an
                        // empty slot (DC §1.4).
                        if ( resolver && resolver->FromPath )
                            *static_cast<uint64_t*>( p ) =
                                 resolver->FromPath( s.GetValue(), field.Meta.AssetType );
                        else if ( !s.GetValue().empty() )
                            LOG_ERROR( "[Reflection] Field '{0}' names the asset '{1}' but was deserialized "
                                       "with no asset resolver, so the reference cannot be turned into a "
                                       "handle; the field keeps its default.",
                                       g.Where().ToString(), s.GetValue() );
                        break;
                    }

                    // A RAW HANDLE, read as an exact 64-bit integer. It went through `double` until
                    // 2026-09-05, which silently rounded every handle above 2^53 — measured on a live
                    // one, 5355760296319878840 loaded back as 5355760296319879168. The int64 the file
                    // carries is reinterpreted rather than converted, so handles above 2^63 (which the
                    // path hash produces about half the time) survive as well.
                    if ( integer )
                        *static_cast<uint64_t*>( p ) = static_cast<uint64_t>( integer.GetValue() );
                    else
                        g.Report( issues, "asset path or integer handle" );
                    break;
                }
                case FieldType::Struct:
                    if ( field.StructType )
                        DeserializeReflected( *field.StructType, p, g, issues, resolver );
                    break;
                default:
                    break;
            }
        }
    }
} // namespace Desert::Reflection
