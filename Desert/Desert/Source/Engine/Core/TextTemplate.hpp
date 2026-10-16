#pragma once

#include <Common/Core/Logger.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Json/Document.hpp>
#include <Common/Json/Template.hpp>

#include <string>
#include <string_view>

namespace Desert::Core
{
    // A text template whose source sits beside the code that fills it, compiled ONCE (hold it in a function-
    // local static or a member) and rendered from a Json value (Common/Json/Template.hpp). It is how the engine
    // writes a list, a report or any text built over a loop — never by gluing pieces with `+` / `+=`.
    // A template that does not compile or render is an error, never an empty string.
    class TextTemplate
    {
    public:
        TextTemplate( std::string_view name, std::string_view text )
             : m_Name( name ), m_Compiled( Common::Text::Compile( text, name ) )
        {
        }

        [[nodiscard]] Common::ResultStr<std::string> Render( const Common::Json::Value& data ) const
        {
            if ( !m_Compiled.IsSuccess() )
                return Common::MakeError<std::string>( m_Compiled.GetError() );
            return Common::Text::Render( m_Compiled.GetValue(), Common::Json::Root( data ) );
        }

        // For a text that has no error channel of its own (a log line, a part of a message): a failure is
        // logged at ERROR and the error itself becomes the text, so it shows wherever the text goes.
        [[nodiscard]] std::string RenderOrLog( const Common::Json::Value& data ) const
        {
            auto rendered = Render( data );
            if ( rendered.IsSuccess() )
                return rendered.GetValue();
            LOG_ERROR( "[Text] template '{}' failed: {}", m_Name, rendered.GetError() );
            return rendered.GetError();
        }

    private:
        std::string                               m_Name;
        Common::ResultStr<Common::Text::Template> m_Compiled;
    };
} // namespace Desert::Core
