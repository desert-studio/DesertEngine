#pragma once

#include <Engine/Core/TextTemplate.hpp>

#include <Common/Json/Document.hpp>

#include <string>
#include <string_view>
#include <utility>

// A list of names as one line of text ("a, b, c") through the engine's text template
// (Engine/Core/TextTemplate.hpp), never by `+=` in a loop (owner 10-16).
namespace Desert::Editor::TextTemplates
{
    // Each item as a JSON string, through `project` (identity by default), for a template's list.
    template <typename Range, typename Project>
    [[nodiscard]] Common::Json::Value StringList( const Range& items, Project project )
    {
        Common::Json::Value::Array array;
        for ( const auto& item : items )
            array.emplace_back( std::string( std::string_view( project( item ) ) ) );
        return Common::Json::Value( std::move( array ) );
    }

    template <typename Range>
    [[nodiscard]] Common::Json::Value StringList( const Range& items )
    {
        return StringList( items, []( const auto& item ) -> decltype( auto ) { return item; } );
    }

    // The items of a JSON string list with `separator` between them; empty for no items. A template failure is
    // logged at ERROR and becomes the text (TextTemplate::RenderOrLog).
    [[nodiscard]] inline std::string JoinList( const Common::Json::Value& items, std::string_view separator )
    {
        static const ::Desert::Core::TextTemplate kJoin(
             "JoinList",
             "{% for item in items %}{{ item }}{% if not loop.last %}{{ separator }}{% endif %}{% endfor %}" );
        return kJoin.RenderOrLog( Common::Json::Value(
             Common::Json::ObjectBuilder().Set( "items", items ).Set( "separator", separator ).Build() ) );
    }

    template <typename Range, typename Project>
    [[nodiscard]] std::string JoinList( const Range& items, std::string_view separator, Project project )
    {
        return JoinList( StringList( items, project ), separator );
    }

    template <typename Range>
    [[nodiscard]] std::string JoinList( const Range& items, std::string_view separator )
    {
        return JoinList( StringList( items ), separator );
    }
} // namespace Desert::Editor::TextTemplates
