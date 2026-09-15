// DesertHeaderTool — a lightweight Unreal-Header-Tool-style code generator.
//
// Scans C++ headers for REFLECT() / PROPERTY(...) annotations and emits a single aggregated
// translation unit that registers every reflected type (with field offsets, types and editor
// metadata) into Desert::Reflection::ReflectionRegistry at static-init time.
//
// Usage:
//   DesertHeaderTool <source-root> <output-file> [scan-subdir]
//     <source-root>  include root (e.g. Desert/Desert/Source). #include paths in the generated
//                    file are computed relative to this.
//     <output-file>  path of the generated .cpp (e.g. .../Source/Engine/Generated/Reflection.gen.cpp)
//     [scan-subdir]  optional subdirectory of source-root to scan (default: whole root, e.g. "Engine").
//
// The annotation macros (REFLECT/PROPERTY) expand to nothing during normal compilation; only this
// tool reads them. See Engine/Reflection/ReflectionMacros.hpp.

#include <algorithm>
#include <ToolMain.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <charconv>
#include <string_view>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
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

    struct ReflectedType
    {
        std::string        fqn;          // fully-qualified C++ name, e.g. Desert::Assets::PBRMaterialData
        std::string        registryName; // short name used as the registry key, e.g. PBRMaterialData
        std::vector<Field> fields;
        std::string        headerInclude; // include path relative to source root
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
    std::string StripComments( const std::string& src )
    {
        std::string out;
        out.reserve( src.size() );
        for ( size_t i = 0; i < src.size(); )
        {
            char c = src[i];
            if ( c == '"' || c == '\'' )
            {
                char quote = c;
                out += c;
                ++i;
                while ( i < src.size() )
                {
                    out += src[i];
                    if ( src[i] == '\\' && i + 1 < src.size() )
                    {
                        out += src[i + 1];
                        i += 2;
                        continue;
                    }
                    if ( src[i] == quote )
                    {
                        ++i;
                        break;
                    }
                    ++i;
                }
                continue;
            }
            if ( c == '/' && i + 1 < src.size() && src[i + 1] == '/' )
            {
                while ( i < src.size() && src[i] != '\n' )
                    ++i;
                continue;
            }
            if ( c == '/' && i + 1 < src.size() && src[i + 1] == '*' )
            {
                i += 2;
                while ( i + 1 < src.size() && !( src[i] == '*' && src[i + 1] == '/' ) )
                    ++i;
                i += 2;
                continue;
            }
            out += c;
            ++i;
        }
        return out;
    }

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

    // Reads the attribute's string argument, CONCATENATING adjacent literals ("a" "b" -> "ab").
    // Reading only the first one silently truncated every annotation long enough for clang-format to wrap
    // it at the 115-column limit — the symptom was a tooltip that ended mid-sentence in the generated file
    // ("...NOT a photometric unit (lux/candela): ") with nothing in the source looking wrong.
    // Escapes are preserved verbatim, because the text is re-emitted straight back into a C++ literal.
    std::string ExtractStringLiteral( const std::string& s )
    {
        std::string out;
        size_t      i = 0;
        while ( true )
        {
            const auto a = s.find( '"', i );
            if ( a == std::string::npos )
                break;

            size_t b = a + 1;
            while ( b < s.size() && s[b] != '"' )
                b += ( s[b] == '\\' && b + 1 < s.size() ) ? 2 : 1;
            if ( b >= s.size() )
                break;

            out += s.substr( a + 1, b - a - 1 );

            // Only whitespace may separate adjacent literals; anything else ends this attribute.
            size_t j = b + 1;
            while ( j < s.size() && std::isspace( (unsigned char)s[j] ) )
                ++j;
            if ( j >= s.size() || s[j] != '"' )
                break;
            i = j;
        }
        return out;
    }

    // Splits the contents of PROPERTY( ... ) by top-level commas (ignoring commas inside (), <>, "").
    std::vector<std::string> SplitTopLevel( const std::string& s )
    {
        std::vector<std::string> out;
        std::string cur;
        int paren = 0, angle = 0;
        bool inStr = false;
        for ( size_t i = 0; i < s.size(); ++i )
        {
            char c = s[i];
            if ( inStr )
            {
                cur += c;
                if ( c == '"' ) inStr = false;
                continue;
            }
            switch ( c )
            {
                case '"': inStr = true; cur += c; break;
                case '(': ++paren; cur += c; break;
                case ')': --paren; cur += c; break;
                case '<': ++angle; cur += c; break;
                case '>': --angle; cur += c; break;
                case ',':
                    if ( paren == 0 && angle == 0 ) { out.push_back( cur ); cur.clear(); }
                    else cur += c;
                    break;
                default: cur += c; break;
            }
        }
        if ( !cur.empty() ) out.push_back( cur );
        return out;
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
    };

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
                    const std::vector<EnumDef>& enums )
    {
        const std::string raw = StripComments( ReadFile( file ) );
        if ( raw.find( "REFLECT()" ) == std::string::npos )
            return;

        std::string headerInclude =
             fs::relative( file, sourceRoot ).generic_string();

        std::vector<Scope> scopes;
        int depth = 0;

        Metadata pendingMeta;
        bool     hasPendingMeta = false;

        for ( size_t i = 0; i < raw.size(); )
        {
            char c = raw[i];

            if ( std::isspace( (unsigned char)c ) ) { ++i; continue; }

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
                        if ( f.meta.isAsset )
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
                    if ( sc.isStruct && sc.reflected && !sc.fields.empty() )
                    {
                        ReflectedType t;
                        t.registryName  = sc.name;
                        t.fqn           = JoinScopes( scopes ).empty()
                                             ? sc.name
                                             : JoinScopes( scopes ) + "::" + sc.name;
                        t.fields        = sc.fields;
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

    void EmitMetadata( std::ostream& o, const Metadata& m )
    {
        o << "PropertyMetadata{ ";
        if ( !m.displayName.empty() ) o << ".DisplayName = \"" << m.displayName << "\", ";
        if ( !m.category.empty() )    o << ".Category = \"" << m.category << "\", ";
        // Designated initializers must follow declaration order (DisplayName, Category, Tooltip, Header, ...).
        if ( !m.tooltip.empty() )     o << ".Tooltip = \"" << m.tooltip << "\", ";
        if ( !m.header.empty() )      o << ".Header = \"" << m.header << "\", ";
        if ( m.hasRange ) o << ".HasRange = true, .RangeMin = " << m.rangeMin << ", .RangeMax = " << m.rangeMax << ", ";
        if ( m.isColor )  o << ".IsColor = true, ";
        if ( m.isAsset )  o << ".IsAsset = true, .AssetType = \"" << m.assetType << "\", ";
        if ( m.thumbnail ) o << ".Thumbnail = true, ";
        if ( m.readOnly )  o << ".ReadOnly = true, ";
        if ( m.hidden )    o << ".Hidden = true, ";
        if ( m.isLength )
            o << ".IsLength = true, ";
        if ( !m.units.empty() )
            o << ".Units = \"" << m.units << "\", ";
        if ( m.advanced )
            o << ".Advanced = true, ";
        if ( m.summary )
            o << ".Summary = true, ";
        if ( m.temperature )
            o << ".Temperature = true, ";
        if ( m.preview )
            o << ".Preview = true, ";
        if ( !m.editCondition.empty() )
            o << ".EditCondition = \"" << m.editCondition << "\", ";
        o << "}";
    }

    // Emits `.IsContainer = true` + typed serialize/deserialize lambdas for a std::vector<...> field.
    // Uses decltype( T::field ) so the exact vector type never has to be re-spelled here.
    void EmitContainerLambdas( std::ostream& o, const Field& f )
    {
        std::string serExpr, deserStmt;
        const std::string& et = f.elemFieldType;
        if ( et == "Bool" )
        {
            serExpr   = "::rfl::Generic( e )";
            deserStmt = "{ auto x = e.to_bool(); v.push_back( x.has_value() ? x.value() : false ); }";
        }
        else if ( et == "Int" || et == "UInt" )
        {
            serExpr   = "::rfl::Generic( static_cast<int64_t>( e ) )";
            deserStmt = "{ auto x = e.to_int64(); v.push_back( static_cast<Elem>( x.has_value() ? x.value() : (int64_t)0 ) ); }";
        }
        else if ( et == "Float" || et == "Double" )
        {
            serExpr   = "::rfl::Generic( static_cast<double>( e ) )";
            deserStmt = "{ auto x = e.to_double(); v.push_back( static_cast<Elem>( x.has_value() ? x.value() : 0.0 ) ); }";
        }
        else if ( et == "String" )
        {
            serExpr   = "::rfl::Generic( e )";
            deserStmt = "{ auto x = e.to_string(); v.push_back( x.has_value() ? x.value() : std::string{} ); }";
        }
        else // AssetHandle
        {
            serExpr   = "::rfl::Generic( static_cast<int64_t>( static_cast<uint64_t>( e ) ) )";
            deserStmt = "{ auto x = e.to_int64(); v.push_back( Elem( static_cast<uint64_t>( x.has_value() ? x.value() : (int64_t)0 ) ) ); }";
        }

        o << ", .IsContainer = true"
          << ", .SerializeContainer = []( const void* p ) -> ::rfl::Generic { "
          << "const auto& v = *static_cast<const decltype( T::" << f.name << " )*>( p ); "
          << "::rfl::Generic::Array arr; for ( const auto& e : v ) arr.push_back( " << serExpr << " ); "
          << "return ::rfl::Generic( std::move( arr ) ); }"
          << ", .DeserializeContainer = []( void* p, const ::rfl::Generic& g ) { "
          << "auto& v = *static_cast<decltype( T::" << f.name << " )*>( p ); v.clear(); "
          << "auto a = g.to_array(); if ( !a.has_value() ) return; "
          << "using Elem = std::decay_t<decltype( v )>::value_type; "
          << "for ( const auto& e : a.value() ) " << deserStmt << " }";
    }

    void Generate( std::ostream& o, const std::vector<ReflectedType>& types )
    {
        o << "// AUTO-GENERATED by DesertHeaderTool. DO NOT EDIT.\n";
        o << "// Regenerated on every build from REFLECT()/PROPERTY() annotations.\n";
        // The emitter's line wrapping is not clang-format-stable across versions (the CI gate runs a
        // different clang-format than developers), so exclude the whole file from formatting. Keeps the
        // "changed lines" format gate green when an enum/field change regenerates these long initializers.
        o << "// clang-format off\n";
        // AND OUT OF THE ANALYSER FOR THE SAME REASON, one level up. The clang-tidy gate checks CHANGED
        // LINES, and every line of this file changes whenever anybody adds a PROPERTY — so a diagnostic
        // here is reported against a developer who did not write the line and cannot fix it in place: the
        // file's own first line says DO NOT EDIT, and the next build would overwrite the fix anyway. The
        // finding belongs to THIS emitter, which is analysed on its own like any other source.
        //
        // Measured 2026-09-14, adding two reflected components: the gate reported `bugprone-sizeof-
        // container` on every `sizeof( T::<a std::string field> )` and `clang-diagnostic-missing-field-
        // initializers` on every designated initializer that omits an optional member of PropertyMetadata
        // — hundreds of them, none about the change that triggered them. The sizeof one is worth reading
        // rather than only silencing: `FieldInfo::Size` for a string field is sizeof(std::string) and not
        // the text's length, which is what the serializer wants, but nothing states that.
        o << "// NOLINTBEGIN\n\n";
        o << "#include <Engine/Reflection/TypeRegistrar.hpp>\n";
        o << "#include <Engine/Reflection/ReflectionRegistry.hpp>\n";
        o << "#include <cstddef>\n";
        o << "#include <cstdint>\n";
        o << "#include <string>\n";
        o << "#include <type_traits>\n";
        o << "#include <vector>\n\n";

        // unique includes
        std::vector<std::string> includes;
        for ( const auto& t : types )
            if ( std::find( includes.begin(), includes.end(), t.headerInclude ) == includes.end() )
                includes.push_back( t.headerInclude );
        for ( const auto& inc : includes )
            o << "#include <" << inc << ">\n";

        o << "\nnamespace\n{\n";
        o << "    struct DesertReflectionAutoRegister\n    {\n";
        o << "        DesertReflectionAutoRegister()\n        {\n";
        o << "            using namespace ::Desert::Reflection;\n";

        for ( const auto& t : types )
        {
            o << "            {\n";
            o << "                using T = ::" << t.fqn << ";\n";
            o << "                TypeBuilder( \"" << t.registryName << "\", sizeof( T ) )\n";
            for ( const auto& f : t.fields )
            {
                o << "                    .Field( FieldInfo{ " << ".Name = \"" << f.name << "\", "
                  << ".Type = FieldType::" << f.fieldType << ", " << ".Offset = offsetof( T, " << f.name
                  << " ), "
                  // FieldFootprint<decltype(...)>() and not sizeof(...): see the note on the function.
                  << ".Size = ::Desert::Reflection::FieldFootprint<decltype( T::" << f.name << " )>(), "
                  << ".TypeName = \"" << f.cppType << "\", " << ".Meta = ";
                EmitMetadata( o, f.meta );
                if ( f.fieldType == "Enum" && !f.enumValues.empty() )
                {
                    o << ", .EnumValues = { ";
                    for ( const auto& [vname, vval] : f.enumValues )
                        o << "EnumValue{ \"" << vname << "\", " << vval << " }, ";
                    o << "}";
                }
                if ( f.isContainer )
                    EmitContainerLambdas( o, f );
                o << " } )\n";
            }
            // Capture a default-constructed instance so the editor can offer reset-to-default per field.
            o << "                    .WithDefault<T>()\n";
            o << "                    .Register();\n";
            o << "            }\n";
        }

        o << "            ReflectionRegistry::Get().ResolveStructLinks();\n";
        o << "        }\n";
        o << "    } g_DesertReflectionAutoRegister;\n";
        o << "}\n\n";

        // Force-link anchor: this TU lives in a static lib, so its static initializer (the registrar
        // above) is only pulled into the final image if a symbol from it is referenced. Engine startup
        // calls this no-op to guarantee the registrations actually run.
        o << "namespace Desert::Reflection\n{\n";
        o << "    void ForceLinkGeneratedReflection() {}\n";
        o << "}\n";
        o << "// NOLINTEND\n";
    }
} // namespace

static int RunTool( int argc, char** argv )
{
    if ( argc < 3 )
    {
        std::cerr << "Usage: DesertHeaderTool <source-root> <output-file> [scan-subdir]\n";
        return 1;
    }

    fs::path sourceRoot = argv[1];
    fs::path outputFile = argv[2];
    fs::path scanRoot   = ( argc >= 4 ) ? ( sourceRoot / argv[3] ) : sourceRoot;

    if ( !fs::exists( scanRoot ) )
    {
        std::cerr << "[DesertHeaderTool] scan root does not exist: " << scanRoot << "\n";
        return 1;
    }

    // Gather headers once.
    std::vector<fs::path> headers;
    for ( auto& entry : fs::recursive_directory_iterator( scanRoot ) )
    {
        if ( !entry.is_regular_file() ) continue;
        const auto ext = entry.path().extension().string();
        if ( ext != ".hpp" && ext != ".h" ) continue;
        // never parse our own generated output
        if ( entry.path().filename().string().find( ".gen." ) != std::string::npos ) continue;
        headers.push_back( entry.path() );
    }

    // Pass 1: collect every enum definition (so a reflected field can reference an enum from any header,
    // regardless of declaration order or file).
    std::vector<EnumDef> enums;
    for ( const auto& h : headers )
        CollectEnums( StripComments( ReadFile( h ) ), enums );

    // Pass 2: parse reflected types, resolving enum field types against the collected enums.
    std::vector<ReflectedType> types;
    const size_t               scanned = headers.size();
    for ( const auto& h : headers )
        ParseFile( h, sourceRoot, types, enums );

    std::ostringstream gen;
    Generate( gen, types );

    // Only rewrite when content changed — avoids needless recompiles of the generated TU.
    std::string newContent = gen.str();
    if ( fs::exists( outputFile ) && ReadFile( outputFile ) == newContent )
    {
        std::cout << "[DesertHeaderTool] up to date (" << types.size() << " reflected types, "
                  << scanned << " headers scanned)\n";
        return 0;
    }

    fs::create_directories( outputFile.parent_path() );
    std::ofstream out( outputFile, std::ios::binary );
    out << newContent;
    out.close();

    std::cout << "[DesertHeaderTool] generated " << outputFile.string() << " ("
              << types.size() << " reflected types, " << scanned << " headers scanned)\n";
    return 0;
}

// The entry point, one line. Anything this tool throws is named on stderr with the tool's own name
// instead of reaching std::terminate, which would print the exception's TYPE and nothing else — see
// Tools/Shared/ToolMain.hpp.
int main( int argc, char** argv )
{
    return Desert::Tools::RunMain( "DesertHeaderTool", argc, argv, &RunTool );
}
