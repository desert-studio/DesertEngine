#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Json/Document.hpp>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

// A Jinja-shaped text template engine with no dependency beyond Common::Json (owner decision TPL1: no inja,
// no nlohmann). It exists for code and text generators, so it is strict where Jinja is lenient:
//   - an unknown variable is an ERROR (name:line:col + the variable path), unless piped through `default`;
//     the only lenient place is a bare path used as a condition (`{% if x %}`, and/or/not operands), which is
//     the existence test;
//   - printing null, an array or an object is an error, not "None" or "[...]";
//   - the filter set is closed: upper, lower, join(sep), length, default(value).
// Supported: {{ a.b[0].c | filter }}, {% for x in list %}, {% for k, v in object %} with loop.index,
// loop.index0, loop.first, loop.last, loop.length; {% if %}/{% elif %}/{% else %}/{% endif %} over == != < >
// <= >= and/or/not and string/number/bool literals; {# comments #}; whitespace control on every tag
// ({{- -}}, {%- -%}, {#- -#}) exactly as Jinja: the dash strips ALL whitespace, newlines included, on its side;
// {% include "name" %} through an explicitly passed loader (the engine never touches the file system).
// A template is parsed once by Compile and rendered any number of times.
namespace Common::Text
{
    namespace Detail
    {
        struct TemplateBody;
    }

    class Template
    {
    public:
        Template() = default;

        [[nodiscard]] const std::string& Name() const;
        [[nodiscard]] bool               IsCompiled() const
        {
            return m_Body != nullptr;
        }
        // The parsed form; opaque outside Template.cpp.
        [[nodiscard]] const Detail::TemplateBody* Body() const
        {
            return m_Body.get();
        }

    private:
        friend ResultStr<Template> Compile( std::string_view text, std::string_view name );

        std::shared_ptr<const Detail::TemplateBody> m_Body;
    };

    // Resolves `{% include "name" %}`. A failure is reported with the including tag's position.
    using TemplateLoader = std::function<ResultStr<const Template*>( std::string_view name )>;

    [[nodiscard]] ResultStr<Template> Compile( std::string_view text, std::string_view name );

    [[nodiscard]] ResultStr<std::string> Render( const Template& tpl, const Json::Node& data,
                                                 const TemplateLoader& loader = {} );

    // Named templates compiled once; its Loader() serves them to `include`. The set must outlive the loader.
    class TemplateSet
    {
    public:
        BoolResultStr Add( std::string_view name, std::string_view text );

        [[nodiscard]] const Template* Find( std::string_view name ) const;
        [[nodiscard]] TemplateLoader  Loader() const;

    private:
        std::map<std::string, Template, std::less<>> m_Templates;
    };
} // namespace Common::Text
