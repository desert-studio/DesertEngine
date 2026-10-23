#include "ReflectedFieldText.hpp"

#include <Editor/Core/AssetPickerRows.hpp>
#include <Editor/Core/CommandHistory.hpp>
#include <Engine/Reflection/FieldValue.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <vector>

namespace Desert::Editor
{
    namespace
    {
        using Reflection::FieldInfo;
        using Reflection::FieldType;
        using Reflection::TypeInfo;
        using Reflection::Value;

        const TypeInfo* StructOf( const FieldInfo& field )
        {
            return field.StructType != nullptr ? field.StructType
                                               : Reflection::ReflectionRegistry::Get().Find( field.TypeName );
        }

        std::string EnumName( const FieldInfo& field, std::int64_t bits )
        {
            for ( const Reflection::EnumValue& e : field.EnumValues )
                if ( e.Value == bits )
                    return e.Name;
            return std::to_string( bits );
        }

        std::string ToText( const FieldInfo& field, const Value& value )
        {
            if ( const bool* b = value.Get<bool>() )
                return *b ? "true" : "false";
            if ( const std::int64_t* i = value.Get<std::int64_t>() )
                return std::to_string( *i );
            if ( const std::uint64_t* u = value.Get<std::uint64_t>() )
                return std::to_string( *u );
            if ( const float* f = value.Get<float>() )
                return std::format( "{}", *f );
            if ( const double* d = value.Get<double>() )
                return std::format( "{}", *d );
            if ( const std::string* s = value.Get<std::string>() )
                return *s;
            if ( const Value::EnumBits* e = value.Get<Value::EnumBits>() )
                return EnumName( field, e->Bits );
            const auto join = []( const auto& components )
            {
                std::string out;
                for ( const float c : components )
                    out += ( out.empty() ? "" : "," ) + std::format( "{}", c );
                return out;
            };
            if ( const Value::Float2* v = value.Get<Value::Float2>() )
                return join( *v );
            if ( const Value::Float3* v = value.Get<Value::Float3>() )
                return join( *v );
            if ( const Value::Float4* v = value.Get<Value::Float4>() )
                return join( *v );
            return {};
        }

        void Describe( const TypeInfo& type, const char* object, const std::string& prefix,
                       std::vector<ReflectedFieldRow>& rows )
        {
            for ( const FieldInfo& field : type.Fields )
            {
                const std::string path = prefix + "." + field.Name;
                if ( field.Type == FieldType::Struct && !field.IsContainer )
                {
                    if ( const TypeInfo* inner = StructOf( field ) )
                        Describe( *inner, object + field.Offset, path, rows );
                    continue;
                }
                const auto read = Reflection::ReadField( field, object );
                if ( !read )
                    continue; // no Value form (container, entity, ...): ReadField's own refusal, not a row
                rows.push_back( { path, std::string( Reflection::FieldTypeName( field.Type ) ),
                                  ToText( field, read.GetValue() ) } );
            }
        }

        template <typename X>
        bool ParseInteger( std::string_view text, X& out )
        {
            const char* end     = text.data() + text.size();
            const auto [at, ec] = std::from_chars( text.data(), end, out );
            return ec == std::errc{} && at == end;
        }

        bool ParseDouble( std::string_view text, double& out )
        {
            const std::string owned( text );
            if ( owned.empty() )
                return false;
            char* end = nullptr;
            out       = std::strtod( owned.c_str(), &end );
            return end == owned.c_str() + owned.size();
        }

