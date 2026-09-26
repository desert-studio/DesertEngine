// TextTemplate: the Jinja-shaped text template engine in Common/Json/Template (TPL1). One test per construct, the
// whitespace-control rules byte for byte, every error kind with its name:line:col, include, nesting depth
// and a large template as an upper time bound (a guard, not a benchmark).
#include <Common/Json/Template.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>

using namespace Common;
using namespace Common::Text;

namespace
{
    Json::Value Data( std::string_view json )
    {
        auto parsed = Json::Parse( json );
        EXPECT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
        return parsed.ExtractValue();
    }

    // Renders `text` against `json`; returns the output or "ERROR: <message>".
    std::string Expand( std::string_view text, std::string_view json = "{}", const TemplateLoader& loader = {} )
    {
        auto compiled = Compile( text, "t" );
        if ( !compiled.IsSuccess() )
            return "ERROR: " + compiled.GetError();
        const Json::Value data     = Data( json );
        auto              rendered = Render( compiled.GetValue(), Json::Root( data ), loader );
        return rendered.IsSuccess() ? rendered.GetValue() : "ERROR: " + rendered.GetError();
    }
} // namespace

TEST( TextTemplate, PlainTextPassesThrough )
{
    EXPECT_EQ( Expand( "no tags { here } at all" ), "no tags { here } at all" );
    EXPECT_EQ( Expand( "" ), "" );
}

TEST( TextTemplate, PathsReachMembersAndIndices )
{
    const char* json =
         R"({"a":{"b":{"c":"deep"}},"items":[{"name":"first"},{"name":"second"}],"n":42,"r":1.5,"t":true})";
    EXPECT_EQ( Expand( "{{ a.b.c }}", json ), "deep" );
    EXPECT_EQ( Expand( "{{ items[1].name }}|{{ items[0][\"name\"] }}", json ), "second|first" );
    EXPECT_EQ( Expand( "{{n}} {{ r }} {{ t }}", json ), "42 1.5 true" );
    EXPECT_EQ( Expand( "{{ 'lit' }} {{ -7 }}", json ), "lit -7" );
}

TEST( TextTemplate, FiltersAreTheClosedSet )
{
    const char* json = R"({"s":"MiXed","list":["a","b","c"],"nums":[1,2],"obj":{"x":1,"y":2}})";
    EXPECT_EQ( Expand( "{{ s | upper }} {{ s | lower }}", json ), "MIXED mixed" );
    EXPECT_EQ( Expand( "{{ list | join(\", \") }}|{{ nums | join(\"+\") }}", json ), "a, b, c|1+2" );
    EXPECT_EQ( Expand( "{{ list | length }} {{ s | length }} {{ obj | length }}", json ), "3 5 2" );
    EXPECT_EQ( Expand( "{{ missing | default(\"x\") }} {{ s | default(\"x\") }}", json ), "x MiXed" );
    EXPECT_EQ( Expand( "{{ list | join(\"\") | upper }}", json ), "ABC" );
    EXPECT_EQ( Expand( "{{ s | title }}", json ),
               "ERROR: t:1:8: unknown filter 'title' (known: upper, lower, join, length, default)" );
    EXPECT_EQ( Expand( "{{ nums | upper }}", json ), "ERROR: t:1:11: filter 'upper' needs a string, found array" );
}

TEST( TextTemplate, ForOverArrayWithLoopVariables )
{
    const char* json = R"({"xs":["a","b","c"]})";
    EXPECT_EQ( Expand( "{% for x in xs %}{{ loop.index }}{{ loop.index0 }}{{ x }}"
                    "{% if loop.first %}F{% endif %}{% if loop.last %}L{% endif %};{% endfor %}",
                    json ),
               "10aF;21b;32cL;" );
    EXPECT_EQ(
         Expand( "{% for x in xs %}{% if not loop.last %}{{ x }},{% else %}{{ x }}{% endif %}{% endfor %}", json ),
         "a,b,c" );
    EXPECT_EQ( Expand( "[{% for x in empty %}never{% endfor %}]", R"({"empty":[]})" ), "[]" );
}

