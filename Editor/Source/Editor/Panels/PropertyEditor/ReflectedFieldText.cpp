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
#include <memory>
#include <optional>
#include <system_error>
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
        // One element of a std::vector field, restored through the field's typed accessors (FieldInfo::ContainerSet):
        // an element has no address the reflection hands out, and its value - not its bytes - is what survives.
        class ContainerElementCommand final : public ICommand
        {
        public:
            ContainerElementCommand( const FieldInfo& field, void* container, std::size_t index, Value before,
                                     Value after )
                 : m_Set( field.ContainerSet ), m_Container( container ), m_Index( index ),
                   m_Before( std::move( before ) ), m_After( std::move( after ) ), m_Label( field.DisplayName() )
            {
            }
            bool Undo() override
            {
                return m_Set( m_Container, m_Index, m_Before );
            }
            bool Redo() override
            {
                return m_Set( m_Container, m_Index, m_After );
            }
            bool IsVolatile() const override
            {
                return true; // a raw address inside a component, as ByteCommand's
            }
            [[nodiscard]] const void* EditedObject() const override
            {
                return m_Container;
            }
            std::string GetLabel() const override
            {
                return "Edit " + m_Label;
            }

        private:
            bool ( *m_Set )( void*, std::size_t, const Value& );
            void*       m_Container;
            std::size_t m_Index;
            Value       m_Before;
            Value       m_After;
            std::string m_Label;
        };

        // A TEXT EDIT OF AN OWNED OBJECT (an entity's component), undone and redone by VALUE through the owner's
        // write path (ReflectedEditRoute: the component entry's EditData, entity + type), never through an
        // address: EnTT moves a component when its pool changes, and a component with one writer
        // (StaticMeshComponent -> ECS::SetStaticMesh) is only changed through that writer, so a byte copy into
        // its storage would both risk a dead address and skip the writer's on_update.
        class RoutedFieldCommand final : public ICommand
        {
        public:
            RoutedFieldCommand( ReflectedEditRoute route, std::size_t ownerOffset, const FieldInfo& field,
                                std::optional<std::size_t> index, Value before, Value after )
                 : m_Route( std::move( route ) ), m_OwnerOffset( ownerOffset ), m_Field( field ), m_Index( index ),
                   m_Before( std::move( before ) ), m_After( std::move( after ) )
            {
            }
            bool Undo() override
            {
                return Apply( m_Before );
            }
            bool Redo() override
            {
                return Apply( m_After );
            }
            std::string GetLabel() const override
            {
                return "Edit " + m_Field.DisplayName();
            }

        private:
            bool Apply( const Value& value )
            {
                bool written = false;
                const bool routed = m_Route(
                     [&]( void* object )
                     {
                         char* owner = static_cast<char*>( object ) + m_OwnerOffset;
                         if ( m_Index.has_value() )
                             written = m_Field.ContainerSet( owner + m_Field.Offset, *m_Index, value );
                         else
                             written = static_cast<bool>( Reflection::WriteField( m_Field, owner, value ) );
                     } );
                return routed && written;
            }

            ReflectedEditRoute         m_Route;
            std::size_t                m_OwnerOffset;
            FieldInfo                  m_Field;
            std::optional<std::size_t> m_Index;
            Value                      m_Before;
            Value                      m_After;
        };

        // `Name[index]` -> {Name, index}; a segment without brackets has no index.
        struct SegmentIndex
        {
            std::string_view           Name;
            std::optional<std::size_t> Index;
            bool                       Malformed = false;
        };
        SegmentIndex SplitIndex( std::string_view segment )
        {
            const std::size_t open = segment.find( '[' );
            if ( open == std::string_view::npos )
                return { segment, std::nullopt, false };
            if ( segment.back() != ']' )
                return { segment.substr( 0, open ), std::nullopt, true };
            std::size_t            index  = 0;
            const std::string_view digits = segment.substr( open + 1, segment.size() - open - 2 );
            const auto [end, error] = std::from_chars( digits.data(), digits.data() + digits.size(), index );
            if ( digits.empty() || error != std::errc() || end != digits.data() + digits.size() )
                return { segment.substr( 0, open ), std::nullopt, true };
            return { segment.substr( 0, open ), index, false };
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
                                                     CommandHistory& history, const ReflectedEditRoute& route )
    {
        const TypeInfo* type = Reflection::ReflectionRegistry::Get().Find( typeName );
        if ( type == nullptr )
            return Common::MakeError<bool>( std::format( "'{}' is not a reflected type", typeName ) );

        char*            at   = static_cast<char*>( object );
        std::string_view rest = fieldPath;
        while ( true )
        {
            const std::size_t      dot     = rest.find( '.' );
            const SegmentIndex     split   = SplitIndex( rest.substr( 0, dot ) );
            const std::string_view segment = split.Name;
            if ( split.Malformed )
                return Common::MakeError<bool>( std::format(
                     "'{}' is not <Field>[<index>] with a decimal index", rest.substr( 0, dot ) ) );
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

            if ( split.Index.has_value() )
            {
                // ONE ELEMENT OF A std::vector FIELD (StaticMesh.MaterialSlots[0]): parsed as the element's kind
                // (an asset path checked against the field's Asset<T>), written through ContainerSet, which
                // refuses an index past the end - an element is addressed, never appended.
                if ( !field->IsContainer || field->ContainerSet == nullptr || field->ContainerGet == nullptr )
                    return Common::MakeError<bool>(
                         std::format( "'{}.{}' is not a list, so [{}] addresses nothing", type->Name, field->Name,
                                      *split.Index ) );
                void*             container = at + field->Offset;
                const std::size_t count     = field->ContainerSize ? field->ContainerSize( container ) : 0;
                if ( *split.Index >= count )
                    return Common::MakeError<bool>(
                         std::format( "'{}.{}' has {} element(s), so [{}] is past its end", type->Name,
                                      field->Name, count, *split.Index ) );
                if ( dot != std::string_view::npos )
                {
                    // ONE FIELD OF A STRUCT ELEMENT (VFX.Overrides[0].Value, UE Remote Control's property path
                    // through an array of structs): the element travels as a Value of its ScriptStruct, so its
                    // field is parsed by that field's own type, set in the element, and the element written
                    // back through ContainerSet as one undoable element edit.
                    const std::string_view leaf    = rest.substr( dot + 1 );
                    Value                  element = field->ContainerGet( container, *split.Index );
                    const auto*            data    = element.Get<Value::StructData>();
                    const TypeInfo*        inner =
                         data != nullptr ? Reflection::ReflectionRegistry::Get().Find( data->Type ) : nullptr;
                    if ( inner == nullptr || leaf.find( '.' ) != std::string_view::npos ||
                         leaf.find( '[' ) != std::string_view::npos )
                        return Common::MakeError<bool>(
                             std::format( "'{}.{}[{}]' is a value, so '{}' addresses nothing inside it",
                                          type->Name, field->Name, *split.Index, leaf ) );
                    const FieldInfo* sub = nullptr;
                    for ( const FieldInfo& candidate : inner->Fields )
                        if ( candidate.Name == leaf )
                            sub = &candidate;
                    if ( sub == nullptr )
                        return Common::MakeError<bool>(
                             std::format( "'{}' has no field '{}'", inner->Name, leaf ) );
                    const auto parsed = Parse( *sub, text );
                    if ( !parsed )
                        return Common::MakeError<bool>( parsed.GetError() );
                    Value::StructData edited = *data;
                    edited.Fields.Set( std::string( leaf ), parsed.GetValue() );
                    Value after = Value::MakeStruct( edited.Type, std::move( edited.Fields ) );
                    if ( !field->ContainerSet( container, *split.Index, after ) )
                        return Common::MakeError<bool>( std::format( "'{}.{}[{}].{}' refused the value '{}'",
                                                                     type->Name, field->Name, *split.Index, leaf,
                                                                     text ) );
                    history.PushFieldCommand( std::make_unique<ContainerElementCommand>(
                         *field, container, *split.Index, std::move( element ), std::move( after ) ) );
                    return Common::MakeSuccess( true );
                }
                FieldInfo element = *field;
                element.Type      = field->ElementType;
                const auto parsed = Parse( element, text );
                if ( !parsed )
                    return Common::MakeError<bool>( parsed.GetError() );
                Value before = field->ContainerGet( container, *split.Index );
                if ( !field->ContainerSet( container, *split.Index, parsed.GetValue() ) )
                    return Common::MakeError<bool>( std::format( "'{}.{}[{}]' refused the value '{}'",
                                                                 type->Name, field->Name, *split.Index, text ) );
                if ( route )
                    history.PushFieldCommand( std::make_unique<RoutedFieldCommand>(
                         route, static_cast<std::size_t>( at - static_cast<char*>( object ) ), *field,
                         *split.Index, std::move( before ), parsed.GetValue() ) );
                else
                    history.PushFieldCommand( std::make_unique<ContainerElementCommand>(
                         *field, container, *split.Index, std::move( before ), parsed.GetValue() ) );
                return Common::MakeSuccess( true );
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
            if ( route )
            {
                // An owned object: the undo record holds the field's VALUES and replays them through the route.
                auto before = Reflection::ReadField( *field, at );
                if ( !before )
                    return Common::MakeError<bool>( before.GetError() );
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — a live std::string member
                const bool                isString = field->Type == FieldType::String;
                const std::string         beforeText = isString ? *reinterpret_cast<std::string*>( p ) : std::string{};
                std::vector<std::uint8_t> beforeBytes( isString ? 0 : field->Size );
                if ( !isString )
                    std::memcpy( beforeBytes.data(), p, field->Size );
                if ( auto written = Reflection::WriteField( *field, at, parsed.GetValue() ); !written )
                    return written;
                // An unchanged value is no edit, as below.
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                if ( isString ? beforeText == *reinterpret_cast<std::string*>( p )
                              : std::memcmp( beforeBytes.data(), p, field->Size ) == 0 )
                    return Common::MakeSuccess( true );
                auto after = Reflection::ReadField( *field, at );
                if ( !after )
                    return Common::MakeError<bool>( after.GetError() );
                history.PushFieldCommand( std::make_unique<RoutedFieldCommand>(
                     route, static_cast<std::size_t>( at - static_cast<char*>( object ) ), *field, std::nullopt,
                     before.ExtractValue(), after.ExtractValue() ) );
                return Common::MakeSuccess( true );
            }
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
