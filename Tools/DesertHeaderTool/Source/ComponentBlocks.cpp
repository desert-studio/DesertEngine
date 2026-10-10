#include "ComponentBlocks.hpp"

#include "AnnotationText.hpp"

#include <algorithm>
#include <format>
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
            // The attribute is the whole name before its '(' — a prefix match would take "Runn(" for "Run(".
            const std::string name = Trimmed( tok.substr( 0, tok.find( '(' ) ) );
            if ( tok == "Whole" )
                whole = true;
            else if ( name == "Key" )
                c.key = ExtractStringLiteral( tok );
            else if ( name == "Block" )
                c.member = ParenIdent( tok );
            else if ( name == "Run" )
                c.run = ParenIdent( tok );
            else
                error = std::format(
                     "COMPONENT: unknown attribute '{}' (Key(\"...\"), Block( Member ) | Whole, Run( ... ))",
                     tok );
        }
        if ( error.empty() && c.key.empty() )
            error = "COMPONENT: Key(\"...\") is required";
        else if ( error.empty() && c.run.empty() )
            error = std::format( "COMPONENT {}: Run( ... ) is required", c.key );
        else if ( error.empty() && whole == !c.member.empty() )
            error = std::format( "COMPONENT {}: exactly one of Block( Member ) and Whole", c.key );
        return c;
    }

    std::optional<std::string> DeclaredMemberType( const std::string& structBody, const std::string& member )
    {
        const std::regex decl( std::format( R"(([A-Za-z_][A-Za-z0-9_:]*)\s+{}\s*[;={{])", member ) );
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
                    errors.push_back( std::format( "{}: COMPONENT {} is Whole but {} is not a REFLECT() type with "
                                                   "properties",
                                                   c.where, c.key, c.fqn ) );
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
                     std::format( "{}: COMPONENT {}: {}'s type '{}' {}", c.where, c.key, c.member, c.memberType,
                                  matches == 0 ? "is not a REFLECT() type" : "names several reflected types" ) );
            else
                c.typeName = shortName;
        }
        std::sort( components.begin(), components.end(),
                   []( const ComponentBlock& a, const ComponentBlock& b ) { return a.key < b.key; } );
        for ( size_t k = 1; k < components.size(); ++k )
            if ( components[k].key == components[k - 1].key )
                errors.push_back( std::format( "{}: COMPONENT key '{}' is also stated at {}", components[k].where,
                                               components[k].key, components[k - 1].where ) );
    }
} // namespace Desert::HeaderTool
