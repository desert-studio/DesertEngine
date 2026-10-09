#pragma once

// The text pieces every annotation reader shares: PROPERTY(...), FUNCTION(...) and COMPONENT(...) are all a
// comma list of attributes, each a word with an optional parenthesised argument.

#include <string>
#include <vector>

namespace Desert::HeaderTool
{
    // The attribute's string argument, CONCATENATING adjacent literals ("a" "b" -> "ab"); escapes kept verbatim.
    std::string ExtractStringLiteral( const std::string& s );

    // The contents of ANNOTATION( ... ) split by top-level commas (commas inside (), <>, "" do not split).
    std::vector<std::string> SplitTopLevel( const std::string& s );

    // `v` without leading and trailing whitespace.
    std::string Trimmed( std::string v );

    // The identifier inside "Word( Ident )", or empty.
    std::string ParenIdent( const std::string& tok );
} // namespace Desert::HeaderTool