TEST( TextTemplate, ForOverObjectKeepsMemberOrder )
{
    EXPECT_EQ( Expand( "{% for k, v in o %}{{ k }}={{ v }} {% endfor %}", R"({"o":{"z":1,"a":2,"m":3}})" ),
               "z=1 a=2 m=3 " );
    EXPECT_EQ( Expand( "{% for x in o %}{% endfor %}", R"({"o":{"z":1}})" ),
               "ERROR: t:1:13: 'for x in' needs an array, found object (iterate an object with 'for k, v in')" );
}

TEST( TextTemplate, NestedLoopsSeeTheirOwnLoopAndOuterVariables )
{
    const char* json = R"({"rows":[{"n":"r1","cells":[1,2]},{"n":"r2","cells":[3]}]})";
    EXPECT_EQ( Expand( "{% for r in rows %}{{ r.n }}:{% for c in r.cells %}{{ loop.index }}/{{ c }}@{{ r.n }} "
                    "{% endfor %}{{ loop.index }}|{% endfor %}",
                    json ),
               "r1:1/1@r1 2/2@r1 1|r2:1/3@r2 2|" );
}

TEST( TextTemplate, IfElifElseAndExpressions )
{
    const char* json = R"({"n":5,"s":"abc","flag":false,"list":[1],"none":null})";
    auto        pick = [&]( std::string_view cond )
    { return Expand( std::string( "{% if " ) + std::string( cond ) + " %}Y{% else %}N{% endif %}", json ); };
    EXPECT_EQ( pick( "n == 5" ), "Y" );
    EXPECT_EQ( pick( "n != 5" ), "N" );
    EXPECT_EQ( pick( "n < 6 and n > 4" ), "Y" );
    EXPECT_EQ( pick( "n <= 4 or n >= 5" ), "Y" );
    EXPECT_EQ( pick( "n == 5.0" ), "Y" );
    EXPECT_EQ( pick( "s == \"abc\"" ), "Y" );
    EXPECT_EQ( pick( "s < \"abd\"" ), "Y" );
    EXPECT_EQ( pick( "not flag" ), "Y" );
    EXPECT_EQ( pick( "flag == false" ), "Y" );
    EXPECT_EQ( pick( "n == \"5\"" ), "N" );
    EXPECT_EQ( pick( "list" ), "Y" );
    EXPECT_EQ( pick( "none" ), "N" );
    EXPECT_EQ( pick( "missing" ), "N" );
    EXPECT_EQ( pick( "not missing and (n == 1 or s)" ), "Y" );
    EXPECT_EQ( pick( "missing.deeper" ), "N" );
    EXPECT_EQ(
         Expand( "{% if n == 1 %}one{% elif n == 5 %}five{% elif n == 5 %}again{% else %}other{% endif %}", json ),
         "five" );
    EXPECT_EQ( Expand( "{% if n == 1 %}one{% elif n == 2 %}two{% endif %}.", json ), "." );
    EXPECT_EQ( pick( "missing == 1" ), "ERROR: t:1:7: unknown variable 'missing'" );
    EXPECT_EQ( pick( "s < 1" ), "ERROR: t:1:9: cannot order string against integer" );
}

TEST( TextTemplate, CommentsProduceNothing )
{
    EXPECT_EQ( Expand( "a{# a comment {{ not_evaluated }} #}b" ), "ab" );
    EXPECT_EQ( Expand( "a\n  {#- stripped -#}  \nb" ), "ab" );
}

TEST( TextTemplate, WhitespaceControlStripsExactlyItsSide )
{
    const char* json = R"({"xs":["a","b"],"v":"V"})";
    EXPECT_EQ( Expand( "x  \n  {{- v }}  \n", json ), "xV  \n" );
    EXPECT_EQ( Expand( "x  \n  {{ v -}}  \n  y", json ), "x  \n  Vy" );
    EXPECT_EQ( Expand( "x  \n  {{- v -}}  \n  y", json ), "xVy" );
    // The code-generation idiom: every statement on its own line, no stray blank lines left behind.
    EXPECT_EQ( Expand( "list:\n"
                    "{%- for x in xs %}\n"
                    "  - {{ x }}\n"
                    "{%- endfor %}\n"
                    "end\n",
                    json ),
               "list:\n  - a\n  - b\nend\n" );
    // Without the dashes every tag leaves its line's newline behind, as in Jinja without trim_blocks.
    EXPECT_EQ( Expand( "{% for x in xs %}\n{{ x }}\n{% endfor %}\n", json ), "\na\n\nb\n\n" );
}