        // AN ASSET REFERENCE SPELLED AS ITS PATH (UE: a soft object path is the text form of TSoftObjectPtr<T>,
        // and both the Details panel and the console take it). The slot's content kind is the census's answer
        // for its Asset<T> (ContentKindForAssetType - every kind, meshes, materials and textures included, not a
        // list of some), the row is ContentRegistry::RowAtPath's, as the Details pickers resolve a dropped file.
        // A path of another kind, or one no row answers for, is refused by name: an empty handle written in its
        // place would be a reference silently lost.
        Common::ResultStr<Value> ParseAssetPath( const FieldInfo& field, std::string_view text )
        {
            const std::size_t colon = text.find( ':' );
            if ( colon == std::string_view::npos || colon == 0 || colon + 1 == text.size() )
                return Common::MakeError<Value>(
                     std::format( "'{}' is neither a handle (an unsigned decimal integer) nor a content path "
                                  "(\"assets:<path>\") (field '{}', Asset<{}>)",
                                  text, field.Name, field.Meta.AssetType ) );
            const auto want = ContentKindForAssetType( field.Meta.AssetType );
            if ( !want )
                return Common::MakeError<Value>(
                     std::format( "field '{}' is Asset<{}>, which is no content-registry kind (not registry "
                                  "content), so a path cannot name it here; give the asset's handle",
                                  field.Name, field.Meta.AssetType ) );
            const std::string_view      noun = Common::Content::KindName( *want );
            const std::filesystem::path path{ std::string( text ) };
            if ( const auto row = Assets::ContentRegistry::RowAtPath( *want, path ) )
                return Common::MakeSuccess( Value::UInt( static_cast<std::uint64_t>( row->Handle ) ) );
            if ( const auto kind = Assets::ContentRegistry::KindForFile( path ); kind && *kind != *want )
                return Common::MakeError<Value>(
                     std::format( "'{}' is a {}, not a {}: field '{}' takes only Asset<{}>", text,
                                  Common::Content::KindName( *kind ), noun, field.Name, field.Meta.AssetType ) );
            return Common::MakeError<Value>(
                 std::format( "no {} at '{}' in the content registry (field '{}', Asset<{}>); the file is "
                              "missing, or was added after the editor gathered the content roots",
                              noun, text, field.Name, field.Meta.AssetType ) );
        }

        Common::ResultStr<Value> Parse( const FieldInfo& field, std::string_view text )
        {
            const auto refuse = [&]( std::string_view shape )
            {
                return Common::MakeError<Value>( std::format( "'{}' is not {} (field '{}', {})", text, shape,
                                                              field.Name,
                                                              Reflection::FieldTypeName( field.Type ) ) );
            };
            switch ( field.Type )
            {
                case FieldType::Bool:
                    if ( text == "true" || text == "1" )
                        return Common::MakeSuccess( Value::Bool( true ) );
                    if ( text == "false" || text == "0" )
                        return Common::MakeSuccess( Value::Bool( false ) );
                    return refuse( "true or false" );
                case FieldType::Enum:
                {
                    for ( const Reflection::EnumValue& e : field.EnumValues )
                        if ( e.Name == text )
                            return Common::MakeSuccess( Value::Enum( e.Value ) );
                    std::int64_t bits = 0;
                    if ( ParseInteger( text, bits ) )
                        return Common::MakeSuccess( Value::Enum( bits ) );
                    std::string names;
                    for ( const Reflection::EnumValue& e : field.EnumValues )
                        names += ( names.empty() ? "" : ", " ) + e.Name;
                    return refuse( std::format( "an enumerator ({})", names ) );
                }
                case FieldType::Int:
                {
                    std::int64_t v = 0;
                    if ( !ParseInteger( text, v ) )
                        return refuse( "a decimal integer" );
                    return Common::MakeSuccess( Value::Int( v ) );
                }
                case FieldType::AssetHandle:
                {
                    std::uint64_t v = 0;
                    if ( ParseInteger( text, v ) )
                        return Common::MakeSuccess( Value::UInt( v ) );
                    return ParseAssetPath( field, text );
                }
                case FieldType::UInt:
                {
                    std::uint64_t v = 0;
                    if ( !ParseInteger( text, v ) )
                        return refuse( "an unsigned decimal integer" );
                    return Common::MakeSuccess( Value::UInt( v ) );
                }
                case FieldType::Float:
                case FieldType::Double:
                {
                    double v = 0.0;
                    if ( !ParseDouble( text, v ) )
                        return refuse( "a number" );
                    return Common::MakeSuccess( field.Type == FieldType::Float
                                                     ? Value::Float( static_cast<float>( v ) )
                                                     : Value::Double( v ) );
                }
                case FieldType::String:
                    return Common::MakeSuccess( Value::String( std::string( text ) ) );
                case FieldType::Vec2:
                case FieldType::Vec3:
                case FieldType::Vec4:
                {
                    std::vector<float> parts;
                    std::string_view   rest = text;
                    while ( true )
                    {
                        const std::size_t comma = rest.find( ',' );
                        double            v     = 0.0;
                        if ( !ParseDouble( rest.substr( 0, comma ), v ) )
                            return refuse( "comma-separated numbers" );
                        parts.push_back( static_cast<float>( v ) );
                        if ( comma == std::string_view::npos )
                            break;
                        rest = rest.substr( comma + 1 );
                    }
                    const std::size_t want = field.Type == FieldType::Vec2   ? 2
                                             : field.Type == FieldType::Vec3 ? 3
                                                                             : 4;
                    if ( parts.size() != want )
                        return refuse( std::format( "{} comma-separated numbers", want ) );
                    if ( want == 2 )
                        return Common::MakeSuccess( Value::Vec2( { parts[0], parts[1] } ) );
                    if ( want == 3 )
                        return Common::MakeSuccess( Value::Vec3( { parts[0], parts[1], parts[2] } ) );
                    return Common::MakeSuccess( Value::Vec4( { parts[0], parts[1], parts[2], parts[3] } ) );
                }
                default:
                    break;
            }
            return Common::MakeError<Value>( std::format( "field '{}' is {}: it has no text form", field.Name,
                                                          Reflection::FieldTypeName( field.Type ) ) );
        }
    } // namespace

