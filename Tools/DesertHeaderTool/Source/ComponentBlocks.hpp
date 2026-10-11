#pragma once

// COMPONENT(...) — the marker that registers an ECS component's block in a scene record
// (Engine/Reflection/ReflectionMacros.hpp). The header tool reads the marker inside the struct, reads the
// Block member's declared type from the struct body, and resolves that type against the REFLECT() registry;
// the resolved, key-ordered rows become Engine/Generated/ReflectedComponentBlocks.gen.hpp.

#include <optional>
#include <string>
#include <vector>

namespace Desert::HeaderTool
{
    // One row of the generated ReflectedComponentBlocks.gen.hpp. The component struct itself is NOT reflected;
    // its Block( Member ) is — or, for Whole, the component is.
    struct ComponentBlock
    {
        std::string fqn;           // the component, fully qualified
        std::string key;           // Key( "..." ): the block's key in a scene record
        std::string member;        // Block( Member ); empty for Whole
        std::string memberType;    // the member's declared type spelling, read from the struct body
        std::string run;           // Run( ... ): a ReflectedBlockRun enumerator
        std::string typeName;      // the reflected type's registry name, resolved once every header is parsed
        std::string headerInclude; // include path relative to source root
        std::string where;         // file:line of the marker, for diagnostics
    };

    // A REFLECT() type as the resolution sees it.
    struct ReflectedTypeName
    {
        std::string fqn;          // fully-qualified C++ name
        std::string registryName; // the registry key (the short name)
    };

    // COMPONENT( Key( "Camera" ), Block( Data ) | Whole, Run( ActorsAndUI ) ) — `argsRaw` is the text between
    // the parentheses. Every attribute is required and an unknown one is an error (written to `error`).
    ComponentBlock ParseComponentMeta( const std::string& argsRaw, std::string& error );

    // The declared type spelling of `member` in a struct body ("<type> <member> ;|=|{"), or nothing.
    std::optional<std::string> DeclaredMemberType( const std::string& structBody, const std::string& member );

    // Each COMPONENT's reflected type by registry name: its Block member's type, or the component itself
    // (Whole). Unknown types and a key stated twice are errors; the rows end up ordered by key, so the list does
    // not depend on the order the directory iterator hands the headers out in.
    void ResolveComponents( const std::vector<ReflectedTypeName>& types, std::vector<ComponentBlock>& components,
                            std::vector<std::string>& errors );
} // namespace Desert::HeaderTool
