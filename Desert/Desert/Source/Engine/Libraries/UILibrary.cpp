#include <Engine/Libraries/UILibrary.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/UI/UIDataStore.hpp>
#include <Engine/UI/UIOverlay.hpp>

#include <format>
#include <optional>

namespace Desert::Libraries
{
    namespace
    {
        using Reflection::FieldType;
        using Reflection::Value;

        UI::UIDataStore& Store()
        {
            return UI::UIDataStore::Get();
        }

        /// A Value as what a UI binding shows: a number (any numeric kind), a boolean, a string or a colour.
        std::optional<UI::UIDataStore::Value> Shown( const Value& value )
        {
            switch ( value.Type() )
            {
                case FieldType::Double:
                    return *value.Get<double>();
                case FieldType::Float:
                    return static_cast<double>( *value.Get<float>() );
                case FieldType::Int:
                    return static_cast<double>( *value.Get<std::int64_t>() );
                case FieldType::UInt:
                    return static_cast<double>( *value.Get<std::uint64_t>() );
                case FieldType::Bool:
                    return *value.Get<bool>();
                case FieldType::String:
                    return *value.Get<std::string>();
                case FieldType::Vec3:
                {
                    const Value::Float3& c = *value.Get<Value::Float3>();
                    return glm::vec3( c[0], c[1], c[2] );
                }
                default:
                    return std::nullopt;
            }
        }

        /// A record's fields -> one UI record. A field a binding cannot show is refused by name rather than
        /// dropped, so a typo in a record is an error at the write and not a blank cell.
        Common::ResultStr<UI::UIDataStore> Record( const Value::Map& fields )
        {
            UI::UIDataStore record;
            for ( std::size_t i = 0; i < fields.Size(); ++i )
            {
                const std::optional<UI::UIDataStore::Value> shown = Shown( fields.Values[i] );
                if ( !shown )
                    return Common::MakeError<UI::UIDataStore>( std::format(
                         "ui.list record: field '{}' is a {}; a UI binding shows a number, a boolean, a string or "
                         "a colour (vector)",
                         fields.Keys[i], Reflection::FieldTypeName( fields.Values[i].Type() ) ) );
                record.Set( fields.Keys[i], *shown );
            }
            return Common::MakeSuccess( std::move( record ) );
        }
    } // namespace

    void UILibrary::Set( const std::string& key, const Value& value )
    {
        const std::optional<UI::UIDataStore::Value> shown = Shown( value );
        if ( !shown )
        {
            LOG_ERROR( "[Script] ui.set('{}'): a value is a number, a boolean, a string or a colour (vector); got {}",
                       key, Reflection::FieldTypeName( value.Type() ) );
            return;
        }
        Store().Set( key, *shown );
    }

    Value UILibrary::Get( const std::string& key )
    {
        const UI::UIDataStore& store = Store();
        if ( const auto b = store.Bool( key ); b && !store.Number( key ) )
            return Value::Bool( *b );
        if ( const auto n = store.Number( key ) )
            return Value::Double( *n );
        if ( const auto t = store.Text( key ) )
            return Value::String( *t );
        if ( const auto c = store.Color( key ) )
            return Value::Vec3( { c->x, c->y, c->z } );
        return {};
    }

    bool UILibrary::Has( const std::string& key )
    {
        return Store().Has( key );
    }

    void UILibrary::Erase( const std::string& key )
    {
        Store().Erase( key );
    }

    void UILibrary::Clear()
    {
        Store().Clear();
    }

    void UILibrary::Send( const std::string& message )
    {
        UI::UIMessageQueue::Get().Push( message );
    }

    void UILibrary::Toast( const std::string& overlay, const std::string& text )
    {
        UI::UIOverlayRequests::Get().Raise( overlay, text );
    }

    Common::BoolResultStr UILibrary::ListAdd( const std::string& key, const Value::Map& record )
    {
        Common::ResultStr<UI::UIDataStore> made = Record( record );
        if ( !made.IsSuccess() )
            return Common::MakeError<bool>( made.GetError() );
        Store().Collection( key ).Add( made.ExtractValue() );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr UILibrary::ListInsert( const std::string& key, int index, const Value::Map& record )
    {
        Common::ResultStr<UI::UIDataStore> made = Record( record );
        if ( !made.IsSuccess() )
            return Common::MakeError<bool>( made.GetError() );
        return Store().Collection( key ).Insert( index - 1, made.ExtractValue() );
    }

    Common::BoolResultStr UILibrary::ListRemove( const std::string& key, int index )
    {
        return Store().Collection( key ).Remove( index - 1 );
    }

    Common::BoolResultStr UILibrary::ListSet( const std::string& key, int index, const std::string& field,
                                              const Value& value )
    {
        const std::optional<UI::UIDataStore::Value> shown = Shown( value );
        if ( !shown )
            return Common::MakeError<bool>( std::format(
                 "ui.list_set('{}', {}, '{}'): a value is a number, a boolean, a string or a colour (vector); got {}",
                 key, index, field, Reflection::FieldTypeName( value.Type() ) ) );
        return Store().Collection( key ).SetField( index - 1, field, *shown );
    }

    void UILibrary::ListClear( const std::string& key )
    {
        Store().Collection( key ).Clear();
    }

    int UILibrary::ListCount( const std::string& key )
    {
        const UI::UICollection* collection = Store().FindCollection( key );
        return collection != nullptr ? collection->Size() : 0;
    }
} // namespace Desert::Libraries