    std::vector<ReflectedFieldRow> DescribeReflectedFields( const std::string& typeName, const void* object )
    {
        std::vector<ReflectedFieldRow> rows;
        if ( const TypeInfo* type = Reflection::ReflectionRegistry::Get().Find( typeName ); type && object )
            Describe( *type, static_cast<const char*>( object ), typeName, rows );
        return rows;
    }

    Common::BoolResultStr SetReflectedFieldFromText( void* object, const std::string& typeName,
                                                     std::string_view fieldPath, std::string_view text,
                                                     CommandHistory& history )
    {
        const TypeInfo* type = Reflection::ReflectionRegistry::Get().Find( typeName );
        if ( type == nullptr )
            return Common::MakeError<bool>( std::format( "'{}' is not a reflected type", typeName ) );

        char*            at   = static_cast<char*>( object );
        std::string_view rest = fieldPath;
        while ( true )
        {
            const std::size_t      dot     = rest.find( '.' );
            const std::string_view segment = rest.substr( 0, dot );
            const FieldInfo*       field   = nullptr;
            for ( const FieldInfo& candidate : type->Fields )
                if ( candidate.Name == segment )
                    field = &candidate;
            if ( field == nullptr )
            {
                std::string names;
                for ( const FieldInfo& candidate : type->Fields )
                    names += ( names.empty() ? "" : ", " ) + candidate.Name;
                return Common::MakeError<bool>(
                     std::format( "'{}' has no field '{}'. Its fields: {}", type->Name, segment, names ) );
            }

            if ( dot != std::string_view::npos )
            {
                const TypeInfo* inner = field->Type == FieldType::Struct ? StructOf( *field ) : nullptr;
                if ( inner == nullptr || field->IsContainer )
                    return Common::MakeError<bool>(
                         std::format( "'{}.{}' is not a struct, so '{}' addresses nothing inside it", type->Name,
                                      field->Name, rest.substr( dot + 1 ) ) );
                at += field->Offset;
                type = inner;
                rest = rest.substr( dot + 1 );
                continue;
            }

            const auto parsed = Parse( *field, text );
            if ( !parsed )
                return Common::MakeError<bool>( parsed.GetError() );

            void* p = at + field->Offset;
            if ( field->Type == FieldType::String )
            {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — a live std::string member
                std::string before = *reinterpret_cast<std::string*>( p );
                if ( auto written = Reflection::WriteField( *field, at, parsed.GetValue() ); !written )
                    return written;
                history.PushString( static_cast<std::string*>( p ), std::move( before ),
                                    *static_cast<const std::string*>( p ) );
                return Common::MakeSuccess( true );
            }

            std::vector<std::uint8_t> before( field->Size );
            std::memcpy( before.data(), p, field->Size );
            if ( auto written = Reflection::WriteField( *field, at, parsed.GetValue() ); !written )
                return written;
            if ( std::memcmp( before.data(), p, field->Size ) != 0 )
                history.Push( p, before.data(), p, field->Size );
            return Common::MakeSuccess( true );
        }
    }
} // namespace Desert::Editor