TEST( TextTemplate, UnknownVariableNamesTemplateLineColumnAndPath )
{
    EXPECT_EQ( Expand( "line one\n  {{ a.b.c }}", R"({"a":{"b":{}}})" ), "ERROR: t:2:6: unknown variable 'a.b.c'" );
    EXPECT_EQ( Expand( "{{ items[3].name }}", R"({"items":[]})" ), "ERROR: t:1:4: unknown variable 'items[3].name'" );
    EXPECT_EQ( Expand( "{% for x in nope %}{% endfor %}" ), "ERROR: t:1:13: unknown variable 'nope'" );
    EXPECT_EQ( Expand( "{{ s.member }}", R"({"s":"text"})" ), "ERROR: t:1:4: 's.member': member 'member' of string" );
    EXPECT_EQ( Expand( "{{ o }}", R"({"o":{}})" ), "ERROR: t:1:4: cannot print object 'o'" );
    EXPECT_EQ( Expand( "{{ z }}", R"({"z":null})" ), "ERROR: t:1:4: cannot print null 'z'" );
}

TEST( TextTemplate, SyntaxErrorsNamePosition )
{
    EXPECT_EQ( Expand( "ok\n{% for x in xs %}\nbody" ), "ERROR: t:2:1: 'for' is never closed, expected 'endfor'" );
    EXPECT_EQ( Expand( "{% endif %}" ), "ERROR: t:1:1: 'endif' without a matching opening tag" );
    EXPECT_EQ( Expand( "{% if a %}x{% endfor %}" ), "ERROR: t:1:12: 'endfor' without a matching opening tag" );
    EXPECT_EQ( Expand( "a {{ x" ), "ERROR: t:1:3: unterminated tag, expected '}}'" );
    EXPECT_EQ( Expand( "{{ a + b }}" ), "ERROR: t:1:6: unexpected character '+'" );
    EXPECT_EQ( Expand( "{{ a b }}" ), "ERROR: t:1:6: unexpected token, found 'b'" );
    EXPECT_EQ( Expand( "{% while x %}" ), "ERROR: t:1:1: unknown statement 'while' (known: for, if, include)" );
    EXPECT_EQ( Expand( "{% for x of xs %}{% endfor %}" ), "ERROR: t:1:10: expected 'in', found 'of'" );
    EXPECT_EQ( Expand( "{{ }}" ), "ERROR: t:1:1: empty output tag" );
    EXPECT_EQ( Expand( "{{ 'open }}" ), "ERROR: t:1:4: unterminated string literal" );
    EXPECT_EQ( Expand( "{% if x %}{% else %}{% elif y %}{% endif %}" ), "ERROR: t:1:21: 'elif' without a matching opening tag" );
    EXPECT_EQ( Expand( "{{ s | join }}" ),
               "ERROR: t:1:13: filter 'join' takes one argument, expected '(', found end of tag" );
}

