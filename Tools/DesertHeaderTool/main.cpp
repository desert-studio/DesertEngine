// DesertHeaderTool — a lightweight Unreal-Header-Tool-style code generator.
//
// Scans C++ headers for REFLECT() / PROPERTY(...) annotations and emits, per engine module of
// BuildScripts/DesertModules.lua, one translation unit with RegisterReflection_<Module>() that registers the
// module's reflected types (with field offsets, types and editor metadata) into
// Desert::Reflection::ReflectionRegistry, plus the module list <output-file> that calls them in the table's
// order at static-init time and resolves struct links once (plan C11).
//
// Usage:
//   DesertHeaderTool --templates <dir> [--modules <DesertModules.lua>]
//                    [--reflect <source-root> <scan-subdir> <output-file> [--reflect-anchor <Name>]
//                               [--reflect-components <output-file>]]
//                    [--check <include-root>]... [--context <include-root>]...
//                    [--subsystems <Owner> <OwnerType> <owner-header> <output-file>]...
//     --templates    directory of the *.tpl text templates (Tools/DesertHeaderTool/Templates).
//     --modules      the module table. The repository root is its file's grandparent.
//     --reflect      REFLECT()/PROPERTY()/FUNCTION() registration of <source-root>/<scan-subdir>: the module list
//                    in <output-file>, each module's types in Reflection_<Module>.gen.cpp beside it. Without
//                    --modules the scanned set is ONE module named by <output-file>'s stem (a test fixture).
//     --reflect-anchor <Name>  after --reflect: the force-link function the output defines (default
//                    ForceLinkGeneratedReflection; a second generated set in one image names its own).
//     --reflect-components <output-file>  after --reflect: the COMPONENT(...) block list.
//     --check        sources whose routed-event handlers are verified (a build error with file:line).
//     --context      sources read for events, bases and attachments but not diagnosed.
//     --subsystems   CreateSubsystems() of <OwnerType> for every DESERT_SUBSYSTEM( <Owner> ) class.
//
//     --reflect-components  after --reflect: the COMPONENT(...) rows as a header
//     (ReflectedComponentBlocks.gen.hpp).
// The annotation macros (REFLECT/PROPERTY) expand to nothing during normal compilation; only this
// tool reads them. See Engine/Reflection/ReflectionMacros.hpp.

#include <algorithm>
#include <Common/Json/Document.hpp>
#include <Common/Json/Template.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <ToolMain.hpp>

#include "Source/AnnotationText.hpp"
#include "Source/ComponentBlocks.hpp"
#include "Source/HeaderScan.hpp"
#include "Source/ModuleTable.hpp"

