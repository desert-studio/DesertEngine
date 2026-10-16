#pragma once

#include <Common/Core/Logger.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Json/Document.hpp>
#include <Common/Json/Template.hpp>

#include <format>
#include <string>
#include <string_view>
#include <utility>

// Text built from a list, or from parts that come and go, is rendered from a template kept beside the code
// that owns it (owner 10-16) instead of being assembled from `+=` pieces: the message's shape is then read in
// one place. One line of fixed shape stays std::format.
namespace Desert::Editor::TextTemplates
{
    // A template compiled once, where it is declared (a function-local static or a member). A compile error is
    // kept and returned by every Render, so a broken template surfaces where its text is asked for.
    class CompiledTemplate
    {
    public:
        CompiledTemplate( std::string_view text, std::string_view name )
             : m_Compiled( Common::Text::Compile( text, name ) )
        {
        }

        [[nodiscard]] Common::ResultStr<std::string> Render( const Common::Json::Value& data ) const
        {
            if ( !m_Compiled.IsSuccess() )
                return Common::MakeError<std::string>( m_Compiled.GetError() );
            return Common::Text::Render( m_Compiled.GetValue(), Common::Json::Root( data ) );
        }

    private:
        Common::ResultStr<Common::Text::Template> m_Compiled;
    };

    // For a caller that takes plain text (a status line, a refusal, a label): a template error is logged at
    // ERROR and becomes the text itself, so it reads as the failure it is instead of as an empty or partial
    // message.
    [[nodiscard]] inline std::string RenderOrError( const CompiledTemplate& tpl, const Common::Json::Value& data )
    {
        auto rendered = tpl.Render( data );
        if ( rendered.IsSuccess() )
            return rendered.ExtractValue();
        std::string error = std::format( "text template failed: {}", rendered.GetError() );
        LOG_ERROR( "{}", error );
        return error;
    }

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

    // The items of a JSON string list with `separator` between them ("a, b, c"); empty for no items.
    [[nodiscard]] inline std::string JoinList( const Common::Json::Value& items, std::string_view separator )
    {
        static const CompiledTemplate kJoin(
             "{% for item in items %}{{ item }}{% if not loop.last %}{{ separator }}{% endif %}{% endfor %}",
             "JoinList" );
        return RenderOrError(
             kJoin,
             Common::Json::Value(
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