TEST( TextTemplate, IncludeGoesThroughTheLoaderAndSeesTheScope )
{
    TemplateSet set;
    ASSERT_TRUE( set.Add( "item", "<{{ x.name }}:{{ loop.index }}>" ).IsSuccess() );
    ASSERT_TRUE( set.Add( "self", "{% include \"self\" %}" ).IsSuccess() );
    ASSERT_TRUE( set.Add( "broken", "{{ nope }}" ).IsSuccess() );
    const char* json = R"({"xs":[{"name":"a"},{"name":"b"}]})";
    EXPECT_EQ( Expand( "{% for x in xs %}{% include \"item\" %}{% endfor %}", json, set.Loader() ), "<a:1><b:2>" );
    EXPECT_EQ( Expand( "{% include \"absent\" %}", json, set.Loader() ),
               "ERROR: t:1:1: include 'absent': no template named 'absent' in the set" );
    EXPECT_EQ( Expand( "{% include \"item\" %}", json ),
               "ERROR: t:1:1: include 'item' but no template loader was given" );
    EXPECT_EQ( Expand( "{% include \"broken\" %}", json, set.Loader() ),
               "ERROR: broken:1:4: unknown variable 'nope'" );
    const std::string cycle = Expand( "{% include \"self\" %}", json, set.Loader() );
    EXPECT_NE( cycle.find( "self:1:1: include 'self' nests deeper than 64 (a cycle?)" ), std::string::npos )
         << cycle;
    EXPECT_FALSE( set.Add( "item", "again" ).IsSuccess() );
    const auto badAdd = set.Add( "bad", "{% if %}" );
    ASSERT_FALSE( badAdd.IsSuccess() );
    EXPECT_EQ( badAdd.GetError(), "bad:1:7: expected a value, found end of tag" );
}

TEST( TextTemplate, DeepNestingOfForAndIf )
{
    constexpr int kDepth = 100;
    std::string   text;
    for ( int i = 0; i < kDepth; ++i )
        text += "{% if xs %}{% for x in xs %}";
    text += "{{ x }}";
    for ( int i = 0; i < kDepth; ++i )
        text += "{% endfor %}{% endif %}";
    EXPECT_EQ( Expand( text, R"({"xs":["v"]})" ), "v" );
}

TEST( TextTemplate, CompileOnceRenderMany )
{
    auto compiled = Compile( "{{ v }}", "once" );
    ASSERT_TRUE( compiled.IsSuccess() );
    for ( int i = 0; i < 3; ++i )
    {
        const Json::Value data = Json::Value( Json::ObjectBuilder().Set( "v", i ).Build() );
        const auto rendered = Render( compiled.GetValue(), Json::Root( data ) );
        EXPECT_EQ( rendered.GetValue(), std::to_string( i ) );
    }
    const Json::Value empty = Data( "{}" );
    EXPECT_FALSE( Render( Template{}, Json::Root( empty ) ).IsSuccess() );
}

TEST( TextTemplate, TenThousandLinesRenderWithinABound )
{
    // 10 000 output lines through a loop with a filter, a condition and an include per line. The bound is a
    // guard against quadratic behaviour (it runs in milliseconds), not a benchmark.
    Json::Value::Array rows;
    for ( int i = 0; i < 10000; ++i )
        rows.emplace_back( Json::ObjectBuilder().Set( "name", "row" + std::to_string( i ) ).Set( "n", i ).Build() );
    const Json::Value data( Json::ObjectBuilder().Set( "rows", Json::Value( std::move( rows ) ) ).Build() );
    TemplateSet set;
    ASSERT_TRUE( set.Add( "cell", "{{ r.n }}" ).IsSuccess() );
    const auto start  = std::chrono::steady_clock::now();
    auto compiled = Compile( "{%- for r in rows %}\n{{ r.name | upper }}{% if r.n > 4999 %} high{% endif %} "
                             "{% include \"cell\" %}\n{%- endfor %}",
                             "rows" );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
    const auto rendered = Render( compiled.GetValue(), Json::Root( data ), set.Loader() );
    ASSERT_TRUE( rendered.IsSuccess() ) << rendered.GetError();
    const std::string& output = rendered.GetValue();
    const auto elapsed =
         std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - start ).count();
    EXPECT_EQ( std::count( output.begin(), output.end(), '\n' ), 10000 );
    EXPECT_EQ( output.substr( 0, 13 ), "\nROW0 0\nROW1 " );
    EXPECT_NE( output.find( "\nROW9999 high 9999" ), std::string::npos );
    EXPECT_LT( elapsed, 2000 ) << "10 000 lines took " << elapsed << " ms";
    std::cout << "[TextTemplate] 10000 lines in " << elapsed << " ms\n";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
