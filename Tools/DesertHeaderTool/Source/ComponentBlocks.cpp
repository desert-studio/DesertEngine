#include "ComponentBlocks.hpp"

#include "AnnotationText.hpp"

#include <algorithm>
#include <regex>

namespace Desert::HeaderTool
{
    ComponentBlock ParseComponentMeta( const std::string& argsRaw, std::string& error )
    {
        ComponentBlock c;
        bool           whole = false;
        for ( const auto& tokRaw : SplitTopLevel( argsRaw ) )
        {
            const std::string tok = Trimmed( tokRaw );
            if ( tok.empty() )
                continue;
            if ( tok == "Whole" )
                whole = true;
            else if ( tok.rfind( "Key", 0 ) == 0 )
                c.key = ExtractStringLiteral( tok );
            else if ( tok.rfind( "Block", 0 ) == 0 )
                c.member = ParenIdent( tok );
            else if ( tok.rfind( "Run", 0 ) == 0 )
                c.run = ParenIdent( tok );
            else
                error = "COMPONENT: unknown attribute '" + tok +
                        "' (Key(\"...\"), Block( Member ) | Whole, Run( ... ))";
        }
        if ( error.empty() && c.key.empty() )
            error = "COMPONENT: Key(\"...\") is required";
        else if ( error.empty() && c.run.empty() )
            error = "COMPONENT " + c.key + ": Run( ... ) is required";
        else if ( error.empty() && whole == !c.member.empty() )
            error = "COMPONENT " + c.key + ": exactly one of Block( Member ) and Whole";
        return c;
    }

    std::optional<std::string> DeclaredMemberType( const std::string& structBody, const std::string& member )
    {
        const std::regex decl( "([A-Za-z_][A-Za-z0-9_:]*)\\s+" + member + "\\s*[;={]" );
        std::smatch      m;
        if ( std::regex_search( structBody, m, decl ) )
            return m[1].str();
        return std::nullopt;
    }

    void ResolveComponents( const std::vector<ReflectedTypeName>& types, std::vector<ComponentBlock>& components,
                            std::vector<std::string>& errors )
    {
        for ( auto& c : components )
        {
            if ( c.member.empty() )
            {
                const auto found = std::find_if( types.begin(), types.end(),
                                                 [&]( const ReflectedTypeName& t ) { return t.fqn == c.fqn; } );
                if ( found == types.end() )
                    errors.push_back( c.where + ": COMPONENT " + c.key + " is Whole but " + c.fqn +
                                      " is not a REFLECT() type with properties" );
                else
                    c.typeName = found->registryName;
                continue;
            }
            const auto        colon = c.memberType.rfind( "::" );
            const std::string shortName =
                 colon == std::string::npos ? c.memberType : c.memberType.substr( colon + 2 );
            const auto matches = std::count_if( types.begin(), types.end(), [&]( const ReflectedTypeName& t )
                                                { return t.registryName == shortName; } );
            if ( matches != 1 )
                errors.push_back(
                     c.where + ": COMPONENT " + c.key + ": " + c.member + "'s type '" + c.memberType +
                     ( matches == 0 ? "' is not a REFLECT() type" : "' names several reflected types" ) );
            else
                c.typeName = shortName;
        }
        std::sort( components.begin(), components.end(),
                   []( const ComponentBlock& a, const ComponentBlock& b ) { return a.key < b.key; } );
        for ( size_t k = 1; k < components.size(); ++k )
            if ( components[k].key == components[k - 1].key )
                errors.push_back( components[k].where + ": COMPONENT key '" + components[k].key +
                                  "' is also stated at " + components[k - 1].where );
    }
} // namespace Desert::HeaderTool