#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <iostream>
#include <iterator>
#include <set>
#include <utility>
#include <optional>
#include <sstream>
#include <charconv>
#include <string_view>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    using Desert::HeaderTool::ComponentBlock;
    using Desert::HeaderTool::ExtractStringLiteral;
    using Desert::HeaderTool::ParenIdent;
    using Desert::HeaderTool::SplitTopLevel;
    using Desert::HeaderTool::Trimmed;

    // --------------------------------------------------------------------- data model

    struct Metadata
    {
        std::string displayName;
        std::string category;
        std::string tooltip;
        std::string header;
        bool        hasRange = false;
        std::string rangeMin = "0.0f";
        std::string rangeMax = "0.0f";
        bool        isColor   = false;
        bool        isAsset   = false;
        std::string assetType;
        bool        thumbnail = false;
        bool        readOnly  = false;
        bool        hidden    = false;
        bool        isLength  = false; // PROPERTY(Length) — the field is a distance in world units (cm)
        std::string units;             // PROPERTY(Units("deg")) — display suffix + drag step
        bool        advanced    = false; // PROPERTY(Advanced) — folds under "Advanced" in its category
        bool        summary     = false; // PROPERTY(Summary)  — feeds the component header's one-liner
        bool        temperature = false; // PROPERTY(Temperature) — Kelvin slider on a Color field
        bool        preview     = false; // PROPERTY(Preview) — inline asset preview instead of a name button
        std::string editCondition;       // PROPERTY(EditCondition("Foo")) — grey out while Foo is false
    };

    struct Field
    {
        std::string name;
        std::string cppType;    // e.g. "glm::vec4"
        std::string fieldType;  // FieldType enum name, e.g. "Vec4"
        Metadata    meta;
        std::vector<std::pair<std::string, long long>> enumValues; // populated when fieldType == "Enum"

        bool        isContainer   = false; // std::vector<...>
        std::string elemFieldType;         // FieldType of the element (when isContainer)
    };

    // A scanned enum definition (collected across all headers before reflected types are parsed, so a
    // struct can reference an enum declared in another file).
    struct EnumDef
    {
        std::string                                    fqn;       // e.g. Desert::ECS::LightFalloff
        std::string                                    shortName; // e.g. LightFalloff
        std::vector<std::pair<std::string, long long>> values;
    };

    // FUNCTION(...) attributes (Engine/Reflection/ReflectionMacros.hpp).
    struct FunctionMeta
    {
        bool        scriptCallable = false;
        std::string category;
        std::string tooltip;
    };

    // A FUNCTION(...)-annotated member. Only what the compiler cannot know is read here: the parameters' names
    // and the attributes. The kinds, static and const come from &T::name in the generated file
    // (Engine/Reflection/FunctionThunk.hpp); the spellings below are kept for diagnostics and binding generators.
    struct Function
    {
        std::string                                      name;
        std::string                                      returnType;
        std::vector<std::pair<std::string, std::string>> params; // { name, C++ spelling }
        FunctionMeta                                     meta;
    };

    struct ReflectedType
    {
        std::string           fqn;          // fully-qualified C++ name, e.g. Desert::Assets::SurfaceMaterialData
        std::string           registryName; // short name used as the registry key, e.g. SurfaceMaterialData
        std::vector<Field>    fields;
        std::vector<Function> functions;
        std::string        headerInclude; // include path relative to source root
        std::string        module;        // BuildScripts/DesertModules.lua module of the header
    };

    // --------------------------------------------------------------------- helpers

    std::string ReadFile( const fs::path& p )
    {
        std::ifstream in( p, std::ios::binary );
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    bool IsIdentChar( char c )
    {
        return std::isalnum( static_cast<unsigned char>( c ) ) || c == '_';
    }

    // Removes // and /* */ comments while respecting string and char literals (so braces or //
    // sequences inside "..." are preserved verbatim).
    using Desert::HeaderTool::StripComments;

    // Reads an identifier (optionally qualified with ::) starting at i; advances i past it.
    std::string ReadQualifiedIdent( const std::string& s, size_t& i )
    {
        std::string id;
        while ( i < s.size() )
        {
            if ( IsIdentChar( s[i] ) )
            {
                id += s[i++];
            }
            else if ( s[i] == ':' && i + 1 < s.size() && s[i + 1] == ':' )
            {
                id += "::";
                i += 2;
            }
            else
            {
                break;
            }
        }
        return id;
    }

    void SkipWs( const std::string& s, size_t& i )
    {
        while ( i < s.size() && std::isspace( static_cast<unsigned char>( s[i] ) ) )
            ++i;
    }

    // Maps a C++ type spelling to a Reflection::FieldType enum name.
    std::string MapFieldType( std::string type )
    {
        // normalise: drop leading/trailing spaces
        auto trim = []( std::string& t ) {
            while ( !t.empty() && std::isspace( (unsigned char)t.front() ) ) t.erase( t.begin() );
            while ( !t.empty() && std::isspace( (unsigned char)t.back() ) ) t.pop_back();
        };
        trim( type );

        if ( type == "bool" ) return "Bool";
        if ( type == "int" || type == "int32_t" || type == "int16_t" || type == "int8_t" ||
             type == "long" ) return "Int";
        if ( type == "uint32_t" || type == "unsigned" || type == "unsigned int" || type == "uint16_t" ||
             type == "uint8_t" || type == "size_t" || type == "std::size_t" ) return "UInt";
        if ( type == "float" ) return "Float";
        if ( type == "double" ) return "Double";
        if ( type == "std::string" || type == "string" ) return "String";
        if ( type == "glm::vec2" || type == "vec2" ) return "Vec2";
        if ( type == "glm::vec3" || type == "vec3" ) return "Vec3";
        if ( type == "glm::vec4" || type == "vec4" ) return "Vec4";
        if ( type == "AssetHandle" || type == "Assets::AssetHandle" ||
             type == "Desert::Assets::AssetHandle" ) return "AssetHandle";
        return "Struct"; // unknown class type — resolved later by TypeName
    }

    std::string TrimCopy( std::string v )
    {
        while ( !v.empty() && std::isspace( (unsigned char)v.front() ) ) v.erase( v.begin() );
        while ( !v.empty() && std::isspace( (unsigned char)v.back() ) ) v.pop_back();
        return v;
    }

    // If `type` is a std::vector<...> spelling, returns the trimmed element type; else empty.
    std::string VectorElement( const std::string& typeRaw )
    {
        std::string t = TrimCopy( typeRaw );
        for ( const std::string& pre : { std::string( "std::vector<" ), std::string( "vector<" ) } )
        {
            if ( t.rfind( pre, 0 ) == 0 && !t.empty() && t.back() == '>' )
                return TrimCopy( t.substr( pre.size(), t.size() - pre.size() - 1 ) );
        }
        return "";
    }

    // Parses a single enumerator initializer ("= 2", "= 0x4", "= Other"). Falls back to the running
    // counter for anything it can't evaluate (expressions, shifts) — fine for editor display.
    long long EvalEnumInit( const std::string& exprRaw, long long running,
                            const std::vector<std::pair<std::string, long long>>& sofar )
    {
        std::string e = TrimCopy( exprRaw );
        if ( e.empty() ) return running;

        // PARSED WITHOUT EXCEPTIONS. This was std::stoll inside `catch ( ... ) {}` — a handler that is
        // indistinguishable from a forgotten one, and which also swallowed anything else the try block
        // might ever throw. std::from_chars reports "that was not a number" in its return value, which is
        // what this function actually wants to know: the symbolic lookup below is the answer, not an error.
        // The base prefix is stripped by hand because from_chars takes a base explicitly and has no
        // spelling for std::stoll's base-0 magic.
        std::string_view text     = e;
        bool             negative = false;
        int              base     = 10;
        if ( !text.empty() && ( text.front() == '-' || text.front() == '+' ) )
        {
            negative = text.front() == '-';
            text.remove_prefix( 1 );
        }
        if ( text.size() > 2 && text[0] == '0' && ( text[1] == 'x' || text[1] == 'X' ) )
        {
            base = 16;
            text.remove_prefix( 2 );
        }
        else if ( text.size() > 1 && text[0] == '0' )
        {
            base = 8;
            text.remove_prefix( 1 );
        }

        long long  value  = 0;
        const auto parsed = std::from_chars( text.data(), text.data() + text.size(), value, base );
        if ( parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() )
        {
            return negative ? -value : value;
        }
        for ( const auto& [n, val] : sofar )
            if ( n == e ) return val;
        return running;
    }

    void FlushEnumerator( const std::string& curRaw, EnumDef& def, long long& running )
    {
        std::string cur = TrimCopy( curRaw );
        if ( cur.empty() ) return;

        std::string name = cur, valExpr;
        if ( auto eq = cur.find( '=' ); eq != std::string::npos )
        {
            name    = TrimCopy( cur.substr( 0, eq ) );
            valExpr = cur.substr( eq + 1 );
        }
        if ( name.empty() ) return;

        const long long val = valExpr.empty() ? running : EvalEnumInit( valExpr, running, def.values );
        def.values.emplace_back( name, val );
        running = val + 1;
    }

    // Scans a header for enum/enum-class declarations, tracking namespace/struct scope to build each
    // enum's fully-qualified name. Runs on every header (enums rarely carry REFLECT()).
    void CollectEnums( const std::string& raw, std::vector<EnumDef>& out )
    {
        std::vector<std::pair<std::string, int>> scopes; // (name, brace depth)
        int                                      depth = 0;

        for ( size_t i = 0; i < raw.size(); )
        {
            char c = raw[i];
            if ( std::isspace( (unsigned char)c ) ) { ++i; continue; }

            if ( IsIdentChar( c ) )
            {
                std::string word = ReadQualifiedIdent( raw, i );

                if ( word == "namespace" )
                {
                    SkipWs( raw, i );
                    std::string ns = ReadQualifiedIdent( raw, i );
                    SkipWs( raw, i );
                    if ( i < raw.size() && raw[i] == '{' ) { ++i; ++depth; scopes.push_back( { ns, depth } ); }
                    continue;
                }
                if ( word == "enum" )
                {
                    SkipWs( raw, i );
                    size_t      save = i;
                    std::string kw   = ReadQualifiedIdent( raw, i ); // optional "class"/"struct"
                    if ( kw != "class" && kw != "struct" ) i = save;
                    else SkipWs( raw, i );

                    std::string name = ReadQualifiedIdent( raw, i );
                    while ( i < raw.size() && raw[i] != '{' && raw[i] != ';' ) ++i; // skip ": base"

                    if ( i < raw.size() && raw[i] == '{' )
                    {
                        ++i;
                        EnumDef def;
                        def.shortName = name;
                        std::string prefix;
                        for ( const auto& [n, d] : scopes )
                        {
                            if ( !prefix.empty() ) prefix += "::";
                            prefix += n;
                        }
                        def.fqn = prefix.empty() ? name : prefix + "::" + name;

                        long long   running = 0;
                        std::string cur;
                        int         edepth = 1;
                        while ( i < raw.size() && edepth > 0 )
                        {
                            char ec = raw[i];
                            if ( ec == '{' ) { ++edepth; cur += ec; ++i; continue; }
                            if ( ec == '}' )
                            {
                                --edepth;
                                ++i;
                                if ( edepth == 0 ) break;
                                cur += ec;
                                continue;
                            }
                            if ( ec == ',' && edepth == 1 ) { FlushEnumerator( cur, def, running ); cur.clear(); ++i; continue; }
                            cur += ec;
                            ++i;
                        }
                        FlushEnumerator( cur, def, running ); // last enumerator before '}'

                        if ( !name.empty() && !def.values.empty() ) out.push_back( std::move( def ) );
                    }
                    else if ( i < raw.size() && raw[i] == ';' ) { ++i; } // forward declaration
                    continue;
                }
                if ( word == "struct" || word == "class" )
                {
                    SkipWs( raw, i );
                    std::string tn = ReadQualifiedIdent( raw, i );
                    while ( i < raw.size() && raw[i] != '{' && raw[i] != ';' ) ++i;
                    if ( i < raw.size() && raw[i] == '{' ) { ++i; ++depth; scopes.push_back( { tn, depth } ); }
                    else if ( i < raw.size() && raw[i] == ';' ) { ++i; }
                    continue;
                }
                continue; // any other identifier
            }

            if ( c == '{' ) { ++depth; ++i; continue; }
            if ( c == '}' )
            {
                if ( !scopes.empty() && scopes.back().second == depth ) scopes.pop_back();
                --depth;
                ++i;
                continue;
            }
            ++i;
        }
    }

    // Finds an enum whose spelling matches a field's written type (exact FQN, bare short name, or the
    // trailing segment of a qualified spelling).
    const EnumDef* FindEnum( const std::vector<EnumDef>& enums, const std::string& typeSpelling )
    {
        std::string t = TrimCopy( typeSpelling );
        std::string tail = t;
        if ( auto pos = t.rfind( "::" ); pos != std::string::npos ) tail = t.substr( pos + 2 );

        for ( const auto& e : enums )
            if ( e.fqn == t || e.shortName == t || e.shortName == tail ) return &e;
        return nullptr;
    }

    Metadata ParseMetadata( const std::string& argsRaw )
    {
        Metadata m;
        for ( auto& tokRaw : SplitTopLevel( argsRaw ) )
        {
            std::string tok = tokRaw;
            // trim
            while ( !tok.empty() && std::isspace( (unsigned char)tok.front() ) ) tok.erase( tok.begin() );
            while ( !tok.empty() && std::isspace( (unsigned char)tok.back() ) ) tok.pop_back();
            if ( tok.empty() ) continue;

            if ( tok.rfind( "DisplayName", 0 ) == 0 )      m.displayName = ExtractStringLiteral( tok );
            else if ( tok.rfind( "Category", 0 ) == 0 )    m.category    = ExtractStringLiteral( tok );
            else if ( tok.rfind( "Tooltip", 0 ) == 0 )     m.tooltip     = ExtractStringLiteral( tok );
            else if ( tok.rfind( "Header", 0 ) == 0 )      m.header      = ExtractStringLiteral( tok );
            else if ( tok.rfind( "Range", 0 ) == 0 )
            {
                auto a = tok.find( '(' ), b = tok.rfind( ')' );
                if ( a != std::string::npos && b != std::string::npos && b > a )
                {
                    auto parts = SplitTopLevel( tok.substr( a + 1, b - a - 1 ) );
                    if ( parts.size() == 2 )
                    {
                        auto t = []( std::string v ) {
                            while ( !v.empty() && std::isspace( (unsigned char)v.front() ) ) v.erase( v.begin() );
                            while ( !v.empty() && std::isspace( (unsigned char)v.back() ) ) v.pop_back();
                            return v;
                        };
                        m.hasRange = true;
                        m.rangeMin = t( parts[0] );
                        m.rangeMax = t( parts[1] );
                    }
                }
            }
            else if ( tok.rfind( "Asset", 0 ) == 0 )
            {
                m.isAsset = true;
                auto a = tok.find_first_of( "<(" );
                auto b = tok.find_last_of( ">)" );
                if ( a != std::string::npos && b != std::string::npos && b > a )
                    m.assetType = tok.substr( a + 1, b - a - 1 );
            }
            else if ( tok.rfind( "Units", 0 ) == 0 )
                m.units = ExtractStringLiteral( tok );
            else if ( tok == "Color" )     m.isColor = true;
            else if ( tok == "Thumbnail" ) m.thumbnail = true;
            else if ( tok == "ReadOnly" )  m.readOnly = true;
            else if ( tok == "Hidden" )    m.hidden = true;
            else if ( tok == "Advanced" )
                m.advanced = true;
            else if ( tok == "Summary" )
                m.summary = true;
            else if ( tok == "Temperature" )
                m.temperature = true;
            else if ( tok == "Preview" )
                m.preview = true;
            else if ( tok.rfind( "EditCondition", 0 ) == 0 )
                m.editCondition = ExtractStringLiteral( tok );
            else if ( tok == "Length" )
                m.isLength = true;
        }
        return m;
    }

    // --------------------------------------------------------------------- parser

    struct Scope
    {
        std::string name;     // namespace or struct name
        int         depth;    // brace depth at which this scope opened
        bool        isStruct; // struct/class vs namespace
        bool        reflected = false;
        std::vector<Field> fields =
             {}; // the two `scopes.push_back( { name, depth, isStruct } )` below stop here on purpose
        std::vector<Function> functions = {};
        size_t                        open      = 0;  // the position just after the struct's '{'
        std::optional<ComponentBlock> component = {}; // its COMPONENT(...) marker, if any
    };

    // FUNCTION( ScriptCallable, Category( "..." ), Tooltip( "..." ) ). An unknown token is an error, not a
    // silently ignored attribute: a misspelt ScriptCallable would otherwise hide the function from every language.
    FunctionMeta ParseFunctionMeta( const std::string& argsRaw, std::string& error )
    {
        FunctionMeta m;
        for ( const auto& tokRaw : SplitTopLevel( argsRaw ) )
        {
            const std::string tok = Trimmed( tokRaw );
            if ( tok.empty() )
                continue;
            if ( tok == "ScriptCallable" )
                m.scriptCallable = true;
            else if ( tok.rfind( "Category", 0 ) == 0 )
                m.category = ExtractStringLiteral( tok );
            else if ( tok.rfind( "Tooltip", 0 ) == 0 )
                m.tooltip = ExtractStringLiteral( tok );
            else
                error = "FUNCTION: unknown attribute '" + tok + "' (ScriptCallable, Category(\"...\"), Tooltip(\"...\"))";
        }
        return m;
    }

    // The trailing identifier of `text` ("const std::string& name" -> "name"), or empty.
    std::string TrailingIdent( const std::string& text )
    {
        size_t e = text.size();
        size_t b = e;
        while ( b > 0 && IsIdentChar( text[b - 1] ) )
            --b;
        return text.substr( b, e - b );
    }

    // Reads a FUNCTION-annotated declaration starting at `start` ("[[nodiscard]] static float Name( int a ) const
    // { ... }" or "...;"). Returns the index just past it (past the body's closing brace when it has one).
    size_t ParseFunctionDecl( const std::string& raw, size_t start, Function& fn, std::string& error )
    {
        const size_t open = raw.find( '(', start );
        if ( open == std::string::npos )
        {
            error = "FUNCTION: no declaration follows";
            return raw.size();
        }
        std::string head = raw.substr( start, open - start );
        if ( const size_t attr = head.rfind( "]]" ); attr != std::string::npos )
            head = head.substr( attr + 2 );
        head    = Trimmed( head );
        fn.name = TrailingIdent( head );
        std::string ret = Trimmed( head.substr( 0, head.size() - fn.name.size() ) );
        for ( const char* specifier : { "static ", "virtual ", "inline ", "constexpr ", "explicit " } )
            while ( ret.rfind( specifier, 0 ) == 0 )
                ret = Trimmed( ret.substr( std::string_view( specifier ).size() ) );
        fn.returnType = ret;
        if ( fn.name.empty() || fn.returnType.empty() )
        {
            error = "FUNCTION: expected '<return type> <name>(' after the annotation";
            return raw.size();
        }

        size_t close = open;
        for ( int p = 0; close < raw.size(); ++close )
        {
            if ( raw[close] == '(' )
                ++p;
            else if ( raw[close] == ')' && --p == 0 )
                break;
        }
        const std::string list = Trimmed( raw.substr( open + 1, close - open - 1 ) );
        if ( !list.empty() && list != "void" )
        {
            for ( const auto& paramRaw : SplitTopLevel( list ) )
            {
                std::string param = paramRaw;
                if ( const size_t eq = param.find( '=' ); eq != std::string::npos )
                    param = param.substr( 0, eq );
                param                   = Trimmed( param );
                const std::string pname = TrailingIdent( param );
                const std::string ptype = Trimmed( param.substr( 0, param.size() - pname.size() ) );
                if ( pname.empty() || ptype.empty() )
                {
                    error = "FUNCTION " + fn.name + ": parameter '" + param +
                            "' has no name (a caller sees every parameter by its name)";
                    return raw.size();
                }
                fn.params.emplace_back( pname, ptype );
            }
        }

        size_t j = close + 1;
        while ( j < raw.size() && raw[j] != ';' && raw[j] != '{' )
            ++j;
        if ( j < raw.size() && raw[j] == '{' )
        {
            for ( int b = 0; j < raw.size(); ++j )
            {
                if ( raw[j] == '{' )
                    ++b;
                else if ( raw[j] == '}' && --b == 0 )
                    break;
            }
        }
        return j + 1;
    }

    std::string JoinScopes( const std::vector<Scope>& scopes )
    {
        std::string fqn;
        for ( const auto& s : scopes )
        {
            if ( s.name.empty() ) continue;
            if ( !fqn.empty() ) fqn += "::";
            fqn += s.name;
        }
        return fqn;
    }

    void ParseFile( const fs::path& file, const fs::path& sourceRoot, std::vector<ReflectedType>& out,
                    std::vector<ComponentBlock>& components, const std::vector<EnumDef>& enums,
                    std::vector<std::string>& errors )
    {
        const std::string raw = StripComments( ReadFile( file ) );
        if ( raw.find( "REFLECT()" ) == std::string::npos && raw.find( "COMPONENT(" ) == std::string::npos )
            return;

        std::string headerInclude =
             fs::relative( file, sourceRoot ).generic_string();

        std::vector<Scope> scopes;
        int depth = 0;

        Metadata pendingMeta;
        bool     hasPendingMeta = false;

        FunctionMeta pendingFunction;
        bool         hasPendingFunction = false;
        const auto   fail = [&]( size_t at, const std::string& message )
        {
            const auto line = std::count( raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>( at ), '\n' ) + 1;
            errors.push_back( file.generic_string() + ":" + std::to_string( line ) + ": " + message );
        };

        for ( size_t i = 0; i < raw.size(); )
        {
            char c = raw[i];

            if ( std::isspace( (unsigned char)c ) ) { ++i; continue; }

            // A preprocessor directive is not a declaration: `#define FUNCTION( ... )` in ReflectionMacros.hpp
            // is not an annotation. Skipped to the end of its (backslash-continued) line.
            if ( c == '#' )
            {
                while ( i < raw.size() && raw[i] != '\n' )
                    i += ( raw[i] == '\\' && i + 1 < raw.size() ) ? 2 : 1;
                continue;
            }

            // identifiers / keywords
            if ( IsIdentChar( c ) )
            {
                size_t start = i;
                std::string word = ReadQualifiedIdent( raw, i );

                if ( word == "namespace" )
                {
                    SkipWs( raw, i );
                    std::string nsName = ReadQualifiedIdent( raw, i );
                    SkipWs( raw, i );
                    if ( i < raw.size() && raw[i] == '{' )
                    {
                        ++i; ++depth;
                        scopes.push_back( { nsName, depth, false } );
                    }
                    continue;
                }
                if ( word == "struct" || word == "class" )
                {
                    SkipWs( raw, i );
                    std::string typeName = ReadQualifiedIdent( raw, i );
                    // skip optional base-clause / attributes up to { or ;
                    while ( i < raw.size() && raw[i] != '{' && raw[i] != ';' ) ++i;
                    if ( i < raw.size() && raw[i] == '{' )
                    {
                        ++i; ++depth;
                        scopes.push_back( { typeName, depth, true } );
                        scopes.back().open = i;
                    }
                    else if ( i < raw.size() && raw[i] == ';' )
                    {
                        ++i; // forward declaration
                    }
                    continue;
                }
                if ( word == "REFLECT" )
                {
                    SkipWs( raw, i );
                    if ( i < raw.size() && raw[i] == '(' )
                    {
                        // consume ()
                        int p = 0;
                        do { if ( raw[i] == '(' ) ++p; else if ( raw[i] == ')' ) --p; ++i; }
                        while ( i < raw.size() && p > 0 );
                    }
                    if ( !scopes.empty() && scopes.back().isStruct )
                        scopes.back().reflected = true;
                    continue;
                }
                if ( word == "PROPERTY" )
                {
                    SkipWs( raw, i );
                    std::string args;
                    if ( i < raw.size() && raw[i] == '(' )
                    {
                        int p = 0; size_t s0 = i;
                        do { if ( raw[i] == '(' ) ++p; else if ( raw[i] == ')' ) --p; ++i; }
                        while ( i < raw.size() && p > 0 );
                        // contents between the outer parens
                        args = raw.substr( s0 + 1, ( i - 1 ) - ( s0 + 1 ) );
                    }
                    pendingMeta = ParseMetadata( args );
                    hasPendingMeta = true;
                    continue;
                }

                if ( word == "FUNCTION" )
                {
                    SkipWs( raw, i );
                    std::string args;
                    if ( i < raw.size() && raw[i] == '(' )
                    {
                        int    p  = 0;
                        size_t s0 = i;
                        do
                        {
                            if ( raw[i] == '(' )
                                ++p;
                            else if ( raw[i] == ')' )
                                --p;
                            ++i;
                        } while ( i < raw.size() && p > 0 );
                        args = raw.substr( s0 + 1, ( i - 1 ) - ( s0 + 1 ) );
                    }
                    if ( scopes.empty() || !scopes.back().isStruct || !scopes.back().reflected )
                    {
                        fail( start, "FUNCTION outside a REFLECT() type" );
                        continue;
                    }
                    std::string error;
                    pendingFunction = ParseFunctionMeta( args, error );
                    if ( !error.empty() )
                        fail( start, error );
                    hasPendingFunction = true;
                    continue;
                }
                if ( word == "COMPONENT" )
                {
                    SkipWs( raw, i );
                    if ( i >= raw.size() || raw[i] != '(' )
                        continue; // the word, not the marker
                    const size_t s0 = i;
                    int          p  = 0;
                    do
                    {
                        if ( raw[i] == '(' )
                            ++p;
                        else if ( raw[i] == ')' )
                            --p;
                        ++i;
                    } while ( i < raw.size() && p > 0 );
                    const std::string args = raw.substr( s0 + 1, ( i - 1 ) - ( s0 + 1 ) );
                    if ( scopes.empty() || !scopes.back().isStruct )
                    {
                        fail( start, "COMPONENT outside a struct" );
                        continue;
                    }
                    if ( scopes.back().component.has_value() )
                    {
                        fail( start, "COMPONENT twice in " + scopes.back().name );
                        continue;
                    }
                    std::string    error;
                    ComponentBlock c = Desert::HeaderTool::ParseComponentMeta( args, error );
                    if ( !error.empty() )
                    {
                        fail( start, error );
                        continue;
                    }
                    const auto line =
                         std::count( raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>( start ), '\n' ) + 1;
                    c.where                 = file.generic_string() + ":" + std::to_string( line );
                    c.headerInclude         = headerInclude;
                    scopes.back().component = std::move( c );
                    continue;
                }
                if ( hasPendingFunction )
                {
                    Function    fn;
                    std::string error;
                    fn.meta = pendingFunction;
                    i       = ParseFunctionDecl( raw, start, fn, error );
                    hasPendingFunction = false;
                    if ( !error.empty() )
                    {
                        fail( start, error );
                        continue;
                    }
                    auto& functions = scopes.back().functions;
                    if ( std::any_of( functions.begin(), functions.end(),
                                      [&]( const Function& other ) { return other.name == fn.name; } ) )
                    {
                        fail( start, "FUNCTION " + fn.name +
                                          " is declared twice in this type (no overloads: a caller calls by name)" );
                        continue;
                    }
                    functions.push_back( std::move( fn ) );
                    continue;
                }

                // Some other identifier. If we have a pending PROPERTY and we're directly inside a
                // reflected struct, this begins the annotated field declaration: read "<type...> name"
                // up to ';' / '=' / '{'.
                if ( hasPendingMeta && !scopes.empty() && scopes.back().isStruct && scopes.back().reflected )
                {
                    size_t declStart = start;
                    size_t j = declStart;
                    while ( j < raw.size() && raw[j] != ';' && raw[j] != '=' && raw[j] != '{' ) ++j;
                    std::string decl = raw.substr( declStart, j - declStart );

                    // split into type + last identifier (the field name)
                    // strip array suffix
                    auto br = decl.find( '[' );
                    if ( br != std::string::npos ) decl = decl.substr( 0, br );
                    // trim
                    while ( !decl.empty() && std::isspace( (unsigned char)decl.back() ) ) decl.pop_back();

                    size_t namePos = decl.find_last_of( " \t" );
                    if ( namePos != std::string::npos )
                    {
                        std::string fieldName = decl.substr( namePos + 1 );
                        std::string typeName  = decl.substr( 0, namePos );
                        while ( !typeName.empty() && std::isspace( (unsigned char)typeName.back() ) ) typeName.pop_back();
                        while ( !fieldName.empty() && std::isspace( (unsigned char)fieldName.front() ) ) fieldName.erase( fieldName.begin() );

                        Field f;
                        f.name      = fieldName;
                        f.cppType   = typeName;
                        f.fieldType = MapFieldType( typeName );
                        f.meta      = pendingMeta;
                        // Asset metadata implies AssetHandle field type even if spelled generically.
                        // A vector of them (Asset<T> on std::vector<AssetHandle>) is a container of handles whose
                        // elements are references of that asset type, not one handle.
                        const std::string assetElem = VectorElement( typeName );
                        if ( f.meta.isAsset && !assetElem.empty() && MapFieldType( assetElem ) == "AssetHandle" )
                        {
                            f.isContainer   = true;
                            f.elemFieldType = "AssetHandle";
                        }
                        else if ( f.meta.isAsset )
                        {
                            f.fieldType = "AssetHandle";
                        }
                        else if ( f.fieldType == "Struct" )
                        {
                            // Unknown class spelling — enum, or a std::vector<...> container.
                            if ( const EnumDef* e = FindEnum( enums, typeName ) )
                            {
                                f.fieldType  = "Enum";
                                f.enumValues = e->values;
                            }
                            else if ( std::string elem = VectorElement( typeName ); !elem.empty() )
                            {
                                std::string et = MapFieldType( elem );
                                // Supported element types (scalars + asset handle). vector<struct/vec> later.
                                if ( et == "AssetHandle" || et == "Bool" || et == "Int" || et == "UInt" ||
                                     et == "Float" || et == "Double" || et == "String" )
                                {
                                    f.isContainer   = true;
                                    f.elemFieldType = et;
                                }
                            }
                        }
                        scopes.back().fields.push_back( f );
                    }

                    hasPendingMeta = false;
                    i = j; // continue from the delimiter
                }
                continue;
            }

            // braces
            if ( c == '{' ) { ++depth; ++i; continue; }
            if ( c == '}' )
            {
                // closing the current scope?
                if ( !scopes.empty() && scopes.back().depth == depth )
                {
                    Scope sc = scopes.back();
                    scopes.pop_back();
                    if ( sc.isStruct && sc.component.has_value() )
                    {
                        ComponentBlock c = std::move( *sc.component );
                        c.fqn = JoinScopes( scopes ).empty() ? sc.name : JoinScopes( scopes ) + "::" + sc.name;
                        if ( !c.member.empty() )
                        {
                            // The member's declared type, from the struct's own body: "<type> <Member> ;|=|{".
                            const std::string body = raw.substr( sc.open, i - sc.open );
                            if ( auto type = Desert::HeaderTool::DeclaredMemberType( body, c.member ) )
                                c.memberType = std::move( *type );
                            else
                                errors.push_back( c.where + ": COMPONENT " + c.key + ": no member '" + c.member +
                                                  "' declared in " + sc.name );
                        }
                        components.push_back( std::move( c ) );
                    }
                    if ( sc.isStruct && sc.reflected && ( !sc.fields.empty() || !sc.functions.empty() ) )
                    {
                        ReflectedType t;
                        t.registryName  = sc.name;
                        t.fqn           = JoinScopes( scopes ).empty()
                                             ? sc.name
                                             : JoinScopes( scopes ) + "::" + sc.name;
                        t.fields        = sc.fields;
                        t.functions     = sc.functions;
                        t.headerInclude = headerInclude;
                        out.push_back( std::move( t ) );
                    }
                }
                --depth; ++i; continue;
            }

            ++i; // any other punctuation
        }
    }

    // --------------------------------------------------------------------- generator

    // The text lives in Templates/*.tpl; this is only the data they are rendered against. A property enters the
    // `meta` object only when it is set, so a template's `{% if f.meta.x %}` is the whole emission rule.
    Common::Json::Value MetadataModel( const Metadata& m )
    {
        Common::Json::ObjectBuilder b;
        const auto                  text = [&b]( std::string_view key, const std::string& value )
        {
            if ( !value.empty() )
                b.Set( key, value );
        };
        const auto flag = [&b]( std::string_view key, bool value )
        {
            if ( value )
                b.Set( key, true );
        };
        text( "displayName", m.displayName );
        text( "category", m.category );
        text( "tooltip", m.tooltip );
        text( "header", m.header );
        if ( m.hasRange )
            b.Set( "hasRange", true ).Set( "rangeMin", m.rangeMin ).Set( "rangeMax", m.rangeMax );
        flag( "isColor", m.isColor );
        if ( m.isAsset )
            b.Set( "isAsset", true ).Set( "assetType", m.assetType );
        flag( "thumbnail", m.thumbnail );
        flag( "readOnly", m.readOnly );
        flag( "hidden", m.hidden );
        flag( "isLength", m.isLength );
        text( "units", m.units );
        flag( "advanced", m.advanced );
        flag( "summary", m.summary );
        flag( "temperature", m.temperature );
        flag( "preview", m.preview );
        text( "editCondition", m.editCondition );
        return b.Build();
    }

    Common::Json::Value FieldModel( const Field& f )
    {
        Common::Json::ObjectBuilder b;
        b.Set( "name", f.name )
             .Set( "fieldType", f.fieldType )
             .Set( "cppType", f.cppType )
             .Set( "meta", MetadataModel( f.meta ) );
        if ( f.fieldType == "Enum" && !f.enumValues.empty() )
        {
            Common::Json::Value::Array values;
            for ( const auto& [name, value] : f.enumValues )
                values.emplace_back(
                     Common::Json::ObjectBuilder().Set( "name", name ).Set( "value", value ).Build() );
            b.Set( "enumValues", Common::Json::Value( std::move( values ) ) );
        }
        if ( f.isContainer )
            b.Set( "isContainer", true ).Set( "elemFieldType", f.elemFieldType );
        return b.Build();
    }

    Common::Json::Value FunctionModel( const Function& fn )
    {
        Common::Json::Value::Array params;
        for ( const auto& [name, cppType] : fn.params )
            params.emplace_back( Common::Json::ObjectBuilder().Set( "name", name ).Set( "cppType", cppType ).Build() );
        return Common::Json::ObjectBuilder()
             .Set( "name", fn.name )
             .Set( "returnType", fn.returnType )
             .Set( "paramCount", static_cast<long long>( fn.params.size() ) )
             .Set( "params", Common::Json::Value( std::move( params ) ) )
             .Set( "scriptCallable", std::string( fn.meta.scriptCallable ? "true" : "false" ) )
             .Set( "category", fn.meta.category )
             .Set( "tooltip", fn.meta.tooltip )
             .Build();
    }

    Common::Json::Value ReflectionModel( const std::string& module, const std::vector<ReflectedType>& types )
    {
        // Each header is included once, in first-use order.
        std::vector<std::string> includes;
        for ( const auto& t : types )
            if ( std::find( includes.begin(), includes.end(), t.headerInclude ) == includes.end() )
                includes.push_back( t.headerInclude );
        Common::Json::Value::Array includeValues( includes.begin(), includes.end() );

        Common::Json::Value::Array typeValues;
        bool                       hasFunctions = false;
        for ( const auto& t : types )
        {
            Common::Json::Value::Array fields;
            for ( const auto& f : t.fields )
                fields.push_back( FieldModel( f ) );
            Common::Json::Value::Array functions;
            for ( const auto& fn : t.functions )
                functions.push_back( FunctionModel( fn ) );
            hasFunctions = hasFunctions || !t.functions.empty();
            typeValues.emplace_back( Common::Json::ObjectBuilder()
                                          .Set( "fqn", t.fqn )
                                          .Set( "registryName", t.registryName )
                                          .Set( "fields", Common::Json::Value( std::move( fields ) ) )
                                          .Set( "functions", Common::Json::Value( std::move( functions ) ) )
                                          .Build() );
        }
        return Common::Json::ObjectBuilder()
             .Set( "module", module )
             .Set( "includes", Common::Json::Value( std::move( includeValues ) ) )
             .Set( "types", Common::Json::Value( std::move( typeValues ) ) )
             .Set( "hasFunctions", hasFunctions )
             .Build();
    }

    // Every *.tpl in `dir` under its file name, so `{% include "Field.tpl" %}` resolves inside the same set.
    // No directory, no entry template or a template that does not compile is an error with the path.
    Common::ResultStr<std::string> RenderTemplate( const fs::path& dir, std::string_view entry,
                                                   const Common::Json::Value& model )
    {
        if ( !fs::is_directory( dir ) )
            return Common::MakeError<std::string>( "template directory does not exist: " + dir.string() );
        Common::Text::TemplateSet set;
        for ( const auto& file : fs::directory_iterator( dir ) )
        {
            if ( !file.is_regular_file() || file.path().extension() != ".tpl" )
                continue;
            if ( auto added = set.Add( file.path().filename().string(), ReadFile( file.path() ) );
                 !added.IsSuccess() )
                return Common::MakeError<std::string>( file.path().string() + ": " + added.GetError() );
        }
        const Common::Text::Template* root = set.Find( entry );
        if ( root == nullptr )
            return Common::MakeError<std::string>( "entry template missing: " + ( dir / entry ).string() );
        return Common::Text::Render( *root, Common::Json::Root( model ), set.Loader() );
    }

    Common::Json::Value SubsystemsModel( const std::string& ownerType, const std::string& ownerHeader,
                                         const std::vector<Desert::HeaderTool::SubsystemDeclaration>& subsystems )
    {
        Common::Json::Value::Array includes;
        Common::Json::Value::Array names;
        std::set<std::string>      included;
        for ( const auto& subsystem : subsystems )
        {
            if ( included.insert( subsystem.Include ).second )
                includes.emplace_back( subsystem.Include );
            names.emplace_back( subsystem.QualifiedName );
        }
        const size_t scope = ownerType.rfind( "::" );
        return Common::Json::ObjectBuilder()
             .Set( "ownerType", ownerType )
             .Set( "ownerNamespace", scope == std::string::npos ? std::string() : ownerType.substr( 0, scope ) )
             .Set( "ownerHeader", ownerHeader )
             .Set( "includes", Common::Json::Value( std::move( includes ) ) )
             .Set( "subsystems", Common::Json::Value( std::move( names ) ) )
             .Build();
    }

    std::vector<Desert::HeaderTool::ScannedFile> GatherSources( const fs::path& root, bool checked )
    {
        std::vector<Desert::HeaderTool::ScannedFile> files;
        for ( const auto& entry : fs::recursive_directory_iterator( root ) )
        {
            if ( !entry.is_regular_file() )
                continue;
            const auto ext = entry.path().extension().string();
            if ( ext != ".hpp" && ext != ".h" && ext != ".cpp" && ext != ".mm" )
                continue;
            if ( entry.path().filename().string().find( ".gen." ) != std::string::npos )
                continue;
            files.push_back( { entry.path(), ReadFile( entry.path() ), checked,
                               fs::relative( entry.path(), root ).generic_string() } );
        }
        return files;
    }

    Common::BoolResultStr WriteIfChanged( const fs::path& outputFile, const std::string& content )
    {
        if ( fs::exists( outputFile ) && ReadFile( outputFile ) == content )
            return Common::MakeSuccess( false );
        fs::create_directories( outputFile.parent_path() );
        Common::BoolResultStr saved = Common::Utils::FileSystem::WriteContentToFileAtomic( outputFile, content );
        if ( !saved.IsSuccess() )
            return saved;
        return Common::MakeSuccess( true );
    }

    Common::Json::Value ComponentsModel( const std::vector<ComponentBlock>& components )
    {
        std::vector<std::string> includes;
        for ( const auto& c : components )
            if ( std::find( includes.begin(), includes.end(), c.headerInclude ) == includes.end() )
                includes.push_back( c.headerInclude );
        std::sort( includes.begin(), includes.end() );
        Common::Json::Value::Array includeValues( includes.begin(), includes.end() );

        Common::Json::Value::Array rows;
        for ( const auto& c : components )
            rows.emplace_back( Common::Json::ObjectBuilder()
                                    .Set( "fqn", c.fqn )
                                    .Set( "key", c.key )
                                    .Set( "member", c.member )
                                    .Set( "whole", c.member.empty() )
                                    .Set( "typeName", c.typeName )
                                    .Set( "run", c.run )
                                    .Build() );
        return Common::Json::ObjectBuilder()
             .Set( "includes", Common::Json::Value( std::move( includeValues ) ) )
             .Set( "components", Common::Json::Value( std::move( rows ) ) )
             .Build();
    }

    struct ReflectRequest
    {
        fs::path SourceRoot;
        fs::path ScanRoot;
        fs::path Output; // the module list; the per-module files are written beside it
        // The force-link function the module list defines. The engine's is ForceLinkGeneratedReflection; a second
        // generated set linked into the same image (a test fixture's) names its own (--reflect-anchor).
        std::string Anchor = "ForceLinkGeneratedReflection";
        // Where the COMPONENT(...) rows are emitted (--reflect-components); empty: not requested.
        fs::path Components;
    };

    constexpr std::string_view kModuleFilePrefix = "Reflection_";
    constexpr std::string_view kModuleFileSuffix = ".gen.cpp";

    // Renders `entry` with `model` into `output` when its text changed; false on an error, already reported.
    bool Emit( const fs::path& templateDir, std::string_view entry, const Common::Json::Value& model,
               const fs::path& output )
    {
        auto rendered = RenderTemplate( templateDir, entry, model );
        if ( !rendered.IsSuccess() )
        {
            std::cerr << "[DesertHeaderTool] " << rendered.GetError() << "\n";
            return false;
        }
        const Common::BoolResultStr written = WriteIfChanged( output, rendered.ExtractValue() );
        if ( !written.IsSuccess() )
        {
            std::cerr << "[DesertHeaderTool] " << written.GetError() << "\n";
            return false;
        }
        if ( written.GetValue() )
            std::cout << std::format( "[DesertHeaderTool] generated {}\n", output.string() );
        return true;
    }

    struct SubsystemsRequest
    {
        std::string Owner;
        std::string OwnerType;
        std::string OwnerHeader;
        fs::path    Output;
    };

    // `modules` null: no module table (a test fixture), the scanned set is one module named by the output's stem.
    int Reflect( const fs::path& templateDir, const ReflectRequest& request,
                 const Desert::HeaderTool::ModuleTable* modules, const fs::path& repoRoot )
    {
        if ( !fs::exists( request.ScanRoot ) )
        {
            std::cerr << "[DesertHeaderTool] scan root does not exist: " << request.ScanRoot << "\n";
            return 1;
        }
        std::vector<fs::path> headers;
        for ( const auto& entry : fs::recursive_directory_iterator( request.ScanRoot ) )
        {
            if ( !entry.is_regular_file() )
                continue;
            const auto ext = entry.path().extension().string();
            if ( ext != ".hpp" && ext != ".h" )
                continue;
            if ( entry.path().filename().string().find( ".gen." ) != std::string::npos )
                continue;
            headers.push_back( entry.path() );
        }

        // Pass 1: every enum definition, so a reflected field can reference an enum from any header.
        std::vector<EnumDef> enums;
        for ( const auto& h : headers )
            CollectEnums( StripComments( ReadFile( h ) ), enums );

        // Pass 2: reflected types, resolving enum field types against the collected enums.
        std::vector<ReflectedType>  types;
        std::vector<ComponentBlock> components;
        std::vector<std::string>    errors;
        for ( const auto& h : headers )
            ParseFile( h, request.SourceRoot, types, components, enums, errors );
        std::vector<Desert::HeaderTool::ReflectedTypeName> typeNames;
        typeNames.reserve( types.size() );
        for ( const ReflectedType& t : types )
            typeNames.push_back( { t.fqn, t.registryName } );
        Desert::HeaderTool::ResolveComponents( typeNames, components, errors );
        for ( const std::string& error : errors )
            std::cerr << error << "\n";
        if ( !errors.empty() )
            return 1;

        // Each type to its module, by the header's place in the repository (plan C11). Without a module table
        // (a test fixture) the scanned set is one module named by the output's stem.
        std::map<std::string, std::vector<ReflectedType>> byModule;
        std::vector<std::string> moduleNames;
        if ( modules == nullptr )
        {
            const std::string fileName = request.Output.filename().string();
            moduleNames.push_back( fileName.substr( 0, fileName.find( '.' ) ) );
        }
        else
            for ( const auto& module : modules->Modules() )
                moduleNames.push_back( module.Name );
        for ( auto& t : types )
        {
            if ( modules == nullptr )
            {
                t.module = moduleNames.front();
                byModule[t.module].push_back( t );
                continue;
            }
            const fs::path header = fs::relative( request.SourceRoot / t.headerInclude, repoRoot );
            t.module              = modules->ModuleOf( header.generic_string() );
            if ( t.module.empty() )
            {
                std::cerr << std::format( "[DesertHeaderTool] {}: {} belongs to no module of the module table\n",
                                          header.generic_string(), t.fqn );
                return 1;
            }
            byModule[t.module].push_back( t );
        }

        const fs::path        outputDir = request.Output.parent_path();
        std::set<std::string> written;
        // EVERY module of the table gets its file, the ones without a reflected type an empty registration: the
        // set of files is then fixed by the table, which premake reads, so the source glob premake expands at
        // generation time always holds the file a newly reflected type lands in.
        Common::Json::Value::Array listed; // in the module table's order: dependencies register first
        for ( const std::string& module : moduleNames )
        {
            const std::string file = std::format( "{}{}{}", kModuleFilePrefix, module, kModuleFileSuffix );
            if ( !Emit( templateDir, "ReflectionModule.gen.cpp.tpl", ReflectionModel( module, byModule[module] ),
                        outputDir / file ) )
                return 1;
            written.insert( file );
            listed.emplace_back( module );
        }
        if ( !Emit( templateDir, "Reflection.gen.cpp.tpl",
                    Common::Json::ObjectBuilder()
                         .Set( "modules", Common::Json::Value( std::move( listed ) ) )
                         .Set( "anchor", request.Anchor )
                         .Build(),
                    request.Output ) )
            return 1;

        // A module that left the table leaves no file behind for the source glob to compile.
        for ( const auto& entry : fs::directory_iterator( outputDir ) )
        {
            const std::string name = entry.path().filename().string();
            if ( name.starts_with( kModuleFilePrefix ) && name.ends_with( kModuleFileSuffix ) && !written.contains( name ) )
            {
                fs::remove( entry.path() );
                std::cout << std::format( "[DesertHeaderTool] removed {} (its module is not in the module table)\n",
                                          entry.path().string() );
            }
        }
        std::cout << std::format( "[DesertHeaderTool] {} reflected types in {} modules, {} headers scanned\n", types.size(),
                                  written.size(), headers.size() );
        if ( request.Components.empty() )
            return 0;

        if ( !Emit( templateDir, "ReflectedComponentBlocks.gen.hpp.tpl", ComponentsModel( components ),
                    request.Components ) )
            return 1;
        std::cout << std::format( "[DesertHeaderTool] {} component blocks\n", components.size() );
        return 0;
    }
} // namespace

static int RunTool( int argc, char** argv )
{
    std::optional<fs::path>        templateDir;
    std::optional<ReflectRequest>  reflect;
    std::optional<fs::path>        moduleTable;
    std::vector<SubsystemsRequest> subsystemRequests;
    std::vector<fs::path>          checkRoots;
    std::vector<fs::path>          contextRoots;
    for ( int i = 1; i < argc; ++i )
    {
        const std::string_view arg  = argv[i];
        const int              left = argc - i - 1;
        if ( arg == "--templates" && left >= 1 )
            templateDir = argv[++i];
        else if ( arg == "--modules" && left >= 1 )
            moduleTable = argv[++i];
        else if ( arg == "--check" && left >= 1 )
            checkRoots.emplace_back( argv[++i] );
        else if ( arg == "--context" && left >= 1 )
            contextRoots.emplace_back( argv[++i] );
        else if ( arg == "--reflect" && left >= 3 )
        {
            const fs::path root = argv[i + 1];
            reflect             = ReflectRequest{ root, root / argv[i + 2], argv[i + 3] };
            i += 3;
        }
        else if ( arg == "--reflect-anchor" && left >= 1 && reflect )
            reflect->Anchor = argv[++i];
        else if ( arg == "--reflect-components" && left >= 1 && reflect )
            reflect->Components = argv[++i];
        else if ( arg == "--subsystems" && left >= 4 )
        {
            subsystemRequests.push_back( { argv[i + 1], argv[i + 2], argv[i + 3], argv[i + 4] } );
            i += 4;
        }
        else
        {
            std::cerr << "[DesertHeaderTool] unknown argument: " << arg << "\n";
            templateDir.reset();
            break;
        }
    }
    if ( !templateDir || ( !reflect && checkRoots.empty() ) )
    {
        std::cerr << "Usage: DesertHeaderTool --templates <dir> [--modules <DesertModules.lua>]\n"
                     "       [--reflect <source-root> <scan-subdir> <output> [--reflect-anchor <Name>]\n"
                     "                  [--reflect-components <output>]]\n"
                     "       [--check <include-root>]... [--context <include-root>]...\n"
                     "       [--subsystems <Owner> <OwnerType> <owner-header> <output>]...\n";
        return 1;
    }

    std::vector<Desert::HeaderTool::ScannedFile> files;
    for ( const auto& [roots, checked] : { std::pair{ &checkRoots, true }, std::pair{ &contextRoots, false } } )
    {
        for ( const fs::path& root : *roots )
        {
            if ( !fs::is_directory( root ) )
            {
                std::cerr << "[DesertHeaderTool] root does not exist: " << root << "\n";
                return 1;
            }
            auto gathered = GatherSources( root, checked );
            files.insert( files.end(), std::make_move_iterator( gathered.begin() ),
                          std::make_move_iterator( gathered.end() ) );
        }
    }

    const Desert::HeaderTool::HeaderModel model = Desert::HeaderTool::ScanHeaders( files );
    for ( const auto& error : model.Errors )
        std::cerr << Desert::HeaderTool::FormatDiagnostic( error ) << "\n";
    if ( !model.Errors.empty() )
        return 1;

    for ( const SubsystemsRequest& request : subsystemRequests )
    {
        std::vector<Desert::HeaderTool::SubsystemDeclaration> owned;
        std::copy_if( model.Subsystems.begin(), model.Subsystems.end(), std::back_inserter( owned ),
                      [&]( const auto& subsystem ) { return subsystem.Owner == request.Owner; } );
        auto rendered = RenderTemplate( *templateDir, "Subsystems.gen.cpp.tpl",
                                        SubsystemsModel( request.OwnerType, request.OwnerHeader, owned ) );
        if ( !rendered.IsSuccess() )
        {
            std::cerr << "[DesertHeaderTool] " << rendered.GetError() << "\n";
            return 1;
        }
        const Common::BoolResultStr written = WriteIfChanged( request.Output, rendered.ExtractValue() );
        if ( !written.IsSuccess() )
        {
            std::cerr << "[DesertHeaderTool] " << written.GetError() << "\n";
            return 1;
        }
        std::cout << "[DesertHeaderTool] " << ( written.GetValue() ? "generated " : "up to date " )
                  << request.Output.string() << " (" << owned.size() << " " << request.Owner << " subsystems)\n";
    }

    if ( !reflect )
        return 0;
    if ( !moduleTable )
        return Reflect( *templateDir, *reflect, nullptr, {} );
    auto modules = Desert::HeaderTool::ModuleTable::Load( *moduleTable );
    if ( !modules.IsSuccess() )
    {
        std::cerr << "[DesertHeaderTool] " << modules.GetError() << "\n";
        return 1;
    }
    return Reflect( *templateDir, *reflect, &modules.GetValue(), fs::absolute( *moduleTable ).parent_path().parent_path() );
}

// The entry point, one line. Anything this tool throws is named on stderr with the tool's own name
// instead of reaching std::terminate, which would print the exception's TYPE and nothing else — see
// Tools/Shared/ToolMain.hpp.
int main( int argc, char** argv )
{
    return Desert::Tools::RunMain( "DesertHeaderTool", argc, argv, &RunTool );
}
