#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>

#include <Common/Content/ShaderAssetHeader.hpp>
#include <Engine/Assets/Serialization/WorldParameters.hpp>
#include <Engine/Core/ShaderCompiler/ShadingModels/ShaderRootShadingModels.hpp>
#include <Engine/Core/ShaderCompiler/ShaderGraphBindings.hpp>
#include <Engine/Core/ShaderCompiler/SharedSamplerBinding.hpp>
#include <Engine/Core/TextTemplate.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <functional>
#include <optional>
#include <regex>
#include <span>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>

namespace Desert::Core::Preprocess
{
    using namespace Desert::Core::Formats;

    namespace
    {
        // The legal default-texture names as one comma-separated list, for the refusal of an unknown one.
        // Built from kAllDefaultTextureKinds, so a name added to the enum joins the message by itself.
        std::string DefaultTextureKindList()
        {
            static const TextTemplate kList(
                 "DefaultTextureKindList",
                 R"({% for kind in kinds %}{% if not loop.first %}, {% endif %}"{{ kind }}"{% endfor %})" );
            Common::Json::Value::Array kinds;
            for ( const DefaultTextureKind kind : kAllDefaultTextureKinds )
                kinds.emplace_back( std::string( DefaultTextureKindName( kind ) ) );
            return kList.RenderOrLog(
                 Common::Json::Value( Common::Json::ObjectBuilder()
                                           .Set( "kinds", Common::Json::Value( std::move( kinds ) ) )
                                           .Build() ) );
        }

        // ─── Cursor: char-by-char scanner with line tracking ────────────────────────

        struct Cursor
        {
            const std::string& Src;
            size_t             Pos  = 0;
            uint32_t           Line = 1;

            bool AtEnd() const
            {
                return Pos >= Src.size();
            }
            char Peek() const
            {
                return AtEnd() ? '\0' : Src[Pos];
            }
            char Advance()
            {
                const char c = Src[Pos++];
                if ( c == '\n' )
                    ++Line;
                return c;
            }
        };

        struct ParseError
        {
            uint32_t    Line;
            std::string Message;
        };

        // Skips whitespace plus // and /* */ comments.
        void SkipTrivia( Cursor& c )
        {
            while ( !c.AtEnd() )
            {
                const char ch = c.Peek();
                if ( std::isspace( static_cast<unsigned char>( ch ) ) )
                {
                    c.Advance();
                }
                else if ( ch == '/' && c.Pos + 1 < c.Src.size() && c.Src[c.Pos + 1] == '/' )
                {
                    while ( !c.AtEnd() && c.Peek() != '\n' )
                        c.Advance();
                }
                else if ( ch == '/' && c.Pos + 1 < c.Src.size() && c.Src[c.Pos + 1] == '*' )
                {
                    c.Advance();
                    c.Advance();
                    while ( !c.AtEnd() &&
                            !( c.Peek() == '*' && c.Pos + 1 < c.Src.size() && c.Src[c.Pos + 1] == '/' ) )
                        c.Advance();
                    if ( !c.AtEnd() )
                    {
                        c.Advance();
                        c.Advance();
                    }
                }
                else
                {
                    break;
                }
            }
        }

        bool IsIdentChar( char ch )
        {
            return std::isalnum( static_cast<unsigned char>( ch ) ) || ch == '_';
        }

        std::string ReadIdent( Cursor& c )
        {
            SkipTrivia( c );
            std::string out;
            while ( !c.AtEnd() && IsIdentChar( c.Peek() ) )
                out.push_back( c.Advance() );
            return out;
        }

        bool Expect( Cursor& c, char ch, ParseError& err, const char* context )
        {
            SkipTrivia( c );
            if ( c.Peek() != ch )
            {
                err = { c.Line, std::string( "expected '" ) + ch + "' " + context };
                return false;
            }
            c.Advance();
            return true;
        }

        bool ReadQuoted( Cursor& c, std::string& out, ParseError& err )
        {
            SkipTrivia( c );
            if ( c.Peek() != '"' )
            {
                err = { c.Line, "expected a quoted string" };
                return false;
            }
            c.Advance();
            out.clear();
            while ( !c.AtEnd() && c.Peek() != '"' && c.Peek() != '\n' )
                out.push_back( c.Advance() );
            if ( c.Peek() != '"' )
            {
                err = { c.Line, "unterminated string" };
                return false;
            }
            c.Advance();
            return true;
        }

        bool ReadNumber( Cursor& c, float& out, ParseError& err )
        {
            SkipTrivia( c );
            std::string tok;
            while ( !c.AtEnd() && ( std::isdigit( static_cast<unsigned char>( c.Peek() ) ) || c.Peek() == '.' ||
                                    c.Peek() == '-' || c.Peek() == '+' ) )
                tok.push_back( c.Advance() );
            try
            {
                out = std::stof( tok );
            }
            catch ( ... )
            {
                err = { c.Line, "expected a number, got '" + tok + "'" };
                return false;
            }
            return true;
        }

        // Reads a brace-balanced block verbatim (comments respected so a '}' inside them
        // doesn't end the block). Cursor must be at '{'. Returns the raw content and the
        // line the content starts on.
        bool ReadBlock( Cursor& c, std::string& outContent, uint32_t& outStartLine, ParseError& err )
        {
            SkipTrivia( c );
            if ( c.Peek() != '{' )
            {
                err = { c.Line, "expected '{'" };
                return false;
            }
            c.Advance();
            outStartLine = c.Line;

            const size_t begin = c.Pos;
            int          depth = 1;
            while ( !c.AtEnd() )
            {
                const char ch = c.Peek();
                if ( ch == '/' && c.Pos + 1 < c.Src.size() &&
                     ( c.Src[c.Pos + 1] == '/' || c.Src[c.Pos + 1] == '*' ) )
                {
                    SkipTrivia( c ); // consumes the whole comment; keeps line counting exact
                    continue;
                }
                if ( ch == '{' )
                    ++depth;
                else if ( ch == '}' )
                {
                    if ( --depth == 0 )
                    {
                        outContent = c.Src.substr( begin, c.Pos - begin );
                        c.Advance();
                        return true;
                    }
                }
                c.Advance();
            }
            err = { c.Line, "unterminated block (missing '}')" };
            return false;
        }

        // ─── Keyword tables ─────────────────────────────────────────────────────────

        std::string Lower( std::string s )
        {
            for ( auto& ch : s )
                ch = static_cast<char>( std::tolower( static_cast<unsigned char>( ch ) ) );
            return s;
        }

        ShaderStage StageFromKeyword( const std::string& kw )
        {
            const std::string s = Lower( kw );
            if ( s == "vertex" )
                return ShaderStage::Vertex;
            if ( s == "fragment" || s == "pixel" )
                return ShaderStage::Fragment;
            if ( s == "compute" )
                return ShaderStage::Compute;
            if ( s == "tesscontrol" )
                return ShaderStage::TessControl;
            if ( s == "tesseval" || s == "tessevaluation" )
                return ShaderStage::TessEvaluation;
            if ( s == "raygen" )
                return ShaderStage::RayGen;
            if ( s == "miss" )
                return ShaderStage::Miss;
            if ( s == "closesthit" )
                return ShaderStage::ClosestHit;
            if ( s == "anyhit" )
                return ShaderStage::AnyHit;
            return ShaderStage::None;
        }

        bool ParamTypeFromKeyword( const std::string& kw, ShaderParam& param )
        {
            const std::string s = Lower( kw );
            using VT            = ShaderValueType;
            using W             = ShaderParamWidget;

            if ( s == "float" )
                param.Type = VT::Float;
            else if ( s == "vec2" )
                param.Type = VT::Float2;
            else if ( s == "vec3" )
                param.Type = VT::Float3;
            else if ( s == "vec4" )
                param.Type = VT::Float4;
            else if ( s == "int" )
                param.Type = VT::Int;
            else if ( s == "bool" )
                param.Type = VT::Bool;
            else if ( s == "color" || s == "color4" )
            {
                param.Type   = VT::Float4;
                param.Widget = W::Color;
            }
            else if ( s == "color3" )
            {
                param.Type   = VT::Float3;
                param.Widget = W::Color;
            }
            else if ( s == "texture2d" )
            {
                param.IsTexture = true;
                param.Type      = VT::Unknown;
            }
            else if ( s == "texture2darray" )
            {
                param.IsTexture      = true;
                param.IsArrayTexture = true;
                param.Type           = VT::Unknown;
            }
            else if ( s == "landscapelayerinfo" )
            {
                param.AssetKind = "LandscapeLayerInfoAsset";
                param.Type      = VT::Unknown;
            }
            else if ( s == "texturecube" )
            {
                param.IsTexture     = true;
                param.IsCubeTexture = true;
                param.Type          = VT::Unknown;
            }
            // Non-texture asset references — CPU-side inputs a renderer resolves through its own service,
            // never a GLSL declaration. The keyword is the ASSET CLASS so the schema names what the slot
            // accepts; a new asset-reference kind is one line here plus a widget branch in the editor.
            else if ( s == "cloudtype" )
            {
                param.AssetKind = "CloudTypeAsset";
                param.Type      = VT::Unknown;
            }
            else if ( s == "cloudlayout" )
            {
                param.AssetKind = "CloudLayoutAsset";
                param.Type      = VT::Unknown;
            }
            // A reference to another SHADER — today only ever a Volume-domain `Medium` fragment, which is
            // the authored cloud medium a material substitutes into the four programs that sample the
            // field. It is an asset reference like the two above and travels the same name->handle map;
            // what makes it worth its own keyword rather than a texture slot is that a `.shader` is what
            // the graph editor already produces and already gives an identity to.
            else if ( s == "shaderprogram" )
            {
                param.AssetKind = "ShaderAsset";
                param.Type      = VT::Unknown;
            }
            else
                return false;
            return true;
        }

        // ─── Section parsers ────────────────────────────────────────────────────────

        // 2D-vs-cube for the auto-generated samplers comes from ShaderParam::IsCubeTexture itself; the
        // parallel "Extras" array this struct used to carry is gone with it.
        struct PropertiesInfo
        {
            std::optional<uint32_t> UBBinding;      // Binding(n)
            std::optional<uint32_t> TextureBinding; // TextureBinding(n)
        };

        bool ParsePropertyAttributes( Cursor& c, ShaderParam& param, ParseError& err )
        {
            // ( "Display Name" [, Range(a,b)] [, Category("...")] [, Tooltip("...")] [, Timing(Immediate)]
            //   [, EngineSet] )
            if ( !Expect( c, '(', err, "after property name" ) )
                return false;

            SkipTrivia( c );
            if ( c.Peek() == '"' )
            {
                if ( !ReadQuoted( c, param.DisplayName, err ) )
                    return false;
            }

            SkipTrivia( c );
            while ( c.Peek() == ',' )
            {
                c.Advance();
                const std::string attr = Lower( ReadIdent( c ) );
                if ( attr == "range" )
                {
                    float a = 0, b = 0;
                    if ( !Expect( c, '(', err, "after Range" ) || !ReadNumber( c, a, err ) ||
                         !Expect( c, ',', err, "in Range" ) || !ReadNumber( c, b, err ) ||
                         !Expect( c, ')', err, "closing Range" ) )
                        return false;
                    param.Min = a;
                    param.Max = b;
                    if ( param.Widget == ShaderParamWidget::Auto )
                        param.Widget = ShaderParamWidget::Slider;
                }
                else if ( attr == "category" )
                {
                    if ( !Expect( c, '(', err, "after Category" ) || !ReadQuoted( c, param.Category, err ) ||
                         !Expect( c, ')', err, "closing Category" ) )
                        return false;
                }
                // A texture the ENGINE writes per draw (ShaderParam::EngineSet). Refused on anything but a
                // texture: an engine-written scalar has a uniform buffer of its own, and an attribute that
                // meant nothing on a float would be a dead setting the author believes in.
                else if ( attr == "engineset" )
                {
                    if ( !param.IsTexture )
                    {
                        err = {
                             c.Line,
                             std::format( "EngineSet is only valid on a Texture2D or TextureCube property ('{}')",
                                          param.Name ) };
                        return false;
                    }
                    param.EngineSet = true;
                }
                // `Element(Group, k)` (ShaderParam::ElementOf): element k of a group — a Texture2D slice of
                // the Texture2DArray named Group, or an asset reference grouped with it. Checked against the
                // whole block once it is read (the group may be declared after its elements).
                else if ( attr == "element" )
                {
                    if ( !( ( param.IsTexture && !param.IsCubeTexture && !param.IsArrayTexture ) ||
                            param.IsAssetRef() ) )
                    {
                        err = { c.Line, std::format( "Element(...) on '{}', which is neither a Texture2D nor an "
                                                     "asset reference",
                                                     param.Name ) };
                        return false;
                    }
                    float k = -1.0f;
                    if ( !Expect( c, '(', err, "after Element" ) )
                        return false;
                    SkipTrivia( c );
                    param.ElementOf = ReadIdent( c );
                    if ( param.ElementOf.empty() || !Expect( c, ',', err, "in Element" ) ||
                         !ReadNumber( c, k, err ) || !Expect( c, ')', err, "closing Element" ) )
                    {
                        if ( err.Message.empty() )
                            err = { c.Line, std::format( "Element(...) on '{}' names no group", param.Name ) };
                        return false;
                    }
                    if ( k < 0.0f || k != static_cast<float>( static_cast<int32_t>( k ) ) )
                    {
                        err = { c.Line,
                                std::format( "Element({}, {}) on '{}': the index must be a whole number >= 0",
                                             param.ElementOf, k, param.Name ) };
                        return false;
                    }
                    param.ElementIndex = static_cast<int32_t>( k );
                }
                else if ( attr == "tooltip" )
                {
                    if ( !Expect( c, '(', err, "after Tooltip" ) || !ReadQuoted( c, param.Tooltip, err ) ||
                         !Expect( c, ')', err, "closing Tooltip" ) )
                        return false;
                }
                // WHEN AN EDIT HERE REACHES THE PICTURE — `Timing(Immediate)` or `Timing(Rebake)`. See
                // ShaderParamTiming. An UNQUOTED enumerator and not a string, because there are exactly two
                // of them and a misspelling must be a parse error the shader author sees at load rather
                // than a free-text field that silently means nothing.
                else if ( attr == "timing" )
                {
                    if ( !Expect( c, '(', err, "after Timing" ) )
                        return false;
                    SkipTrivia( c );
                    const std::string timing = Lower( ReadIdent( c ) );
                    if ( timing == "immediate" )
                        param.Timing = ShaderParamTiming::Immediate;
                    else if ( timing == "rebake" )
                        param.Timing = ShaderParamTiming::Rebake;
                    else
                    {
                        err = { c.Line, "unknown Timing '" + timing + "' (expected Immediate or Rebake)" };
                        return false;
                    }
                    if ( !Expect( c, ')', err, "closing Timing" ) )
                        return false;
                }
                // THE TEMPLATE'S SAMPLER FOR A TEXTURE SLOT — `Sampler(WrapU, WrapV, Filter)`, unquoted
                // enumerators (Repeat|Clamp|Mirror, Linear|Nearest) for the same reason as Timing: a misspelling
                // is a parse error at load, not a free-text field that silently means Repeat.
                else if ( attr == "sampler" )
                {
                    if ( !param.IsTexture || param.IsCubeTexture )
                    {
                        err = { c.Line, std::format( "Sampler(...) on '{}', which is not a Texture2D property",
                                                     param.Name ) };
                        return false;
                    }
                    if ( !Expect( c, '(', err, "after Sampler" ) )
                        return false;
                    std::string words[3];
                    for ( int i = 0; i < 3; ++i )
                    {
                        if ( i > 0 && !Expect( c, ',', err, "in Sampler" ) )
                            return false;
                        SkipTrivia( c );
                        words[i] = Lower( ReadIdent( c ) );
                    }
                    const auto wrapU  = ParseSamplerWrap( words[0] );
                    const auto wrapV  = ParseSamplerWrap( words[1] );
                    const auto filter = ParseSamplerFilter( words[2] );
                    if ( !wrapU || !wrapV || !filter )
                    {
                        err = { c.Line, std::format( "Sampler({}, {}, {}) on '{}': expected (Repeat|Clamp|Mirror, "
                                                     "Repeat|Clamp|Mirror, Linear|Nearest)",
                                                     words[0], words[1], words[2], param.Name ) };
                        return false;
                    }
                    param.Sampler = { *wrapU, *wrapV, *filter };
                    if ( !Expect( c, ')', err, "closing Sampler" ) )
                        return false;
                }
                // WHAT THE SLOT'S TEXTURE IS FOR — `Intent(NormalMap)`, an unquoted name from
                // `TextureIntent.hpp`'s one table (case-sensitive, as the `.detex` spells it). Only on a
                // Texture2D.
                else if ( attr == "intent" )
                {
                    if ( !param.IsTexture || param.IsCubeTexture )
                    {
                        err = { c.Line, std::format( "Intent(...) on '{}', which is not a Texture2D property",
                                                     param.Name ) };
                        return false;
                    }
                    if ( !Expect( c, '(', err, "after Intent" ) )
                        return false;
                    SkipTrivia( c );
                    const std::string word   = ReadIdent( c );
                    const auto        intent = ::Desert::Core::Formats::TextureIntentFromName( word );
                    if ( intent == ::Desert::Core::Formats::TextureIntent::Count ||
                         intent == ::Desert::Core::Formats::TextureIntent::Unspecified )
                    {
                        err = { c.Line,
                                std::format(
                                     "Intent({}) on '{}': expected Colour, NormalMap, Mask, Data or PackedMasks",
                                     word, param.Name ) };
                        return false;
                    }
                    param.SlotIntent = intent;
                    if ( !Expect( c, ')', err, "closing Intent" ) )
                        return false;
                }
                else
                {
                    err = { c.Line, "unknown property attribute '" + attr +
                                         "' (expected Range, Category, Tooltip, Timing, EngineSet, Sampler or "
                                         "Intent)" };
                    return false;
                }
                SkipTrivia( c );
            }

            return Expect( c, ')', err, "closing the property attribute list" );
        }

        bool ParsePropertyDefault( Cursor& c, ShaderParam& param, ParseError& err )
        {
            SkipTrivia( c );
            if ( c.Peek() != '=' )
                return true; // defaults are optional
            c.Advance();
            SkipTrivia( c );

            if ( c.Peek() == '"' ) // texture default: = "white"
            {
                if ( !param.IsTexture )
                {
                    err = { c.Line, "string default is only valid for texture properties" };
                    return false;
                }

                const uint32_t line = c.Line;
                std::string    name;
                if ( !ReadQuoted( c, name, err ) )
                    return false;

                // REFUSED HERE OR NEVER. An unknown name used to be stored verbatim in a std::string that
                // nothing read, so `= "wihte"` parsed, compiled, loaded and drew — and the only evidence
                // was a surface somebody eventually noticed was the wrong colour. Now the set is closed
                // (Core/Formats/DefaultTexture.hpp) and this is the one line that can still say WHICH
                // file, WHICH property and WHICH line, so it says all three.
                const auto kind = ParseDefaultTextureKind( name );
                if ( !kind )
                {
                    err = { line,
                            std::format( "unknown default texture '{}' for property '{}' (expected one of {})",
                                         name, param.Name, DefaultTextureKindList() ) };
                    return false;
                }
                param.DefaultTexture = *kind;
                return true;
            }

            if ( c.Peek() == '(' ) // vector default: = (r, g, b, a)
            {
                c.Advance();
                for ( glm::length_t i = 0; i < 4; ++i )
                {
                    float v = 0;
                    if ( !ReadNumber( c, v, err ) )
                        return false;
                    param.Default[i] = v;
                    SkipTrivia( c );
                    if ( c.Peek() == ',' )
                    {
                        c.Advance();
                        continue;
                    }
                    break;
                }
                return Expect( c, ')', err, "closing the default value" );
            }

            // scalar default: = 4
            float v = 0;
            if ( !ReadNumber( c, v, err ) )
                return false;
            param.Default[0] = v;
            return true;
        }

        // One `Import` row with the line it came from; checked against Properties once the whole file is read
        // (the block may come before Properties).
        struct PendingImport
        {
            uint32_t                          Line = 0;
            Common::Content::ShaderImportLine Row;
        };

        // `Import { <row per line> }` — the template's import contract. The row grammar has ONE home,
        // Common::Content::ParseShaderImportLine, which ReadShaderManifest also reads the block with.
        bool ParseImportBlock( Cursor& c, std::vector<PendingImport>& out, ParseError& err )
        {
            if ( !Expect( c, '{', err, "opening the Import block" ) )
                return false;
            while ( true )
            {
                if ( c.AtEnd() )
                {
                    err = { c.Line, "unterminated Import block" };
                    return false;
                }
                const uint32_t line = c.Line;
                std::string    text;
                while ( !c.AtEnd() && c.Peek() != '\n' )
                    text.push_back( c.Advance() );
                if ( !c.AtEnd() )
                    c.Advance();
                if ( const auto comment = text.find( "//" ); comment != std::string::npos )
                    text.erase( comment );
                const auto first = text.find_first_not_of( " \t\r" );
                if ( first == std::string::npos )
                    continue;
                text = text.substr( first, text.find_last_not_of( " \t\r" ) - first + 1 );
                if ( text == "}" )
                    return true;
                auto row = Common::Content::ParseShaderImportLine( text );
                if ( !row )
                {
                    err = { line, row.GetError() };
                    return false;
                }
                out.push_back( { line, row.GetValue() } );
            }
        }

        // Every mapped Property must be one this shader declares, and only a texture takes source channels.
        bool CheckImportRows( const std::vector<PendingImport>& rows, const ShaderProgramMeta& meta,
                              ParseError& err )
        {
            for ( const auto& pending : rows )
            {
                if ( pending.Row.IsRequires )
                    continue;
                const auto& row   = pending.Row.Row;
                const auto  param = std::find_if( meta.Params.begin(), meta.Params.end(),
                                                  [&]( const ShaderParam& p ) { return p.Name == row.Property; } );
                if ( param == meta.Params.end() )
                {
                    err = {
                         pending.Line,
                         std::format( "Import maps \"{}\" to '{}', which this shader's Properties do not declare",
                                      row.SourceKey, row.Property ) };
                    return false;
                }
                if ( !row.Channels.empty() && !param->IsTexture )
                {
                    err = { pending.Line, std::format( "Import maps \"{}\" to '{}.{}', but '{}' is not a texture: "
                                                       "only a texture takes source channels",
                                                       row.SourceKey, row.Property, row.Channels, row.Property ) };
                    return false;
                }
            }
            return true;
        }

        // Every Element texture names a Texture2DArray of the block as its group, and no two texture elements
        // of one group share an index: a slice two slots claim, or an array nobody declared, would otherwise be
        // decided by whichever loop ran last.
        bool CheckElementGroups( const ShaderProgramMeta& meta, uint32_t line, ParseError& err )
        {
            for ( const auto& p : meta.Params )
            {
                if ( p.ElementIndex < 0 || !p.IsTexture )
                    continue;
                const auto group =
                     std::find_if( meta.Params.begin(), meta.Params.end(), [&]( const ShaderParam& q )
                                   { return q.Name == p.ElementOf && q.IsArrayTexture; } );
                if ( group == meta.Params.end() )
                {
                    err = { line,
                            std::format( "Element({}, {}) on '{}': the block declares no Texture2DArray '{}'",
                                         p.ElementOf, p.ElementIndex, p.Name, p.ElementOf ) };
                    return false;
                }
                for ( const auto& q : meta.Params )
                    if ( &q != &p && q.IsTexture && q.ElementOf == p.ElementOf &&
                         q.ElementIndex == p.ElementIndex )
                    {
                        err = { line, std::format( "'{}' and '{}' are both Element({}, {})", p.Name, q.Name,
                                                   p.ElementOf, p.ElementIndex ) };
                        return false;
                    }
            }
            return true;
        }

        bool ParsePropertiesBlock( Cursor& c, ShaderProgramMeta& meta, PropertiesInfo& info, ParseError& err )
        {
            // Optional Binding(n) / TextureBinding(n) before '{'
            SkipTrivia( c );
            while ( c.Peek() != '{' && !c.AtEnd() )
            {
                const std::string opt = Lower( ReadIdent( c ) );
                float             v   = 0;
                if ( opt == "binding" )
                {
                    if ( !Expect( c, '(', err, "after Binding" ) || !ReadNumber( c, v, err ) ||
                         !Expect( c, ')', err, "closing Binding" ) )
                        return false;
                    info.UBBinding = static_cast<uint32_t>( v );
                }
                else if ( opt == "texturebinding" )
                {
                    if ( !Expect( c, '(', err, "after TextureBinding" ) || !ReadNumber( c, v, err ) ||
                         !Expect( c, ')', err, "closing TextureBinding" ) )
                        return false;
                    info.TextureBinding = static_cast<uint32_t>( v );
                }
                else
                {
                    err = { c.Line,
                            "unknown Properties option '" + opt + "' (expected Binding or TextureBinding)" };
                    return false;
                }
                SkipTrivia( c );
            }

            if ( !Expect( c, '{', err, "opening the Properties block" ) )
                return false;

            while ( true )
            {
                SkipTrivia( c );
                if ( c.Peek() == '}' )
                {
                    c.Advance();
                    return CheckElementGroups( meta, c.Line, err );
                }
                if ( c.AtEnd() )
                {
                    err = { c.Line, "unterminated Properties block" };
                    return false;
                }

                const uint32_t    entryLine = c.Line;
                const std::string typeKw    = ReadIdent( c );
                ShaderParam       param;
                if ( !ParamTypeFromKeyword( typeKw, param ) )
                {
                    err = { entryLine, "unknown property type '" + typeKw + "'" };
                    return false;
                }

                param.Name = ReadIdent( c );
                if ( param.Name.empty() )
                {
                    err = { c.Line, "property is missing a name" };
                    return false;
                }

                SkipTrivia( c );
                if ( c.Peek() == '(' )
                {
                    if ( !ParsePropertyAttributes( c, param, err ) )
                        return false;
                }
                if ( param.DisplayName.empty() )
                    param.DisplayName = param.Name;

                if ( !ParsePropertyDefault( c, param, err ) )
                    return false;

                // An asset reference inside a GPU-bound Properties block is refused HERE, at parse, with
                // the parameter named. Skipping it silently instead would shift every row field after it
                // by one slot — a divergence between the C++ row upload and the generated struct that no
                // validation layer reports and no test that reads either side alone can see.
                // The row (Binding()) skips asset references in the ONE layout builder the GLSL and the C++
                // upload are both written from (Core::Formats::BuildMaterialLayout), so one may sit in a row
                // block; TextureBinding() would hand it a sampler binding of its own, and so would an
                // Element texture, which is a CPU-side input copied into its array.
                if ( ( param.IsAssetRef() || param.ElementIndex >= 0 ) && info.TextureBinding )
                {
                    err = { entryLine, "property '" + param.Name +
                                            "' is a CPU-side input (an asset reference or an Element texture) "
                                            "and cannot appear in a Properties block that declares "
                                            "TextureBinding()" };
                    return false;
                }

                meta.Params.push_back( std::move( param ) );
            }
        }

        bool ParseStateBlock( Cursor& c, ShaderRenderState& state, ParseError& err )
        {
            if ( !Expect( c, '{', err, "opening the State block" ) )
                return false;

            while ( true )
            {
                SkipTrivia( c );
                if ( c.Peek() == '}' )
                {
                    c.Advance();
                    return true;
                }
                if ( c.AtEnd() )
                {
                    err = { c.Line, "unterminated State block" };
                    return false;
                }

                const uint32_t    line   = c.Line;
                const std::string rawCmd = ReadIdent( c );
                const std::string cmd    = Lower( rawCmd );

                if ( cmd == "cull" )
                {
                    const std::string v = Lower( ReadIdent( c ) );
                    if ( v == "none" )
                        state.Cull = StateCull::None;
                    else if ( v == "front" )
                        state.Cull = StateCull::Front;
                    else if ( v == "back" )
                        state.Cull = StateCull::Back;
                    else if ( v == "frontandback" )
                        state.Cull = StateCull::FrontAndBack;
                    else
                    {
                        err = { line, "unknown Cull mode '" + v + "'" };
                        return false;
                    }
                }
                else if ( cmd == "ztest" )
                {
                    const std::string v = Lower( ReadIdent( c ) );
                    using C             = StateCompare;
                    if ( v == "off" )
                        state.DepthTest = false;
                    else
                    {
                        std::optional<C> cmp;
                        if ( v == "never" )
                            cmp = C::Never;
                        else if ( v == "less" )
                            cmp = C::Less;
                        else if ( v == "equal" )
                            cmp = C::Equal;
                        else if ( v == "lequal" || v == "lessorequal" )
                            cmp = C::LessOrEqual;
                        else if ( v == "greater" )
                            cmp = C::Greater;
                        else if ( v == "notequal" )
                            cmp = C::NotEqual;
                        else if ( v == "gequal" || v == "greaterorequal" )
                            cmp = C::GreaterOrEqual;
                        else if ( v == "always" )
                            cmp = C::Always;
                        if ( !cmp )
                        {
                            err = { line, "unknown ZTest mode '" + v + "'" };
                            return false;
                        }
                        state.DepthCompare = cmp;
                        state.DepthTest    = true;
                    }
                }
                else if ( cmd == "zwrite" )
                {
                    const std::string v = Lower( ReadIdent( c ) );
                    state.DepthWrite    = ( v == "on" || v == "true" );
                }
                else if ( cmd == "blend" )
                {
                    // `Blend Off | On | Alpha` OR custom factors `Blend <src> <dst>` (e.g.
                    // `Blend SrcAlpha OneMinusSrcAlpha`, `Blend One One` for additive).
                    const auto factor = []( const std::string& v ) -> std::optional<StateBlendFactor>
                    {
                        if ( v == "zero" )
                            return StateBlendFactor::Zero;
                        if ( v == "one" )
                            return StateBlendFactor::One;
                        if ( v == "srccolor" )
                            return StateBlendFactor::SrcColor;
                        if ( v == "oneminussrccolor" )
                            return StateBlendFactor::OneMinusSrcColor;
                        if ( v == "dstcolor" )
                            return StateBlendFactor::DstColor;
                        if ( v == "oneminusdstcolor" )
                            return StateBlendFactor::OneMinusDstColor;
                        if ( v == "srcalpha" )
                            return StateBlendFactor::SrcAlpha;
                        if ( v == "oneminussrcalpha" )
                            return StateBlendFactor::OneMinusSrcAlpha;
                        if ( v == "dstalpha" )
                            return StateBlendFactor::DstAlpha;
                        if ( v == "oneminusdstalpha" )
                            return StateBlendFactor::OneMinusDstAlpha;
                        return std::nullopt;
                    };
                    const std::string v = Lower( ReadIdent( c ) );
                    if ( v == "off" || v == "false" )
                        state.Blend = false;
                    else if ( v == "on" || v == "alpha" || v == "true" )
                        state.Blend = true;
                    else if ( auto src = factor( v ) )
                    {
                        const std::string d   = Lower( ReadIdent( c ) );
                        auto              dst = factor( d );
                        if ( !dst )
                        {
                            err = { line, "unknown Blend dst factor '" + d + "'" };
                            return false;
                        }
                        state.Blend    = true;
                        state.BlendSrc = *src;
                        state.BlendDst = *dst;
                    }
                    else
                    {
                        err = { line, "unknown Blend value '" + v + "'" };
                        return false;
                    }
                }
                else if ( cmd == "stencil" )
                {
                    // `Stencil <compare> <ref> [<fail> <pass> <depthFail>]`. Ops default Keep/Replace/Keep
                    // (write the ref where the test passes — the outline-mask idiom).
                    const auto compare = []( const std::string& v ) -> std::optional<StateCompare>
                    {
                        if ( v == "never" )
                            return StateCompare::Never;
                        if ( v == "less" )
                            return StateCompare::Less;
                        if ( v == "equal" )
                            return StateCompare::Equal;
                        if ( v == "lequal" || v == "lessorequal" )
                            return StateCompare::LessOrEqual;
                        if ( v == "greater" )
                            return StateCompare::Greater;
                        if ( v == "notequal" )
                            return StateCompare::NotEqual;
                        if ( v == "gequal" || v == "greaterorequal" )
                            return StateCompare::GreaterOrEqual;
                        if ( v == "always" )
                            return StateCompare::Always;
                        return std::nullopt;
                    };
                    const auto stencilOp = []( const std::string& v ) -> std::optional<StateStencilOp>
                    {
                        if ( v == "keep" )
                            return StateStencilOp::Keep;
                        if ( v == "zero" )
                            return StateStencilOp::Zero;
                        if ( v == "replace" )
                            return StateStencilOp::Replace;
                        if ( v == "incrclamp" || v == "incrementclamp" )
                            return StateStencilOp::IncrementClamp;
                        if ( v == "decrclamp" || v == "decrementclamp" )
                            return StateStencilOp::DecrementClamp;
                        if ( v == "invert" )
                            return StateStencilOp::Invert;
                        if ( v == "incrwrap" || v == "incrementwrap" )
                            return StateStencilOp::IncrementWrap;
                        if ( v == "decrwrap" || v == "decrementwrap" )
                            return StateStencilOp::DecrementWrap;
                        return std::nullopt;
                    };

                    const std::string cmpS = Lower( ReadIdent( c ) );
                    auto              cmp  = compare( cmpS );
                    if ( !cmp )
                    {
                        err = { line, "unknown Stencil compare '" + cmpS + "'" };
                        return false;
                    }
                    float ref = 0;
                    if ( !ReadNumber( c, ref, err ) )
                        return false;

                    state.StencilTest    = true;
                    state.StencilCompare = cmp;
                    state.StencilRef     = static_cast<uint32_t>( ref );

                    // Optional 3 ops: peek — if the next ident is a stencil op, consume fail/pass/depthFail
                    // (Cursor has a reference member so it isn't assignable; rewind via Pos/Line).
                    const size_t   savePos  = c.Pos;
                    const uint32_t saveLine = c.Line;
                    if ( stencilOp( Lower( ReadIdent( c ) ) ) )
                    {
                        c.Pos                  = savePos;
                        c.Line                 = saveLine;
                        state.StencilFail      = stencilOp( Lower( ReadIdent( c ) ) );
                        state.StencilPass      = stencilOp( Lower( ReadIdent( c ) ) );
                        state.StencilDepthFail = stencilOp( Lower( ReadIdent( c ) ) );
                        if ( !state.StencilFail || !state.StencilPass || !state.StencilDepthFail )
                        {
                            err = { line, "Stencil expects 3 ops (fail pass depthFail) when any is given" };
                            return false;
                        }
                    }
                    else
                    {
                        c.Pos  = savePos; // not ops -> leave for the next State command
                        c.Line = saveLine;
                    }
                }
                else if ( cmd == "topology" )
                {
                    const std::string v = Lower( ReadIdent( c ) );
                    if ( v == "triangles" )
                        state.Topology = StateTopology::Triangles;
                    else if ( v == "lines" )
                        state.Topology = StateTopology::Lines;
                    else if ( v == "points" )
                        state.Topology = StateTopology::Points;
                    else if ( v == "patches" )
                    {
                        state.Topology = StateTopology::Patches;
                        float n        = 0;
                        if ( !ReadNumber( c, n, err ) )
                            return false;
                        state.PatchControlPoints = static_cast<uint32_t>( n );
                    }
                    else
                    {
                        err = { line, "unknown Topology '" + v + "'" };
                        return false;
                    }
                }
                else
                {
                    err = { line, "unknown State command '" + rawCmd + "'" };
                    return false;
                }
            }
        }

        // ─── Stage GLSL assembly ────────────────────────────────────────────────────

        struct RawBlock
        {
            std::string Content;
            uint32_t    StartLine = 1;
        };

        // The fields a surface function ASSIGNS: every `.<Field>` (then any swizzle or index) followed by an
        // assignment operator. What a shading model's Inputs are checked against (MissingInput) — a model that
        // reads a field the surface never writes would shade from DefaultSurfaceOutput's placeholder.
        std::vector<std::string> SurfaceWrittenFields( const std::string_view surface )
        {
            const auto isIdent = []( const char ch )
            { return std::isalnum( static_cast<unsigned char>( ch ) ) != 0 || ch == '_'; };
            const auto skipSpace = [&]( std::size_t at )
            {
                while ( at < surface.size() && std::isspace( static_cast<unsigned char>( surface[at] ) ) != 0 )
                    ++at;
                return at;
            };
            std::vector<std::string> fields;
            for ( std::size_t dot = surface.find( '.' ); dot != std::string_view::npos;
                  dot             = surface.find( '.', dot + 1 ) )
            {
                std::size_t end = skipSpace( dot + 1 );
                if ( end >= surface.size() ||
                     ( std::isalpha( static_cast<unsigned char>( surface[end] ) ) == 0 && surface[end] != '_' ) )
                    continue;
                const std::size_t nameStart = end;
                while ( end < surface.size() && isIdent( surface[end] ) )
                    ++end;
                const std::string_view name = surface.substr( nameStart, end - nameStart );
                // `.Normal.xy = ...`, `.BaseColor[0] = ...`: a write to part of the field writes the field.
                for ( std::size_t next = skipSpace( end ); next < surface.size(); next = skipSpace( end ) )
                {
                    if ( surface[next] == '.' )
                    {
                        end = skipSpace( next + 1 );
                        while ( end < surface.size() && isIdent( surface[end] ) )
                            ++end;
                    }
                    else if ( surface[next] == '[' )
                    {
                        const std::size_t close = surface.find( ']', next );
                        end                     = close == std::string_view::npos ? surface.size() : close + 1;
                    }
                    else
                    {
                        end = next;
                        break;
                    }
                }
                const std::string_view rest    = surface.substr( std::min( end, surface.size() ) );
                const bool             assigns = ( rest.starts_with( "=" ) && !rest.starts_with( "==" ) ) ||
                                     rest.starts_with( "+=" ) || rest.starts_with( "-=" ) ||
                                     rest.starts_with( "*=" ) || rest.starts_with( "/=" );
                if ( assigns && std::ranges::find( fields, name ) == fields.end() )
                    fields.emplace_back( name );
            }
            return fields;
        }

        // Auto-generated resource declarations from the Properties block (opt-in via Binding /
        // TextureBinding). Injected into the fragment stage (and compute, for kernel shaders) —
        // material parameters belong to shading, and a single-stage declaration keeps the
        // reflection unambiguous.
        //
        // WHAT `Binding(n)` MEANS, AND WHAT IT USED TO MEAN. It now declares a ROW of the shared
        // `Materials[]` storage buffer plus the `#define` that names this draw's row. It used to declare
        // `uniform MaterialUB` — a block per material, which IS the parameters, so a material could hold
        // exactly one set of values and several objects sharing a shader all rendered the first one's
        // (Core/Formats/MaterialParamRow.hpp names the probe scene and the measurement). Nothing about a
        // shader's source had to change for the swap: both spellings answer to `Material.<Name>`, and
        // the difference is only whether that name resolves to a block or to a row.
        //
        // The push constant carrying the row index is an INCLUDE and not text emitted here, because it is
        // identical in every shader in the engine and the offsets have to stay identical too — the same
        // reason the generator emits structure and lets `.glslh` files carry boilerplate.
        //
        // THE LAYOUT IS THE INPUT. Every declaration below is written from Core::Formats::MaterialLayout (the
        // one builder, kept on the parse result), so the GLSL cannot name a parameter, an offset or a binding
        // the layout does not have — and ReconcileMaterialLayout holds the compiled SPIR-V to the same object.
        //
        // A RAY TRACING HIT CELL (kSurfaceRayHitPath) has no draw and so no push constant and no bound row: its
        // row travels in the hit group's shader record (Common/RayTracingHitRecord.glslh), UE's
        // FRayTracingMeshCommand loose parameters. @p rowByShaderRecord declares the same MaterialParams struct
        // and names `Material` through the record's device address instead of a Materials[] binding. Bound
        // (non-row) textures have no place in a record; the parser gives a template with any no hit cell
        // (SurfaceTemplateInfo::RayHitRefusal).
        std::string BuildAutoDeclarations( const MaterialLayout& layout, const bool rowByShaderRecord = false )
        {
            std::ostringstream out;

            if ( rowByShaderRecord )
            {
                // The bindless table, not the engine push block (MaterialTransport.glslh): a hit group has no
                // draw, so no push constants - a pipeline's push range is its ray generation program's.
                out << "#include <" << kParserInjectedIncludes[1] << ">\n";
                out << "#include <" << kSurfaceRayHitRecordInclude << ">\n";
                if ( layout.RowBinding )
                {
                    out << "struct MaterialParams\n{\n";
                    uint32_t slot = 0;
                    for ( const auto& p : layout.Params )
                    {
                        out << "    "
                            << ( p.IsTexture ? kMaterialTextureGlslType : MaterialLayoutGlslType( p.Type ) ) << " "
                            << p.Name << ";\n";
                        if ( const uint32_t pad = ( kMaterialParamSlotSize - p.Size ) / 4; pad > 0 )
                            out << "    float _slotPad" << slot << "[" << pad << "];\n";
                        ++slot;
                    }
                    out << "};\n";
                    out << "layout( buffer_reference, std430, buffer_reference_align = 16 ) readonly buffer "
                           "DesertMaterialRowRef\n{\n    MaterialParams Row;\n};\n";
                    out << "#define Material DesertMaterialRowRef( RayHitRecord.MaterialRow ).Row\n";
                }
                return out.str();
            }

            if ( layout.RowBinding )
            {
                // The table header first: it enables GL_EXT_nonuniform_qualifier, which has to precede every
                // declaration, and every row may carry a BindlessTexture. Always, not only for a row with a
                // texture: the surface contract's material fetch (Mesh/Surface/SurfaceTypes.glslh) takes one.
                out << "#include <" << kParserInjectedIncludes[1] << ">\n";
                out << "#include <" << kParserInjectedIncludes[0] << ">\n";
                out << "struct MaterialParams\n{\n";
                uint32_t slot = 0;
                for ( const auto& p : layout.Params )
                {
                    out << "    " << ( p.IsTexture ? kMaterialTextureGlslType : MaterialLayoutGlslType( p.Type ) )
                        << " " << p.Name << ";\n";
                    // The pad is a float ARRAY (std430 stride 4), so the value plus its pad is exactly one
                    // 16-byte slot whatever the value's own type is (see Core/Formats/MaterialParamRow.hpp).
                    if ( const uint32_t pad = ( kMaterialParamSlotSize - p.Size ) / 4; pad > 0 )
                        out << "    float _slotPad" << slot << "[" << pad << "];\n";
                    ++slot;
                }
                out << "};\n";
                out << "layout( std430, binding = " << *layout.RowBinding << " ) readonly buffer Materials\n"
                    << "{\n    MaterialParams MaterialRows[];\n};\n";
                // The row index: the push block's, unless the stage named its own first (a binned cell,
                // kSurfaceMaterialRowMacro).
                out << "#ifndef " << kSurfaceMaterialRowMacro << "\n#define " << kSurfaceMaterialRowMacro
                    << " m_PushConstants.MaterialIndex\n#endif\n";
                out << "#define Material MaterialRows[" << kSurfaceMaterialRowMacro << "]\n";
            }

            // A bound texture is a separate texture plus its own sampler state; the define keeps every read in
            // the template's source spelled `texture( <Name>, uv )`.
            for ( const auto& t : layout.Textures )
            {
                const std::string_view textureType  = t.IsCube ? "textureCube" : "texture2D";
                const std::string_view combinedType = t.IsCube ? "samplerCube" : "sampler2D";
                out << std::format( "layout( binding = {0} ) uniform {1} {2};\n"
                                    "layout( binding = {3} ) uniform sampler {2}{4};\n"
                                    "#define {2} {5}({2}, {2}{4})\n",
                                    t.Binding, textureType, t.Name, t.SamplerBinding,
                                    kMaterialTextureSamplerSuffix, combinedType );
            }

            return out.str();
        }

        // Pass-level State overrides file-level State field-by-field (only the settings the
        // pass actually specifies).
        ShaderRenderState MergeState( const ShaderRenderState& base, const ShaderRenderState& over )
        {
            ShaderRenderState out = base;
            if ( over.Cull )
                out.Cull = over.Cull;
            if ( over.DepthTest )
                out.DepthTest = over.DepthTest;
            if ( over.DepthWrite )
                out.DepthWrite = over.DepthWrite;
            if ( over.DepthCompare )
                out.DepthCompare = over.DepthCompare;
            if ( over.Blend )
                out.Blend = over.Blend;
            if ( over.BlendSrc )
                out.BlendSrc = over.BlendSrc;
            if ( over.BlendDst )
                out.BlendDst = over.BlendDst;
            if ( over.Topology )
                out.Topology = over.Topology;
            if ( over.PatchControlPoints )
                out.PatchControlPoints = over.PatchControlPoints;
            if ( over.StencilTest )
            {
                out.StencilTest      = over.StencilTest;
                out.StencilCompare   = over.StencilCompare;
                out.StencilRef       = over.StencilRef;
                out.StencilFail      = over.StencilFail;
                out.StencilPass      = over.StencilPass;
                out.StencilDepthFail = over.StencilDepthFail;
            }
            return out;
        }

        // Desert shader-language sugar over GLSL's `layout(...)` qualifiers — a thin, ShaderLab-flavoured
        // vocabulary that translates 1:1 to plain GLSL. Every rule is SINGLE-LINE so the line count is
        // preserved and `#line`-based error mapping stays exact. Raw `layout(...)` still works verbatim
        // (the sugar keywords are Capitalized; GLSL keywords are lowercase, so they never collide):
        //   In(n)  T x;         -> layout(location = n) in  T x;
        //   Out(n) T x;         -> layout(location = n) out T x;
        //   Uniform(n) ...      -> layout(binding = n) uniform ...            (UBO block or a sampler)
        //   Uniform(s, n) ...   -> layout(set = s, binding = n) uniform ...   (explicit descriptor set)
        //   Buffer(n) ...       -> layout(std430, binding = n) buffer ...
        //   ReadBuffer(n) ...   -> layout(std430, binding = n) readonly  buffer ...
        //   WriteBuffer(n) ...  -> layout(std430, binding = n) writeonly buffer ...
        //   LocalSize(x, y, z)  -> layout(local_size_x = x, local_size_y = y, local_size_z = z) in
        //   PushConstant ...    -> layout(push_constant) uniform ...          (block name + instance kept)
        //
        // AUTO NUMBERS (drop the parentheses): `In T x;` / `Out T x;` / `Uniform Name {}` / `Buffer Name {}`
        // / `ReadBuffer`/`WriteBuffer` with NO (n) auto-allocate the lowest free slot, in declaration order.
        // Three independent spaces: `in` locations, `out` locations, and descriptor bindings; auto slots skip
        // any EXPLICIT number already present so the two can be mixed.
        //
        // THE SCOPE IS ONE CALL OF THIS FUNCTION, WHICH IS NARROWER THAN A STAGE — this paragraph said "per
        // STAGE" until 2026-09-08 and the code has never done that. AssembleStage translates the DSL `Include`
        // block and the stage body in TWO separate calls, and ShaderIncluder hands every `#include`d `.glslh`
        // its own call as well. So each of those texts starts counting from zero, and two auto declarations in
        // two of them collide. Nothing in the tree does it today (measured 2026-09-08: NO shipped `.shader` or
        // `.glslh` uses ANY paren-less form, for a location or for a binding), which is why it has never been
        // seen — but the guarantee the old wording offered was not one this function can make. Anything SHARED —
        // a resource an include declares, one a second stage also names, or one a C++ site binds by a fixed
        // number — must keep its EXPLICIT (n).
        //
        // AND THE SEED IS DIGITS, SO A MACRO IS INVISIBLE TOO. The occupancy scan below reads `binding = (\d+)`,
        // and three shipped headers spell the number as a macro instead — Common/CloudAuthored.glslh,
        // Common/CloudParams.glslh, Common/FogParams.glslh. An auto declaration added to one of those files
        // would be handed a number that file has already spent. Widening the pattern does not fix it: the
        // file-scope blindness above defeats any pattern, because the number an auto form collides with usually
        // lives in a text this call never sees. It is caught after compilation instead, where a number is a
        // number whatever spelled it — ShaderReflection::ReflectStage refuses a descriptor slot claimed twice
        // and names both resources, and Tests/Engine/ShaderCacheKey asserts it over every shipped shader.
        //
        // Storage-image format qualifiers (`layout(binding=n, rgba32f) uniform imageCube`) and tessellation
        // layout (`layout(vertices=n) out`, `layout(quads,...) in`) are inherently GLSL-structural and stay
        // as raw `layout(...)` — the only sanctioned escape (DShaderTool allows exactly these forms).
        //
        // COMMENTS ARE PROSE AND ARE NEVER TRANSLATED. Five of the keywords — In, Out, Uniform, Buffer,
        // PushConstant — are ordinary English words, and the sugar used to search the whole file text.
        // `// Integrate alive particles. In LOCAL mode ...` in ParticleSimulate.shader really was rewritten
        // to `... layout(location = 0) in LOCAL mode ...` and really did consume location 0; it was harmless
        // only because a compute stage declares no automatic In/Out, so nobody ever asked for the eaten
        // number. Whoever added the first `In`/`Out` to a stage whose prose contains one of the five would
        // have found the attributes shifted by one and would have gone looking in the code, not in the
        // paragraph above it.

        // Splits `src` into alternating CODE and COMMENT runs, concatenating back to `src` exactly.
        // Comments are what GLSL says they are — `//` to end of line, `/* */` non-nesting — and nothing
        // else. This is a two-state scanner rather than a lexer on purpose: the only thing the sugar has
        // to not-see is a comment, so identifying comments is the whole job, and a full shader lexer would
        // buy nothing but the string literals GLSL does not have.
        struct SourceRun
        {
            std::string Text;
            bool        IsCode;
        };

        std::vector<SourceRun> SplitCodeAndComments( const std::string& src )
        {
            std::vector<SourceRun> runs;
            size_t                 codeBegin = 0;

            const auto flushCode = [&]( size_t end )
            {
                if ( end > codeBegin )
                    runs.push_back( { src.substr( codeBegin, end - codeBegin ), true } );
            };

            for ( size_t i = 0; i < src.size(); )
            {
                if ( src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/' )
                {
                    flushCode( i );
                    const size_t end  = src.find( '\n', i );
                    const size_t stop = ( end == std::string::npos ) ? src.size() : end; // the '\n' is code
                    runs.push_back( { src.substr( i, stop - i ), false } );
                    i = codeBegin = stop;
                }
                else if ( src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*' )
                {
                    flushCode( i );
                    const size_t close = src.find( "*/", i + 2 );
                    // An unterminated block comment swallows the rest of the file — the same thing GLSL's
                    // own preprocessor does with it, so the sugar and the compiler agree about where code
                    // stopped instead of disagreeing silently.
                    const size_t stop = ( close == std::string::npos ) ? src.size() : close + 2;
                    runs.push_back( { src.substr( i, stop - i ), false } );
                    i = codeBegin = stop;
                }
                else
                {
                    ++i;
                }
            }
            flushCode( src.size() );
            return runs;
        }

        // WHICH capitalized keywords stand as whole words in the CODE of this text.
        //
        // Every rule below can only fire where its own keyword appears, so this one hand-written scan
        // answers all fifteen regex questions at once — and the shipped tree answers "no" to most of them.
        // Asking std::regex instead costs a full scan of the text PER PATTERN, at every startup, for every
        // shader and every included header: measured over the 76 `.shader` + 33 `.glslh` of the shipped
        // tree in Debug, the fifteen unconditional passes were 4.7 s and 2.3 s respectively (Г17).
        //
        // Whole-word is the right test because every pattern anchors its keyword with `\b` on the left and
        // is followed either by `\b` or by `\s*\(`, and `(` is not an identifier character either way. So a
        // keyword absent as a whole word cannot be matched by any of them, and skipping is exact rather
        // than approximate — the translated text is byte-identical with and without this gate.
        enum SugarKeyword : uint8_t
        {
            KwIn,
            KwOut,
            KwUniform,
            KwReadBuffer,
            KwWriteBuffer,
            KwBuffer,
            KwLocalSize,
            KwPushConstant,
            KwCount
        };

        using KeywordSet = std::array<bool, KwCount>;

        KeywordSet PresentSugarKeywords( const std::span<const SourceRun> runs )
        {
            static constexpr std::string_view kNames[KwCount] = {
                 "In", "Out", "Uniform", "ReadBuffer", "WriteBuffer", "Buffer", "LocalSize", "PushConstant" };
            KeywordSet present{};
            for ( const auto& run : runs )
            {
                if ( !run.IsCode )
                    continue;

                const std::string& text = run.Text;
                for ( size_t i = 0; i < text.size(); )
                {
                    if ( !IsIdentChar( text[i] ) )
                    {
                        ++i;
                        continue;
                    }
                    const size_t begin = i;
                    while ( i < text.size() && IsIdentChar( text[i] ) )
                        ++i;

                    const std::string_view word( text.data() + begin, i - begin );
                    for ( uint8_t k = 0; k < KwCount; ++k )
                        present[k] = present[k] || word == kNames[k];
                }
            }
            return present;
        }

        // The names of the stage inputs @p src declares in code (comments skipped): `In(N) <type> <name>;`,
        // `In <type> <name>;` and `layout( ... location ... ) in [flat] <type> <name>;`.
        std::vector<std::string> InputDeclarationNames( const std::string& src )
        {
            static const std::regex kInput(
                 R"((?:\bIn\s*\(\s*\d+\s*\)|\bIn\b|\blayout\s*\([^)]*\blocation\b[^)]*\)\s*in\b)\s+(?:(?:flat|smooth|noperspective)\s+)?\w+\s+(\w+)\s*;)" );
            std::vector<std::string> names;
            for ( const SourceRun& run : SplitCodeAndComments( src ) )
            {
                if ( !run.IsCode )
                    continue;
                for ( auto it = std::sregex_iterator( run.Text.begin(), run.Text.end(), kInput );
                      it != std::sregex_iterator(); ++it )
                    names.push_back( ( *it )[1].str() );
            }
            return names;
        }

        std::string TranslateLayoutSugar( const std::string& src )
        {
            std::vector<SourceRun> runs = SplitCodeAndComments( src );

            // (1) EXPLICIT forms first — a numbered declaration always wins and is left untouched by the
            // auto pass below (the capitalized keyword is consumed here).
            struct Rule
            {
                SugarKeyword Keyword;
                std::regex   Pattern;
                const char*  Replacement;
            };
            static const Rule kRules[] = {
                 { KwIn, std::regex( R"(\bIn\s*\(\s*(\d+)\s*\))" ), "layout(location = $1) in" },
                 { KwOut, std::regex( R"(\bOut\s*\(\s*(\d+)\s*\))" ), "layout(location = $1) out" },
                 { KwUniform, std::regex( R"(\bUniform\s*\(\s*(\d+)\s*,\s*(\d+)\s*\))" ),
                   "layout(set = $1, binding = $2) uniform" },
                 { KwUniform, std::regex( R"(\bUniform\s*\(\s*(\d+)\s*\))" ), "layout(binding = $1) uniform" },
                 { KwReadBuffer, std::regex( R"(\bReadBuffer\s*\(\s*(\d+)\s*\))" ),
                   "layout(std430, binding = $1) readonly buffer" },
                 { KwWriteBuffer, std::regex( R"(\bWriteBuffer\s*\(\s*(\d+)\s*\))" ),
                   "layout(std430, binding = $1) writeonly buffer" },
                 { KwBuffer, std::regex( R"(\bBuffer\s*\(\s*(\d+)\s*\))" ),
                   "layout(std430, binding = $1) buffer" },
                 { KwLocalSize, std::regex( R"(\bLocalSize\s*\(\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\))" ),
                   "layout(local_size_x = $1, local_size_y = $2, local_size_z = $3) in" },
                 { KwPushConstant, std::regex( R"(\bPushConstant\b)" ), "layout(push_constant) uniform" },
            };

            // Gated PER RUN, not per text: a stage body is split into dozens of code runs by its comments, and
            // a keyword present somewhere in the text is absent from most of them. Asking the regex of a run
            // that lacks the keyword was the bulk of the cold preprocess (AL1-12a, measured in Debug: 2.1 s
            // over 78 programs); the gate is exact for the same whole-word reason as above.
            for ( auto& run : runs )
            {
                if ( !run.IsCode )
                    continue;
                const KeywordSet explicitForms = PresentSugarKeywords( std::span<const SourceRun>( &run, 1 ) );
                for ( const auto& rule : kRules )
                    if ( explicitForms[rule.Keyword] )
                        run.Text = std::regex_replace( run.Text, rule.Pattern, rule.Replacement );
            }

            // The keywords that SURVIVED the explicit pass are the paren-less ones — `Uniform(0)` has
            // become `layout(binding = 0) uniform` and no longer reads as a keyword at all. A text with
            // none left needs neither the occupancy scan nor the allocation pass, which is every shipped
            // shader today: not one uses an automatic form.
            const KeywordSet autoForms = PresentSugarKeywords( runs );
            const bool       autoIn    = autoForms[KwIn];
            const bool       autoOut   = autoForms[KwOut];
            const bool autoBind = autoForms[KwUniform] || autoForms[KwReadBuffer] || autoForms[KwWriteBuffer] ||
                                  autoForms[KwBuffer];

            // (2) Seed each auto-allocator with the EXPLICIT numbers already present so auto never collides.
            // Over the CODE runs only: a number quoted in a comment is documentation, not an occupancy.
            const auto collect = [&]( const std::regex& re, std::set<int>& used )
            {
                for ( const auto& run : runs )
                {
                    if ( !run.IsCode )
                        continue;
                    for ( std::sregex_iterator it( run.Text.begin(), run.Text.end(), re ), end; it != end; ++it )
                        used.insert( std::stoi( ( *it )[1].str() ) );
                }
            };
            std::set<int> usedIn, usedOut, usedBind;
            if ( autoIn )
                collect( std::regex( R"(location\s*=\s*(\d+)\s*\)\s*in\b)" ), usedIn );
            if ( autoOut )
                collect( std::regex( R"(location\s*=\s*(\d+)\s*\)\s*out\b)" ), usedOut );
            if ( autoBind )
                collect( std::regex( R"(binding\s*=\s*(\d+))" ), usedBind );

            const auto alloc = []( std::set<int>& used )
            {
                int n = 0;
                while ( used.count( n ) )
                    ++n;
                used.insert( n );
                return n;
            };

            // (3) Replace paren-less forms left-to-right (declaration order). Replacing the FIRST occurrence
            // each pass consumes the capitalized keyword, so the next search advances to the next one; the
            // lowercase `in`/`out`/`uniform`/`buffer` in the replacement is never re-matched.
            //
            // Each allocator is drained run by run in text order, which is the same order it saw when the
            // three passes swept one flat string: the runs are ordered, and a counter only cares about the
            // order of ITS OWN keyword.
            const auto replaceFirst = []( std::string& text, const std::regex& re,
                                          const std::function<std::string( const std::smatch& )>& make )
            {
                std::smatch m;
                while ( std::regex_search( text, m, re ) )
                    text = m.prefix().str() + make( m ) + m.suffix().str();
            };

            static const std::regex kAutoIn( R"(\bIn\b(?!\s*\())" );
            static const std::regex kAutoOut( R"(\bOut\b(?!\s*\())" );
            // One combined pattern over the four binding keywords so they share ONE binding space in text order.
            static const std::regex kAutoBinding( R"(\b(Uniform|ReadBuffer|WriteBuffer|Buffer)\b(?!\s*\())" );
            // `Uniform sampler <name>;` is a SHARED SAMPLER STATE: it takes no number from this text (which is
            // blind to the other stages, the included headers and every macro-spelled binding) but the marker the
            // program build renumbers once every stage is compiled (SharedSamplerBinding.hpp).
            static const std::regex kSharedSampler( R"(^\s+sampler\b)" );

            std::string out;
            for ( auto& run : runs )
            {
                if ( run.IsCode )
                {
                    if ( autoIn )
                        replaceFirst( run.Text, kAutoIn, [&]( const std::smatch& )
                                      { return std::format( "layout(location = {}) in", alloc( usedIn ) ); } );
                    if ( autoOut )
                        replaceFirst( run.Text, kAutoOut, [&]( const std::smatch& )
                                      { return std::format( "layout(location = {}) out", alloc( usedOut ) ); } );
                    if ( autoBind )
                        replaceFirst( run.Text, kAutoBinding,
                                      [&]( const std::smatch& m )
                                      {
                                          const std::string kw = m[1].str();
                                          if ( kw == "Uniform" &&
                                               std::regex_search( m.suffix().first, m.suffix().second,
                                                                  kSharedSampler ) )
                                              return std::format( "layout(binding = {}) uniform",
                                                                  kAutoSamplerBinding );
                                          const int n = alloc( usedBind );
                                          if ( kw == "Uniform" )
                                              return std::format( "layout(binding = {}) uniform", n );
                                          if ( kw == "ReadBuffer" )
                                              return std::format( "layout(std430, binding = {}) readonly buffer",
                                                                  n );
                                          if ( kw == "WriteBuffer" )
                                              return std::format( "layout(std430, binding = {}) writeonly buffer",
                                                                  n );
                                          return std::format( "layout(std430, binding = {}) buffer", n );
                                      } );
                }
                out += run.Text;
            }
            return out;
        }

        std::string AssembleStage( ShaderStage stage, const RawBlock& code, const RawBlock& include,
                                   const std::string& autoDecls )
        {
            std::ostringstream out;
            // 460, not 450: GL_EXT_ray_query (rayQueryEXT, accelerationStructureEXT) requires GLSL 460, and
            // one version for every stage keeps every program on the same language rules.
            out << "#version 460\n";

            // A stage that names a ray-query type gets the extension; every other stage compiles exactly
            // as before, with no OpExtension and no RayQueryKHR capability a device without ray query
            // would refuse. An #extension must precede every declaration, so the stage text cannot
            // enable it itself below the automatic declarations; a ray-query type used only inside an
            // #include'd header is not seen here and fails in glslang naming the undeclared type.
            static const std::regex kRayQueryType( R"(\b(rayQueryEXT|accelerationStructureEXT)\b)" );
            if ( std::regex_search( code.Content, kRayQueryType ) ||
                 std::regex_search( include.Content, kRayQueryType ) )
                out << "#extension GL_EXT_ray_query : require\n";

            // Buffer references (the GPU skin cache's streams, Mesh/Surface/Vertex_Skinned.glslh) are declared in
            // #include'd headers this text cannot see, and an #extension in a header lands after declarations,
            // where glslang refuses it. Enabled for every stage: `enable` adds no capability by itself (glslang
            // emits PhysicalStorageBufferAddresses only into a module that declares a buffer_reference block),
            // and the target (kShaderTargetEnvVersion, Vulkan 1.2 / SPIR-V 1.5) has the storage class in core.
            out << "#extension GL_EXT_buffer_reference : enable\n";
            out << "#extension GL_EXT_buffer_reference_uvec2 : enable\n";

            // A ray tracing stage (a program's RayGen / Miss, a surface template's hit cell) gets the pipeline's
            // extension and DESERT_RAY_TRACING_STAGE, the one name a header shared with a compute program guards
            // its compute-only parts (shared memory, LocalSize) with.
            if ( Formats::IsRayTracingStage( stage ) )
                out << "#extension GL_EXT_ray_tracing : require\n#define DESERT_RAY_TRACING_STAGE 1\n";

            if ( !autoDecls.empty() && ( stage == ShaderStage::Fragment || stage == ShaderStage::Compute ||
                                         stage == ShaderStage::ClosestHit || stage == ShaderStage::AnyHit ) )
                out << autoDecls;

            if ( !include.Content.empty() )
            {
                // GLSL: after `#line N`, the NEXT line is numbered N+1.
                out << "#line " << ( include.StartLine > 0 ? include.StartLine - 1 : 0 ) << "\n";
                out << TranslateLayoutSugar( include.Content ) << "\n";
            }

            out << "#line " << ( code.StartLine > 0 ? code.StartLine - 1 : 0 ) << "\n";
            out << TranslateLayoutSugar( code.Content );

            return out.str();
        }

        // Every name an `OpacityMask` line states must be a parameter of the right kind: a 2D texture, a Float
        // channel, Vec2 tiling and offset, a Float rotation. Empty = sound.
        std::string OpacityMaskParamError( const Formats::SurfaceOpacityMask& mask,
                                           const std::vector<ShaderParam>&    params )
        {
            const auto find = [&]( const std::string& name ) -> const ShaderParam*
            {
                const auto it = std::find_if( params.begin(), params.end(),
                                              [&]( const ShaderParam& p ) { return p.Name == name; } );
                return it == params.end() ? nullptr : &*it;
            };
            const ShaderParam* texture = find( mask.Texture );
            if ( texture == nullptr || !texture->IsTexture || texture->IsCubeTexture )
                return std::format( "OpacityMask names '{}', which is not a Texture2D of the Properties block",
                                    mask.Texture );
            const auto value = [&]( const std::string& name, const ShaderValueType type,
                                    const char* what ) -> std::string
            {
                const ShaderParam* p = find( name );
                if ( p == nullptr || p->IsTexture || p->Type != type )
                    return std::format( "OpacityMask {} '{}' is not a {} of the Properties block", what, name,
                                        type == ShaderValueType::Float ? "Float" : "Vec2" );
                return {};
            };
            if ( !mask.ChannelParam.empty() )
                if ( auto e = value( mask.ChannelParam, ShaderValueType::Float, "Channel" ); !e.empty() )
                    return e;
            if ( !mask.TilingParam.empty() )
            {
                if ( auto e = value( mask.TilingParam, ShaderValueType::Float2, "tiling" ); !e.empty() )
                    return e;
                if ( auto e = value( mask.OffsetParam, ShaderValueType::Float2, "offset" ); !e.empty() )
                    return e;
                if ( auto e = value( mask.RotationParam, ShaderValueType::Float, "rotation" ); !e.empty() )
                    return e;
            }
            return {};
        }

        std::string HeightParamError( const Formats::SurfaceHeight& height, const std::vector<ShaderParam>& params )
        {
            const auto find = [&]( const std::string& name ) -> const ShaderParam*
            {
                const auto it = std::find_if( params.begin(), params.end(),
                                              [&]( const ShaderParam& p ) { return p.Name == name; } );
                return it == params.end() ? nullptr : &*it;
            };
            const ShaderParam* texture = find( height.Texture );
            if ( texture == nullptr || !texture->IsTexture || texture->IsCubeTexture )
                return std::format( "Height names '{}', which is not a Texture2D of the Properties block",
                                    height.Texture );
            const auto value = [&]( const std::string& name, const ShaderValueType type,
                                    const char* what ) -> std::string
            {
                const ShaderParam* p = find( name );
                if ( p == nullptr || p->IsTexture || p->Type != type )
                    return std::format( "Height {} '{}' is not a {} of the Properties block", what, name,
                                        type == ShaderValueType::Float ? "Float" : "Vec2" );
                return {};
            };
            if ( auto e = value( height.ScaleParam, ShaderValueType::Float, "Scale" ); !e.empty() )
                return e;
            if ( !height.CenterParam.empty() )
                if ( auto e = value( height.CenterParam, ShaderValueType::Float, "Center" ); !e.empty() )
                    return e;
            if ( !height.ChannelParam.empty() )
                if ( auto e = value( height.ChannelParam, ShaderValueType::Float, "Channel" ); !e.empty() )
                    return e;
            if ( !height.TilingParam.empty() )
            {
                if ( auto e = value( height.TilingParam, ShaderValueType::Float2, "tiling" ); !e.empty() )
                    return e;
                if ( auto e = value( height.OffsetParam, ShaderValueType::Float2, "offset" ); !e.empty() )
                    return e;
                if ( auto e = value( height.RotationParam, ShaderValueType::Float, "rotation" ); !e.empty() )
                    return e;
            }
            return {};
        }

        // THE GENERATED HEIGHT READS of a template with a `Height` line, placed after the shared types and BEFORE the
        // surface block (whose SurfaceParallaxOcclusion call reaches them; Mesh/Surface/SurfaceParallax.glslh
        // declares their prototypes): the height map's UV, one fetch at explicit gradients (the march loops a
        // different number of times per lane), the scale in centimetres and the centre height.
        std::string HeightFunctions( const Formats::SurfaceHeight& height )
        {
            std::string uv = std::format( "i.UV{}", height.UVSet );
            std::string transform;
            if ( !height.TilingParam.empty() )
            {
                transform = std::format( "    vec2 tiling = Material.{};\n"
                                         "    if ( tiling.x <= 0.0 ) tiling.x = 1.0;\n"
                                         "    if ( tiling.y <= 0.0 ) tiling.y = 1.0;\n",
                                         height.TilingParam );
                uv        = std::format( "SurfaceTransformUV( {}, Material.{}, tiling, Material.{} )", uv,
                                         height.OffsetParam, height.RotationParam );
            }
            const std::string channel =
                 height.ChannelParam.empty()
                      ? std::to_string( height.Channel )
                      : std::format( "clamp( int( Material.{} ), 0, 3 )", height.ChannelParam );
            const std::string center =
                 height.CenterParam.empty() ? std::string( "0.0" )
                                            : std::format( "clamp( Material.{}, 0.0, 1.0 )", height.CenterParam );
            return std::format( "vec2 DesertSurfaceHeightUV( SurfaceInput i )\n{{\n{}    return {};\n}}\n"
                                "#ifndef DESERT_RAY_TRACING_STAGE\n"
                                "float DesertSurfaceHeightGrad( vec2 uv, vec2 dUVdx, vec2 dUVdy )\n{{\n"
                                "    return SampleBindlessGrad( Material.{}, uv, dUVdx, dUVdy )[{}];\n}}\n"
                                "#endif\n"
                                "float DesertSurfaceHeightScale()\n{{\n    return Material.{};\n}}\n"
                                "float DesertSurfaceHeightCenter()\n{{\n    return {};\n}}\n",
                                transform, uv, height.Texture, channel, height.ScaleParam, center );
        }

        // THE GENERATED OPACITY MASK of a Masked template: the GLSL of its `OpacityMask` line, placed after the
        // surface block and before the pass header, whose DESERT_SURFACE_CLIP calls it. The mask texture is a
        // BindlessTexture member of the material row (Core::Formats::IsRowTexture), read as the surface block
        // reads its maps. A tiling component <= 0 reads as 1 (the templates' own guard on UVTiling); the channel
        // parameter is clamped to 0..3.
        std::string OpacityMaskFunction( const Formats::SurfaceOpacityMask& mask )
        {
            std::string uv = std::format( "i.UV{}", mask.UVSet );
            std::string transform;
            if ( !mask.TilingParam.empty() )
            {
                transform = std::format( "    vec2 tiling = Material.{};\n"
                                         "    if ( tiling.x <= 0.0 ) tiling.x = 1.0;\n"
                                         "    if ( tiling.y <= 0.0 ) tiling.y = 1.0;\n",
                                         mask.TilingParam );
                uv        = std::format( "SurfaceTransformUV( {}, Material.{}, tiling, Material.{} )", uv,
                                         mask.OffsetParam, mask.RotationParam );
            }
            const std::string channel =
                 mask.ChannelParam.empty()
                      ? std::to_string( mask.Channel )
                      : std::format( "clamp( int( Material.{} ), 0, 3 )", mask.ChannelParam );
            // The dither switch is a macro, not a function: DESERT_SURFACE_CLIP reads gl_FragCoord and the view
            // block at its own (fragment) call site, and a function using either would not compile in a vertex
            // stage.
            const std::string dither =
                 mask.DitherParam.empty()
                      ? std::string()
                      : std::format( "#define DESERT_SURFACE_OPACITY_DITHER Material.{}\n", mask.DitherParam );
            return std::format( "{}float DesertSurfaceOpacityMask( SurfaceInput i )\n{{\n{}"
                                "    return SurfaceSampleMaterial( Material.{}, {} )[{}];\n}}\n",
                                dither, transform, mask.Texture, uv, channel );
        }

        // A SURFACE READS A WORLD PARAMETER BY NAME — `WorldParameter( "Wetness" )`, the one spelling a
        // hand-written surface and the shader graph's World Parameter node share — and the cell reads it by KEY:
        // each read is lowered to DesertWorldParameter( <key>u ), the name's Serialization::WorldParameterKey, so
        // the material is compiled against no particular collection (the scene names one). GLSL has no string
        // literals, so a quoted name after the keyword is unambiguous. A malformed read becomes an `#error` naming
        // it, which glslang reports on the .shader's own line. @p reads says whether the cell needs the table
        // declared.
        std::string LowerWorldParameterReads( const std::string_view content, bool& reads )
        {
            constexpr std::string_view keyword = "WorldParameter";
            std::string                out;
            out.reserve( content.size() );
            size_t at = 0;
            reads     = false;
            while ( at < content.size() )
            {
                const size_t found = content.find( keyword, at );
                if ( found == std::string_view::npos )
                    break;
                const bool identBefore = found > 0 && IsIdentChar( content[found - 1] );
                size_t     cursor      = found + keyword.size();
                const bool identAfter  = cursor < content.size() && IsIdentChar( content[cursor] );
                if ( identBefore || identAfter )
                {
                    out.append( content.substr( at, cursor - at ) );
                    at = cursor;
                    continue;
                }
                out.append( content.substr( at, found - at ) );
                const auto skipSpace = [&]
                {
                    while ( cursor < content.size() && ( content[cursor] == ' ' || content[cursor] == '\t' ) )
                        ++cursor;
                };
                skipSpace();
                std::string name;
                bool        wellFormed = cursor < content.size() && content[cursor] == '(';
                if ( wellFormed )
                {
                    ++cursor;
                    skipSpace();
                    wellFormed = cursor < content.size() && content[cursor] == '"';
                }
                if ( wellFormed )
                {
                    const size_t close = content.find( '"', cursor + 1 );
                    wellFormed         = close != std::string_view::npos;
                    if ( wellFormed )
                    {
                        name   = std::string( content.substr( cursor + 1, close - cursor - 1 ) );
                        cursor = close + 1;
                        skipSpace();
                        wellFormed = cursor < content.size() && content[cursor] == ')' &&
                                     Assets::Serialization::IsWorldParameterName( name );
                    }
                }
                if ( !wellFormed )
                {
                    // The rest of the line stays as written; the directive needs a line of its own.
                    const size_t eol = content.find( '\n', found );
                    out.append(
                         "\n#error WorldParameter: expected WorldParameter( \"Name\" ) with an identifier name" );
                    at = eol == std::string_view::npos ? content.size() : eol;
                    continue;
                }
                out.append( std::format( "DesertWorldParameter( {:#010x}u )",
                                         Assets::Serialization::WorldParameterKey( name ) ) );
                reads = true;
                at    = cursor + 1;
            }
            out.append( content.substr( std::min( at, content.size() ) ) );
            return out;
        }

        // Whether @p content names @p identifier as a whole word (not as part of a longer identifier).
        bool NamesIdentifier( const std::string_view content, const std::string_view identifier )
        {
            for ( size_t found = content.find( identifier ); found != std::string_view::npos;
                  found        = content.find( identifier, found + 1 ) )
            {
                const size_t end = found + identifier.size();
                if ( ( found == 0 || !IsIdentChar( content[found - 1] ) ) &&
                     ( end >= content.size() || !IsIdentChar( content[end] ) ) )
                    return true;
            }
            return false;
        }

        // One stage of one surface CELL. The vertex stage is the path's engine header alone (a surface
        // function does not move vertices yet); the fragment stage is the author's surface function between
        // the shared types and the pass's engine header, which owns main(). `#line` puts glslang's errors in
        // the surface function on the .shader's own lines. BOTH stages see which pass and blend mode the cell
        // is (DESERT_SURFACE_PASS_<Pass>, DESERT_SURFACE_MASKED): an opaque depth cell's vertex header declares
        // the shadow pass's push block and no varyings, so the cell's layout is the shadow shader's.
        std::string AssembleSurfaceCellStage( ShaderStage stage, std::string_view path, std::string_view pass,
                                              const RawBlock& surface, const RawBlock& include,
                                              const std::string& autoDecls, const SurfaceBlendMode blend,
                                              const std::string_view shadingModelDefine, const bool responsiveAA,
                                              const std::string& opacityMaskFunction, const bool parallax,
                                              const std::string& heightFunctions, const bool weather,
                                              const bool binned = false )
        {
            const bool        masked   = blend == SurfaceBlendMode::Masked;
            const bool        particle = path == kSurfaceParticleSpritePath || path == kSurfaceParticleRibbonPath;
            const std::string defines  = std::format(
                 "#define DESERT_SURFACE_PASS_{} 1\n{}{}{}{}{}{}#define {} {}\n", pass,
                 // The hit cell's two stages share one pass header; the any-hit stage is told which it is.
                 stage == ShaderStage::AnyHit ? "#define DESERT_SURFACE_RAY_ANY_HIT 1\n" : "",
                 masked ? "#define DESERT_SURFACE_MASKED 1\n" : "",
                 Formats::IsTranslucentBlend( blend ) ? std::format( "#define {} 1\n", kSurfaceTranslucentDefine )
                                                       : std::string(),
                 particle ? std::format( "#define {} 1\n", kSurfaceParticleSpriteDefine ) : std::string(),
                 responsiveAA ? std::format( "#define {} 1\n", kSurfaceResponsiveAADefine ) : std::string(),
                 parallax ? std::format( "#define {} 1\n", kSurfaceParallaxDefine ) : std::string(),
                 kSurfaceShadingModelDefine, shadingModelDefine ) +
                 ( binned ? std::format( "#define {} 1\n", kSurfaceBinnedDefine ) : std::string() );
            // A binned cell's fragment `Material` reads the row its vertex stage resolved (kSurfaceMaterialRowMacro):
            // named ahead of the generated declarations, which default it to the push block's.
            const std::string cellDecls =
                 binned ? std::format( "#define {} g_SurfaceMaterialRow\n{}", kSurfaceMaterialRowMacro, autoDecls )
                        : autoDecls;
            // A lit mesh pass of a template whose row carries the weather response (kSurfaceWeatherDefine).
            const bool wets =
                 weather && ( pass == "Forward" || pass == "GBuffer" ) && !particle && path != kSurfaceRayHitPath;
            const std::string weatherDefines =
                 wets ? std::format( "#define {} 1\n#define {} {:#010x}u\n", kSurfaceWeatherDefine,
                                     kSurfaceWetnessKeyDefine,
                                     Assets::Serialization::WorldParameterKey( kSurfaceWetnessParameter ) )
                      : std::string();
            // The sprite path has its own pass header whatever the blend: the mesh Forward / glass headers light a
            // mesh, the sprite header composites a particle.
            const bool        rayHit      = path == kSurfaceRayHitPath;
            const std::string passInclude = particle ? std::string( kSurfaceParticleSpritePassInclude )
                                            : rayHit ? std::string( kSurfaceRayHitPassInclude )
                                                     : SurfacePassInclude( pass, blend );
            RawBlock          code;
            code.StartLine = 1;
            if ( stage == ShaderStage::Vertex )
                code.Content = std::format( "{}#include <{}>\n#include <{}>\n", defines, kSurfaceTypesInclude,
                                            SurfaceVertexInclude( path ) );
            else if ( IsSurfaceDepthOnlyPass( pass ) && !masked && !parallax )
            {
                // Reflection keeps a declared binding whether or not it is read, so an opaque depth cell must
                // not declare the surface's bindings at all: the pass header alone, no material declarations.
                code.Content = std::format( "{}#include <{}>\n#include <{}>\n", defines, kSurfaceTypesInclude,
                                            SurfacePassInclude( pass, blend ) );
                return AssembleStage( stage, code, include, std::string() );
            }
            else
            {
                bool              readsWorld = false;
                const std::string lowered    = LowerWorldParameterReads( surface.Content, readsWorld );
                const bool fades = particle && NamesIdentifier( surface.Content, kSurfaceDepthFadeFunction );
                code.Content =
                     // Only a stage that evaluates the surface fetches material textures: it alone declares the
                     // biased fetch (Mesh/Surface/SurfaceTypes.glslh SurfaceSampleMaterial) and the camera block.
                     // The world's parameters are declared only into a cell whose surface reads one: reflection
                     // keeps a declared buffer, and every other surface must not ask its renderer to bind it.
                     std::format( "{}{}{}#define {} 1\n#include <{}>\n{}{}#line {}\n{}\n{}#include <{}>\n",
                                  defines, weatherDefines,
                                  fades ? std::format( "#define {} 1\n", kSurfaceDepthFadeDefine ) : std::string(),
                                  kSurfaceSamplesMaterialDefine, kSurfaceTypesInclude,
                                  readsWorld || wets ? std::format( "#include <{}>\n", kWorldParametersInclude )
                                                     : std::string(),
                                  heightFunctions, surface.StartLine > 0 ? surface.StartLine - 1 : 0, lowered,
                                  masked ? opacityMaskFunction : std::string(), passInclude );
            }
            return AssembleStage( stage, code, include, cellDecls );
        }

    } // namespace

    // ─── Public API ─────────────────────────────────────────────────────────────────

    bool DShaderParser::IsDShader( const std::string& source )
    {
        Cursor c{ source };
        SkipTrivia( c );
        size_t      probe = c.Pos;
        std::string ident;
        while ( probe < source.size() && IsIdentChar( source[probe] ) )
            ident.push_back( source[probe++] );
        return ident == "Shader";
    }

    bool DShaderParser::MayDeclareMedium( const std::string_view source )
    {
        constexpr std::string_view keyword = "medium";
        for ( size_t i = 0; i + keyword.size() <= source.size(); ++i )
        {
            size_t matched = 0;
            while ( matched < keyword.size() &&
                    std::tolower( static_cast<unsigned char>( source[i + matched] ) ) == keyword[matched] )
                ++matched;
            if ( matched == keyword.size() )
                return true;
        }
        return false;
    }

    bool DShaderParser::MayDeclareParticle( const std::string_view source )
    {
        // "domain", whitespace, "particle" — any case. A Particle block is legal only under that line.
        const auto lowerAt = [&]( size_t at, std::string_view word )
        {
            if ( at + word.size() > source.size() )
                return false;
            for ( size_t k = 0; k < word.size(); ++k )
                if ( std::tolower( static_cast<unsigned char>( source[at + k] ) ) != word[k] )
                    return false;
            return true;
        };
        for ( size_t i = 0; i < source.size(); ++i )
        {
            if ( !lowerAt( i, "domain" ) )
                continue;
            size_t j = i + 6;
            while ( j < source.size() && std::isspace( static_cast<unsigned char>( source[j] ) ) != 0 )
                ++j;
            if ( j > i + 6 && lowerAt( j, "particle" ) )
                return true;
        }
        return false;
    }

    bool DShaderParser::MayDeclareSurface( const std::string_view source )
    {
        constexpr std::string_view keyword = "surface";
        for ( size_t i = 0; i + keyword.size() <= source.size(); ++i )
        {
            if ( i > 0 && IsIdentChar( source[i - 1] ) )
                continue;
            size_t matched = 0;
            while ( matched < keyword.size() &&
                    std::tolower( static_cast<unsigned char>( source[i + matched] ) ) == keyword[matched] )
                ++matched;
            if ( matched != keyword.size() )
                continue;
            size_t next = i + matched;
            while ( next < source.size() && std::isspace( static_cast<unsigned char>( source[next] ) ) != 0 )
                ++next;
            if ( next < source.size() && source[next] == '{' )
                return true;
        }
        return false;
    }

    std::string SurfaceCellName( const std::string_view path, const std::string_view pass )
    {
        return std::format( "{}.{}", path, pass );
    }

    std::string SurfaceVertexInclude( const std::string_view path )
    {
        return std::format( "Mesh/Surface/Vertex_{}.glslh", path );
    }

    bool SurfaceBlendHasPass( const SurfaceBlendMode blend, const std::string_view pass )
    {
        return !Formats::IsTranslucentBlend( blend ) || pass == "Forward";
    }

    std::string SurfacePassInclude( const std::string_view pass, const SurfaceBlendMode blend )
    {
        // A translucent surface is shaded and composited over the scene by the translucency pass header (UE's
        // translucency base pass). The shading model picks no header: every model is lit by the same pass, which
        // dispatches on DESERT_SHADING_MODEL_INDEX.
        if ( Formats::IsTranslucentBlend( blend ) && pass == "Forward" )
            return std::string( kSurfaceTranslucentPassInclude );
        return std::format( "Mesh/Surface/Pass_{}.glslh", pass );
    }

    std::vector<std::string> SurfaceTemplateIncludes()
    {
        std::vector<std::string> includes{ std::string( kSurfaceTypesInclude ), std::string( kSurfaceParallaxInclude ) };
        for ( const std::string_view path : kSurfaceVertexPaths )
            includes.push_back( SurfaceVertexInclude( path ) );
        for ( const std::string_view pass : kSurfaceCellPasses )
            includes.push_back( SurfacePassInclude( pass, SurfaceBlendMode::Opaque ) );
        includes.emplace_back( kSurfaceTranslucentPassInclude );
        includes.emplace_back( kSurfaceFrontLayerPassInclude );
        includes.push_back( SurfaceVertexInclude( kSurfaceParticleSpritePath ) );
        includes.push_back( SurfaceVertexInclude( kSurfaceParticleRibbonPath ) );
        includes.emplace_back( kSurfaceParticleSpritePassInclude );
        includes.emplace_back( kSurfaceRayHitPassInclude );
        includes.emplace_back( kSurfaceRayHitRecordInclude );
        includes.push_back( SurfaceVertexInclude( kSurfaceGrassBladePath ) );
        return includes;
    }

    std::string DShaderParser::TranslateSugar( const std::string& source )
    {
        return TranslateLayoutSugar( source );
    }

    std::vector<std::string> DShaderParser::DeclaredInputNames( const std::string& source )
    {
        return InputDeclarationNames( source );
    }

    Common::ResultStr<DShaderParseResult> DShaderParser::Parse( const std::string& source )
    {
        DShaderParseResult result;
        ParseError         err{ 0, "" };

        const auto fail = [&err]()
        {
            return Common::MakeFormattedError<DShaderParseResult>( "DShader parse error (line {}): {}", err.Line,
                                                                   err.Message );
        };

        Cursor c{ source };

        if ( ReadIdent( c ) != "Shader" )
        {
            err = { c.Line, "file must start with: Shader \"Name\"" };
            return fail();
        }
        if ( !ReadQuoted( c, result.Name, err ) || result.Name.empty() )
        {
            if ( err.Message.empty() )
                err = { c.Line, "shader name must not be empty" };
            return fail();
        }
        if ( !Expect( c, '{', err, "opening the Shader body" ) )
            return fail();

        struct PendingPass
        {
            std::string                               Name;
            ShaderRenderState                         State;
            std::unordered_map<ShaderStage, RawBlock> Blocks;
        };

        PropertiesInfo           propInfo;
        std::vector<PendingImport> importRows;
        bool                       sawImport = false;
        RawBlock                 includeBlock;
        PendingPass              defaultPass; // top-level stage blocks
        std::vector<PendingPass> namedPasses;
        // The `Surface` block, expanded into cells after the loop; the line of the first TwoSided/BlendMode
        // names the error when those settings arrive without a block to shape.
        std::optional<RawBlock> surfaceBlock;
        uint32_t                surfaceLine        = 0;
        uint32_t                particleLine       = 0;
        uint32_t                surfaceSettingLine = 0;
        uint32_t                responsiveLine     = 0; // the `ResponsiveAA` line, refused on an opaque blend
        std::string             shadingModelName; // `ShadingModel <Name>`; empty = DefaultLit
        uint32_t                shadingModelLine = 0;
        uint32_t                opacityMaskLine  = 0; // the `OpacityMask` line; checked once Properties are known
        uint32_t                heightLine       = 0; // the `Height` line; checked once Properties are known

        // Reads one stage block into the given pass. Returns false + err on problems.
        const auto readStageBlock = [&]( Cursor& cur, PendingPass& pass, ShaderStage stage,
                                         const std::string& section, uint32_t line ) -> bool
        {
            if ( pass.Blocks.count( stage ) )
            {
                err = { line, "duplicate stage block '" + section + "'" };
                return false;
            }
            RawBlock block;
            if ( !ReadBlock( cur, block.Content, block.StartLine, err ) )
                return false;
            if ( block.Content.find( "#version" ) != std::string::npos )
            {
                err = { block.StartLine,
                        "stage blocks must not declare #version — the compiler emits the header" };
                return false;
            }
            // A VERTEX STAGE DECLARES NO INPUT OF ITS OWN: its inputs are generated from the C++ vertex format
            // (Graphic/ShaderProtocols/VertexInputs.hpp) and included (Mesh/MeshVertexInputs.glslh,
            // Common/UIVertexInputs.glslh). A hand-written `In(N)` here would be the second spelling of the
            // layout that used to drift from it unchecked.
            if ( stage == ShaderStage::Vertex )
                if ( const std::vector<std::string> inputs = InputDeclarationNames( block.Content ); !inputs.empty() )
                {
                    err = { block.StartLine,
                            std::format( "the Vertex stage declares the input '{}' by hand; vertex inputs are "
                                         "generated from the C++ vertex format — #include "
                                         "<Mesh/MeshVertexInputs.glslh>, <Mesh/MeshVertexInputsSkinned.glslh> "
                                         "or <Common/UIVertexInputs.glslh> instead",
                                         inputs.front() ) };
                    return false;
                }
            pass.Blocks.emplace( stage, std::move( block ) );
            return true;
        };

        while ( true )
        {
            SkipTrivia( c );
            if ( c.Peek() == '}' )
            {
                c.Advance();
                break;
            }
            if ( c.AtEnd() )
            {
                err = { c.Line, "unterminated Shader body (missing '}')" };
                return fail();
            }

            const uint32_t    line    = c.Line;
            const std::string section = ReadIdent( c );
            const std::string lower   = Lower( section );

            if ( lower == "domain" )
            {
                const std::string v = Lower( ReadIdent( c ) );
                if ( v == "surface" )
                    result.Meta.Domain = ShaderDomain::Surface;
                else if ( v == "terrain" )
                    result.Meta.Domain = ShaderDomain::Terrain;
                else if ( v == "skybox" )
                    result.Meta.Domain = ShaderDomain::Skybox;
                else if ( v == "postprocess" )
                    result.Meta.Domain = ShaderDomain::PostProcess;
                else if ( v == "volume" )
                    result.Meta.Domain = ShaderDomain::Volume;
                else if ( v == "ui" )
                    result.Meta.Domain = ShaderDomain::UI;
                else if ( v == "particle" )
                    result.Meta.Domain = ShaderDomain::Particle;
                else
                {
                    err = { line, "unknown Domain '" + v + "'" };
                    return fail();
                }
            }
            // The template MANIFEST (`Role <Name>`, `Default Surface`) is not program schema: its one reader is
            // Common::Content::ReadShaderManifest (ShaderAsset, the asset registry's Role tag). The compiler
            // only checks the lines are well formed, so a malformed manifest fails the compile by line too.
            else if ( lower == "role" )
            {
                if ( ReadIdent( c ).empty() )
                {
                    err = { line, "Role needs a name ('Role <Name>')" };
                    return fail();
                }
            }
            else if ( lower == "default" )
            {
                const std::string v = Lower( ReadIdent( c ) );
                if ( v != "surface" )
                {
                    err = { line, std::format( "unknown Default '{}' (the only one is 'Default Surface')", v ) };
                    return fail();
                }
            }
            else if ( lower == "import" )
            {
                if ( sawImport )
                {
                    err = { line, "a second Import block: a template has one import contract" };
                    return fail();
                }
                sawImport = true;
                if ( !ParseImportBlock( c, importRows, err ) )
                    return fail();
            }
            else if ( lower == "properties" )
            {
                if ( !ParsePropertiesBlock( c, result.Meta, propInfo, err ) )
                    return fail();
            }
            else if ( lower == "state" )
            {
                if ( !ParseStateBlock( c, defaultPass.State, err ) )
                    return fail();
            }
            else if ( lower == "include" )
            {
                if ( !ReadBlock( c, includeBlock.Content, includeBlock.StartLine, err ) )
                    return fail();
            }
            // A PROGRAM FRAGMENT, not a stage: the authored cloud medium, compiled INTO the four programs
            // that sample the cloud field rather than into a program of its own. See
            // ShaderProgramMeta::MediumSource for why it is a `.shader` and not a loose header.
            else if ( lower == "medium" )
            {
                if ( !result.Meta.MediumSource.empty() )
                {
                    err = { line, "duplicate Medium block" };
                    return fail();
                }
                RawBlock medium;
                if ( !ReadBlock( c, medium.Content, medium.StartLine, err ) )
                    return fail();
                if ( medium.Content.find( "#version" ) != std::string::npos )
                {
                    err = { medium.StartLine, "a Medium block must not declare #version — it is compiled "
                                              "INTO another program, which has already emitted one" };
                    return fail();
                }
                // Empty is refused rather than accepted: an empty Medium is indistinguishable from no
                // Medium at all in the metadata, so a material pointing at it would silently get the
                // default and the author would be told nothing.
                if ( medium.Content.find_first_not_of( " \t\r\n" ) == std::string::npos )
                {
                    err = { medium.StartLine, "a Medium block must not be empty — a material pointing at "
                                              "it would silently draw the default medium" };
                    return fail();
                }
                result.Meta.MediumSource = std::move( medium.Content );
            }
            // THE SECOND PROGRAM FRAGMENT (VFX-04): a stack module or a compiled emitter stack, compiled INTO
            // the particle simulation program. Same rules as the medium, for the same reasons.
            else if ( lower == "particle" )
            {
                if ( !result.Meta.ParticleSource.empty() )
                {
                    err = { line, "duplicate Particle block" };
                    return fail();
                }
                RawBlock particle;
                if ( !ReadBlock( c, particle.Content, particle.StartLine, err ) )
                    return fail();
                if ( particle.Content.find( "#version" ) != std::string::npos )
                {
                    err = { particle.StartLine, "a Particle block must not declare #version — it is compiled "
                                                "INTO the simulation program, which has already emitted one" };
                    return fail();
                }
                if ( particle.Content.find_first_not_of( " \t\r\n" ) == std::string::npos )
                {
                    err = { particle.StartLine, "a Particle block must not be empty — an emitter naming it "
                                                "would silently simulate nothing" };
                    return fail();
                }
                particleLine               = line;
                result.Meta.ParticleSource = std::move( particle.Content );
            }
            else if ( lower == "surface" )
            {
                if ( surfaceBlock )
                {
                    err = { line, "duplicate Surface block" };
                    return fail();
                }
                RawBlock block;
                if ( !ReadBlock( c, block.Content, block.StartLine, err ) )
                    return fail();
                if ( block.Content.find( "#version" ) != std::string::npos )
                {
                    err = { block.StartLine, "a Surface block must not declare #version — every cell's stages "
                                             "are assembled around it" };
                    return fail();
                }
                if ( block.Content.find( "EvaluateSurface" ) == std::string::npos )
                {
                    err = { block.StartLine, "a Surface block must define "
                                             "'SurfaceOutput EvaluateSurface( SurfaceInput )'" };
                    return fail();
                }
                surfaceLine  = line;
                surfaceBlock = std::move( block );
            }
            else if ( lower == "twosided" )
            {
                result.Surface.TwoSided = true;
                surfaceSettingLine      = surfaceSettingLine != 0 ? surfaceSettingLine : line;
            }
            else if ( lower == "blendmode" )
            {
                const std::string v = Lower( ReadIdent( c ) );
                if ( v == "opaque" )
                    result.Surface.Blend = SurfaceBlendMode::Opaque;
                else if ( v == "masked" )
                    result.Surface.Blend = SurfaceBlendMode::Masked;
                else if ( v == "translucent" )
                    result.Surface.Blend = SurfaceBlendMode::Translucent;
                else if ( v == "additive" )
                    result.Surface.Blend = SurfaceBlendMode::Additive;
                else
                {
                    err = { line, std::format( "unknown BlendMode '{}' (Opaque | Masked | Translucent | Additive)",
                                               v ) };
                    return fail();
                }
                surfaceSettingLine = surfaceSettingLine != 0 ? surfaceSettingLine : line;
            }
            else if ( lower == "responsiveaa" )
            {
                result.Surface.ResponsiveAA = true;
                surfaceSettingLine          = surfaceSettingLine != 0 ? surfaceSettingLine : line;
                responsiveLine              = line;
            }
            else if ( lower == "usage" )
            {
                // UE's material usage flags: which vertex factories the template is compiled for beyond the
                // meshes.
                const std::string stated = ReadIdent( c );
                if ( Lower( stated ) == "particlesprites" )
                    result.Surface.UsedWithParticleSprites = true;
                else if ( Lower( stated ) == "particleribbons" )
                    result.Surface.UsedWithParticleRibbons = true;
                else if ( Lower( stated ) == "grassblades" )
                    result.Surface.UsedWithGrassBlades = true;
                else
                {
                    // The author's spelling, not the folded one the comparison used.
                    err = { line,
                            std::format( "unknown Usage '{}' (ParticleSprites, ParticleRibbons, GrassBlades)",
                                         stated ) };
                    return fail();
                }
                surfaceSettingLine                     = surfaceSettingLine != 0 ? surfaceSettingLine : line;
            }
            else if ( lower == "shadingmodel" )
            {
                // The file stem of ShadingModels/<Name>.shadingmodel, as written; resolved once the Surface
                // block is known (a directive is legal only beside one).
                shadingModelName = ReadIdent( c );
                shadingModelLine = line;
                if ( shadingModelName.empty() )
                {
                    err = { line, "ShadingModel needs the model's name (a ShadingModels/<Name>.shadingmodel)" };
                    return fail();
                }
                surfaceSettingLine = surfaceSettingLine != 0 ? surfaceSettingLine : line;
            }
            // UE's OpacityMask input, STATED rather than written in the surface function:
            //   OpacityMask <Texture2D> Channel(<R|G|B|A | FloatParam>) [UV(<0|1>)]
            //               [Transform(<Tiling>, <Offset>, <Rotation>)]
            // The parser generates DesertSurfaceOpacityMask() from it and every cell clips with that, so the
            // shape the raster passes cut and the one a ray-traced reader evaluates from
            // ShaderProgramMeta::OpacityMask are one definition. Checked against the Properties once the file is
            // read.
            else if ( lower == "height" )
            {
                // `Height <Texture2D> [Channel(...)] [UV(n)] [Transform(T, O, R)] Scale(<Float>) [Center(<Float>)]`:
                // the surface's height (Formats::SurfaceHeight). Its texture's assignment selects the `.Parallax`
                // cells (kSurfaceParallaxCellSuffix); the parser generates the DesertSurfaceHeight*() reads.
                if ( heightLine != 0 )
                {
                    err = { line, "duplicate Height" };
                    return fail();
                }
                heightLine = line;
                Formats::SurfaceHeight height;
                height.Texture = ReadIdent( c );
                if ( height.Texture.empty() )
                {
                    err = { line, "Height needs the Texture2D parameter that holds the height map" };
                    return fail();
                }
                for ( ;; )
                {
                    const size_t      pos  = c.Pos;
                    const uint32_t    at   = c.Line;
                    const std::string attr = Lower( ReadIdent( c ) );
                    if ( attr == "channel" )
                    {
                        if ( !Expect( c, '(', err, "after Channel" ) )
                            return fail();
                        const std::string v      = ReadIdent( c );
                        const std::string folded = Lower( v );
                        if ( folded == "r" || folded == "g" || folded == "b" || folded == "a" )
                            height.Channel = folded == "r" ? 0u : folded == "g" ? 1u : folded == "b" ? 2u : 3u;
                        else if ( !v.empty() )
                            height.ChannelParam = v;
                        else
                        {
                            err = { line, "Height Channel takes R | G | B | A or a Float parameter" };
                            return fail();
                        }
                        if ( !Expect( c, ')', err, "after the Channel" ) )
                            return fail();
                    }
                    else if ( attr == "uv" )
                    {
                        float set = 0.0f;
                        if ( !Expect( c, '(', err, "after UV" ) || !ReadNumber( c, set, err ) ||
                             !Expect( c, ')', err, "after the UV set" ) )
                            return fail();
                        if ( set != 0.0f && set != 1.0f )
                        {
                            err = { line, "Height UV takes 0 or 1 (SurfaceInput.UV0 / UV1)" };
                            return fail();
                        }
                        height.UVSet = set == 1.0f ? 1u : 0u;
                    }
                    else if ( attr == "transform" )
                    {
                        if ( !Expect( c, '(', err, "after Transform" ) )
                            return fail();
                        height.TilingParam = ReadIdent( c );
                        if ( !Expect( c, ',', err, "after the tiling parameter" ) )
                            return fail();
                        height.OffsetParam = ReadIdent( c );
                        if ( !Expect( c, ',', err, "after the offset parameter" ) )
                            return fail();
                        height.RotationParam = ReadIdent( c );
                        if ( !Expect( c, ')', err, "after the rotation parameter" ) )
                            return fail();
                        if ( height.TilingParam.empty() || height.OffsetParam.empty() ||
                             height.RotationParam.empty() )
                        {
                            err = { line, "Height Transform names three parameters: (Tiling, Offset, Rotation)" };
                            return fail();
                        }
                    }
                    else if ( attr == "scale" || attr == "center" )
                    {
                        if ( !Expect( c, '(', err, attr == "scale" ? "after Scale" : "after Center" ) )
                            return fail();
                        ( attr == "scale" ? height.ScaleParam : height.CenterParam ) = ReadIdent( c );
                        if ( !Expect( c, ')', err, "after the parameter" ) )
                            return fail();
                    }
                    else
                    {
                        // Not this directive's word: the next directive starts here.
                        c.Pos  = pos;
                        c.Line = at;
                        break;
                    }
                }
                if ( height.ScaleParam.empty() )
                {
                    err = { line, "Height must state its Scale(<Float>) - the centimetres between height 0 and 1, "
                                  "never guessed" };
                    return fail();
                }
                result.Meta.Height = std::move( height );
                surfaceSettingLine = surfaceSettingLine != 0 ? surfaceSettingLine : line;
            }
            else if ( lower == "opacitymask" )
            {
                if ( opacityMaskLine != 0 )
                {
                    err = { line, "duplicate OpacityMask" };
                    return fail();
                }
                opacityMaskLine = line;
                Formats::SurfaceOpacityMask mask;
                mask.Texture    = ReadIdent( c );
                mask.ClipParam  = std::string( kSurfaceMaskClipParam );
                bool sawChannel = false;
                if ( mask.Texture.empty() )
                {
                    err = { line, "OpacityMask needs the Texture2D parameter that holds the mask" };
                    return fail();
                }
                for ( ;; )
                {
                    const size_t      pos  = c.Pos;
                    const uint32_t    at   = c.Line;
                    const std::string attr = Lower( ReadIdent( c ) );
                    if ( attr == "channel" )
                    {
                        if ( !Expect( c, '(', err, "after Channel" ) )
                            return fail();
                        const std::string v      = ReadIdent( c );
                        const std::string folded = Lower( v );
                        if ( folded == "r" || folded == "g" || folded == "b" || folded == "a" )
                            mask.Channel = folded == "r" ? 0u : folded == "g" ? 1u : folded == "b" ? 2u : 3u;
                        else if ( !v.empty() )
                            mask.ChannelParam = v;
                        else
                        {
                            err = { line, "OpacityMask Channel takes R | G | B | A or a Float parameter" };
                            return fail();
                        }
                        if ( !Expect( c, ')', err, "after the Channel" ) )
                            return fail();
                        sawChannel = true;
                    }
                    else if ( attr == "uv" )
                    {
                        float set = 0.0f;
                        if ( !Expect( c, '(', err, "after UV" ) || !ReadNumber( c, set, err ) ||
                             !Expect( c, ')', err, "after the UV set" ) )
                            return fail();
                        if ( set != 0.0f && set != 1.0f )
                        {
                            err = { line, "OpacityMask UV takes 0 or 1 (SurfaceInput.UV0 / UV1)" };
                            return fail();
                        }
                        mask.UVSet = set == 1.0f ? 1u : 0u;
                    }
                    else if ( attr == "transform" )
                    {
                        if ( !Expect( c, '(', err, "after Transform" ) )
                            return fail();
                        mask.TilingParam = ReadIdent( c );
                        if ( !Expect( c, ',', err, "after the tiling parameter" ) )
                            return fail();
                        mask.OffsetParam = ReadIdent( c );
                        if ( !Expect( c, ',', err, "after the offset parameter" ) )
                            return fail();
                        mask.RotationParam = ReadIdent( c );
                        if ( !Expect( c, ')', err, "after the rotation parameter" ) )
                            return fail();
                        if ( mask.TilingParam.empty() || mask.OffsetParam.empty() || mask.RotationParam.empty() )
                        {
                            err = { line,
                                    "OpacityMask Transform names three parameters: (Tiling, Offset, Rotation)" };
                            return fail();
                        }
                    }
                    else
                    {
                        // Not this directive's word: the next directive starts here.
                        c.Pos  = pos;
                        c.Line = at;
                        break;
                    }
                }
                if ( !sawChannel )
                {
                    err = { line,
                            "OpacityMask must state its Channel(R | G | B | A | FloatParam) - never guessed" };
                    return fail();
                }
                result.Meta.OpacityMask = std::move( mask );
                surfaceSettingLine      = surfaceSettingLine != 0 ? surfaceSettingLine : line;
            }
            else if ( lower == "pass" )
            {
                PendingPass pass;
                if ( !ReadQuoted( c, pass.Name, err ) || pass.Name.empty() )
                {
                    if ( err.Message.empty() )
                        err = { line, "Pass name must not be empty" };
                    return fail();
                }
                for ( const auto& existing : namedPasses )
                {
                    if ( existing.Name == pass.Name )
                    {
                        err = { line, "duplicate Pass \"" + pass.Name + "\"" };
                        return fail();
                    }
                }
                if ( !Expect( c, '{', err, "opening the Pass body" ) )
                    return fail();

                while ( true )
                {
                    SkipTrivia( c );
                    if ( c.Peek() == '}' )
                    {
                        c.Advance();
                        break;
                    }
                    if ( c.AtEnd() )
                    {
                        err = { c.Line, "unterminated Pass body (missing '}')" };
                        return fail();
                    }

                    const uint32_t    passLine    = c.Line;
                    const std::string passSection = ReadIdent( c );
                    const std::string passLower   = Lower( passSection );

                    if ( passLower == "state" )
                    {
                        if ( !ParseStateBlock( c, pass.State, err ) )
                            return fail();
                    }
                    else if ( const ShaderStage stage = StageFromKeyword( passSection );
                              stage != ShaderStage::None )
                    {
                        if ( !readStageBlock( c, pass, stage, passSection, passLine ) )
                            return fail();
                    }
                    else
                    {
                        err = { passLine,
                                "unknown Pass section '" + passSection + "' (expected State or a stage block)" };
                        return fail();
                    }
                }

                if ( pass.Blocks.empty() )
                {
                    err = { line, "Pass \"" + pass.Name + "\" defines no stage blocks" };
                    return fail();
                }
                namedPasses.push_back( std::move( pass ) );
            }
            else if ( const ShaderStage stage = StageFromKeyword( section ); stage != ShaderStage::None )
            {
                if ( !readStageBlock( c, defaultPass, stage, section, line ) )
                    return fail();
            }
            else
            {
                err = { line, "unknown section '" + section + "'" };
                return fail();
            }
        }

        if ( !CheckImportRows( importRows, result.Meta, err ) )
            return fail();

        // A MEDIUM-ONLY SHADER IS LEGAL AND IS THE ONE EXCEPTION. It has no stages because it is not a
        // program: it is the body compiled into four other programs. Every other file without a stage
        // block is still the error it always was — a shader that draws nothing.
        if ( surfaceBlock )
        {
            if ( result.Meta.Domain != ShaderDomain::Surface )
            {
                err = { surfaceLine, "a Surface block needs 'Domain Surface'" };
                return fail();
            }
            // A surface template's programs ARE its cells; a stage block beside them would be a program
            // no mesh pass ever selects.
            if ( !defaultPass.Blocks.empty() || !namedPasses.empty() )
            {
                err = { surfaceLine, "a Surface template must not also declare stage blocks or Pass blocks — "
                                     "its programs are its cells" };
                return fail();
            }
        }
        else if ( surfaceSettingLine != 0 )
        {
            err = { surfaceSettingLine, "TwoSided, BlendMode, OpacityMask, Height, ShadingModel, Usage and "
                                        "ResponsiveAA "
                                        "shape a Surface block, and this "
                                        "shader "
                                        "has none" };
            return fail();
        }

        // A GENERIC PROGRAM (a mesh-domain program without a Surface block) writes fragment stages only: its
        // vertex stage is the mesh vertex factory (kGenericVertexPath; assembled below by assembleGenericPass).
        const bool genericProgram = !surfaceBlock && Formats::DrawnByMeshPath( result.Meta.Domain );
        if ( genericProgram )
        {
            const auto refuseVertex = [&]( const PendingPass& pending )
            {
                const auto vertex = pending.Blocks.find( ShaderStage::Vertex );
                if ( vertex == pending.Blocks.end() )
                    return false;
                err = {
                     vertex->second.StartLine,
                     std::format( "a generic program's vertex stage is the mesh vertex factory ({}): delete the "
                                  "Vertex block and read v_Surface in the Fragment stage",
                                  SurfaceVertexInclude( kGenericVertexPath ) ) };
                return true;
            };
            if ( refuseVertex( defaultPass ) )
                return fail();
            for ( const auto& pass : namedPasses )
                if ( refuseVertex( pass ) )
                    return fail();
        }

        // A Particle fragment and its domain come together, and nothing else beside them: stages next to it
        // would be a program no simulation ever selects.
        if ( !result.Meta.ParticleSource.empty() )
        {
            if ( result.Meta.Domain != ShaderDomain::Particle )
            {
                err = { particleLine, "a Particle block needs 'Domain Particle'" };
                return fail();
            }
            if ( !defaultPass.Blocks.empty() || !namedPasses.empty() || !result.Meta.MediumSource.empty() )
            {
                err = { particleLine, "a Particle fragment must not also declare stage, Pass or Medium blocks — "
                                      "it is compiled into the simulation program" };
                return fail();
            }
        }
        else if ( result.Meta.Domain == ShaderDomain::Particle )
        {
            err = { c.Line, "'Domain Particle' needs a Particle { ... } block — the domain is a fragment, not "
                            "a program" };
            return fail();
        }

        if ( defaultPass.Blocks.empty() && namedPasses.empty() && !result.Meta.IsFragmentProgram() &&
             !surfaceBlock )
        {
            err = { c.Line, "shader defines no stage blocks (Vertex/Fragment/Compute...)" };
            return fail();
        }

        // THE SURFACE'S WEATHER RESPONSE (kSurfaceRespondsToWeatherParam, DShaderParser.hpp): appended to the row
        // of every lit Opaque / Masked template, after the authored parameters so no authored slot moves.
        const bool respondsToWeather = surfaceBlock && propInfo.UBBinding.has_value() &&
                                       !Formats::IsTranslucentBlend( result.Surface.Blend );
        if ( respondsToWeather )
        {
            for ( const std::string_view reserved : { kSurfaceRespondsToWeatherParam, kSurfacePorosityParam } )
                for ( const auto& p : result.Meta.Params )
                    if ( p.Name == reserved )
                    {
                        err = { c.Line,
                                std::format( "'{}' is the engine's weather-response parameter of every lit "
                                             "Surface template; rename the template's own",
                                             reserved ) };
                        return fail();
                    }
            const auto weatherParam = []( const std::string_view name, const std::string_view display,
                                          const std::string_view tooltip, const float value )
            {
                ShaderParam p;
                p.Name        = std::string( name );
                p.DisplayName = std::string( display );
                p.Category    = "Weather";
                p.Tooltip     = std::string( tooltip );
                p.Type        = ShaderValueType::Float;
                p.Widget      = ShaderParamWidget::Slider;
                p.Timing      = ShaderParamTiming::Immediate;
                p.Min         = 0.0f;
                p.Max         = 1.0f;
                p.Default     = glm::vec4( value, 0.0f, 0.0f, 0.0f );
                return p;
            };
            result.Meta.Params.push_back( weatherParam(
                 kSurfaceRespondsToWeatherParam, "Responds To Weather",
                 "How much the world's Wetness reaches this surface: 1 = fully, 0 = never (a covered or indoor "
                 "surface)",
                 1.0f ) );
            result.Meta.Params.push_back( weatherParam(
                 kSurfacePorosityParam, "Porosity",
                 "How much water the surface soaks up when wet: 1 = asphalt, soil, wood (darker, glossier), 0 = "
                 "glazed tile, plastic, glass (unchanged until water stands on it). Metal never darkens",
                 0.5f ) );
        }

        result.Meta.LayoutBindings  = MaterialLayoutBindings{ propInfo.UBBinding, propInfo.TextureBinding };
        result.Layout               = BuildMaterialLayout( result.Meta );
        // The reserved scene-read range (Core::kSceneReadBindingFirst, ShaderGraphBindings.hpp) is declared into
        // every view-pass cell by Common/GpuScene.glslh; a material row or texture numbered into it would
        // share a descriptor slot with the view's motion rows, which GLSL accepts without a word.
        if ( result.Layout.RowBinding && Core::IsSceneReadBinding( *result.Layout.RowBinding ) )
        {
            err = { c.Line, std::format( "Properties Binding({}) is inside the reserved scene-read range [{}, {}) "
                                         "(GpuScenePrimitives / ObjectBones); number the material row elsewhere",
                                         *result.Layout.RowBinding, Core::kSceneReadBindingFirst,
                                         Core::kSceneReadBindingFirst + Core::kSceneReadBindingCount ) };
            return fail();
        }
        for ( const auto& texture : result.Layout.Textures )
            for ( const uint32_t binding : { texture.Binding, texture.SamplerBinding } )
                if ( Core::IsSceneReadBinding( binding ) )
                {
                    err = { c.Line,
                            std::format(
                                 "texture property '{}' (or its sampler state) lands on binding {}, inside "
                                 "the reserved scene-read range [{}, {}) (GpuScenePrimitives / ObjectBones); move "
                                 "TextureBinding() so the run of textures and their samplers stays clear of it",
                                 texture.Name, binding, Core::kSceneReadBindingFirst,
                                 Core::kSceneReadBindingFirst + Core::kSceneReadBindingCount ) };
                    return fail();
                }
        const std::string autoDecls = BuildAutoDeclarations( result.Layout );
        // The hit cell's: the same row, named through the hit group's shader record.
        const std::string rayHitAutoDecls = BuildAutoDeclarations( result.Layout, true );

        const auto assemblePass = [&]( const PendingPass& pending, const ShaderRenderState& state )
        {
            DShaderPass out;
            out.Name  = pending.Name;
            out.State = state;
            for ( const auto& [stage, block] : pending.Blocks )
                out.Stages.emplace( stage, AssembleStage( stage, block, includeBlock, autoDecls ) );
            return out;
        };

        // A generic program's pass (kGenericVertexPath): the vertex stage is the mesh vertex factory, the author's
        // fragment reads its varyings. @p binned assembles the `.Binned` twin the GPU-driven bins draw: the vertex
        // stage resolves the draw from its instance id, the fragment's `Material` reads the row it handed down (a
        // macro, expanded in the author's code after DESERT_SURFACE_VARYINGS declared v_Surface). A pass without a
        // Fragment stage (a ray tracing pass) is assembled as written.
        const auto assembleGenericPass = [&]( const PendingPass& pending, const ShaderRenderState& state,
                                              const std::string& name, const bool binned )
        {
            const auto fragment = pending.Blocks.find( ShaderStage::Fragment );
            if ( fragment == pending.Blocks.end() )
            {
                DShaderPass out = assemblePass( pending, state );
                out.Name        = name;
                return out;
            }
            const std::string_view kind =
                 pending.Name == kGenericDepthNormalPass ? kGenericDepthNormalPass : kGenericForwardPassKind;
            const std::string defines =
                 std::format( "#define DESERT_SURFACE_PASS_{} 1\n#define {} 1\n{}", kind, kGenericProgramDefine,
                              binned ? std::format( "#define {} 1\n", kSurfaceBinnedDefine ) : std::string() );
            DShaderPass out;
            out.Name  = name;
            out.State = state;
            for ( const auto& [stage, block] : pending.Blocks )
            {
                if ( stage == ShaderStage::Fragment )
                    continue;
                out.Stages.emplace( stage, AssembleStage( stage, block, includeBlock, autoDecls ) );
            }
            RawBlock vertex;
            vertex.StartLine = 1;
            vertex.Content   = std::format( "{}#include <{}>\n#include <{}>\n", defines, kSurfaceTypesInclude,
                                            SurfaceVertexInclude( kGenericVertexPath ) );
            out.Stages.emplace( ShaderStage::Vertex,
                                AssembleStage( ShaderStage::Vertex, vertex, includeBlock, {} ) );
            RawBlock fragmentCode;
            fragmentCode.StartLine = 1;
            fragmentCode.Content   = std::format(
                 "{}#include <{}>\nDESERT_SURFACE_VARYINGS( in )\n#line {}\n{}\n", defines, kSurfaceTypesInclude,
                 fragment->second.StartLine > 0 ? fragment->second.StartLine - 1 : 0, fragment->second.Content );
            const std::string fragmentDecls = binned ? std::format( "#define {} v_Surface.MaterialRow\n{}",
                                                                    kSurfaceMaterialRowMacro, autoDecls )
                                                     : autoDecls;
            out.Stages.emplace( ShaderStage::Fragment, AssembleStage( ShaderStage::Fragment, fragmentCode,
                                                                      includeBlock, fragmentDecls ) );
            return out;
        };

        if ( surfaceBlock )
        {
            const bool masked = result.Surface.Blend == SurfaceBlendMode::Masked;
            // THE MODEL, from the one loaded set (ShaderRootShadingModels): by name when the template names one,
            // DefaultLit when it does not. No fallback: an unknown name or a set that failed to load refuses.
            const auto  held   = ShadingModels::ShaderRootShadingModels();
            const auto& models = *held;
            if ( !models.IsSuccess() )
            {
                err = { shadingModelLine != 0 ? shadingModelLine : surfaceBlock->StartLine,
                        std::format( "the shading models did not load: {}", models.GetError() ) };
                return fail();
            }
            const ShadingModels::ShadingModelRegistry& registry = models.GetValue().Registry;
            const ShadingModels::ShadingModelEntry*    model =
                 shadingModelName.empty() ? registry.FindByGuid( Common::UUID( ShadingModels::kDefaultLitGuid ) )
                                             : registry.FindByName( shadingModelName );
            if ( model == nullptr )
            {
                std::string known;
                for ( const ShadingModels::ShadingModelEntry& e : registry.Entries() )
                    known.append( known.empty() ? "" : " | " ).append( e.Manifest.Name );
                err = { shadingModelLine,
                        std::format( "unknown ShadingModel '{}' ({})", shadingModelName, known ) };
                return fail();
            }
            // A surface built from DefaultSurfaceOutput() has every field (UE: an unconnected pin takes its
            // default); one declared bare has only the fields it assigns.
            const bool fromDefaults = surfaceBlock->Content.find( "DefaultSurfaceOutput" ) != std::string::npos;
            const std::vector<std::string> written = SurfaceWrittenFields( surfaceBlock->Content );
            if ( const auto missing = fromDefaults
                                           ? std::nullopt
                                           : ShadingModels::ShadingModelRegistry::MissingInput( *model, written ) )
            {
                err = { shadingModelLine != 0 ? shadingModelLine : surfaceBlock->StartLine,
                        std::format( "ShadingModel {} reads SurfaceOutput.{}, which the surface function never "
                                     "assigns ({})",
                                     model->Manifest.Name, *missing,
                                     model->Manifest.SourcePath.generic_string() ) };
                return fail();
            }
            result.Surface.ShadingModel          = model->Manifest.Guid;
            const std::string shadingModelDefine = ShadingModels::ShadingModelIndexDefine( model->Manifest.Name );
            result.Meta.Blend = result.Surface.Blend;
            if ( result.Surface.ResponsiveAA && !Formats::IsTranslucentBlend( result.Surface.Blend ) )
            {
                err = { responsiveLine,
                        "ResponsiveAA marks translucency that writes no velocity of its own (BlendMode Translucent "
                        "or Additive); an opaque or masked surface writes the velocity TAA reprojects it with" };
                return fail();
            }
            if ( masked )
            {
                const auto clip =
                     std::find_if( result.Meta.Params.begin(), result.Meta.Params.end(), []( const ShaderParam& p )
                                   { return p.Name == kSurfaceMaskClipParam && !p.IsTexture; } );
                if ( clip == result.Meta.Params.end() || clip->Type != ShaderValueType::Float ||
                     !result.Layout.RowBinding )
                {
                    err = { surfaceSettingLine,
                            std::format( "BlendMode Masked needs a Float '{}' in a 'Properties Binding(n)' block "
                                         "— the clip threshold is a material parameter",
                                         kSurfaceMaskClipParam ) };
                    return fail();
                }
                if ( !result.Meta.OpacityMask )
                {
                    err = {
                         surfaceSettingLine,
                         "BlendMode Masked needs an `OpacityMask <Texture2D> Channel(...)` line - the mask the "
                         "cells clip with and ray tracing reads is stated, not written in the surface function" };
                    return fail();
                }
                if ( const std::string bad = OpacityMaskParamError( *result.Meta.OpacityMask, result.Meta.Params );
                     !bad.empty() )
                {
                    err = { opacityMaskLine, bad };
                    return fail();
                }
                const auto dither =
                     std::find_if( result.Meta.Params.begin(), result.Meta.Params.end(),
                                   []( const ShaderParam& p ) { return p.Name == kSurfaceMaskDitherParam; } );
                if ( dither != result.Meta.Params.end() )
                {
                    if ( dither->IsTexture || dither->Type != ShaderValueType::Float )
                    {
                        err = { opacityMaskLine,
                                std::format( "'{}' (the dithered opacity mask switch) must be a Float",
                                             kSurfaceMaskDitherParam ) };
                        return fail();
                    }
                    result.Meta.OpacityMask->DitherParam = std::string( kSurfaceMaskDitherParam );
                }
            }
            else if ( result.Meta.OpacityMask )
            {
                err = { opacityMaskLine,
                        "OpacityMask is the input of BlendMode Masked; this template is not Masked" };
                return fail();
            }
            if ( result.Meta.Height )
            {
                if ( const std::string bad = HeightParamError( *result.Meta.Height, result.Meta.Params ); !bad.empty() )
                {
                    err = { heightLine, bad };
                    return fail();
                }
                // UE draws a translucent material with no Pixel Depth Offset: it writes no depth to offset, and a
                // relief self-shadow over a refracted background would be a second, different shading.
                if ( Formats::IsTranslucentBlend( result.Surface.Blend ) )
                {
                    err = { heightLine, "Height's parallax writes the relief's depth (Pixel Depth Offset); a "
                                        "translucent template writes no depth - Height is for Opaque and Masked "
                                        "templates" };
                    return fail();
                }
            }

            const std::string opacityMaskFunction =
                 result.Meta.OpacityMask ? OpacityMaskFunction( *result.Meta.OpacityMask ) : std::string();
            const std::string heightFunctions =
                 result.Meta.Height ? HeightFunctions( *result.Meta.Height ) : std::string();

            ShaderRenderState cellOverride;
            if ( result.Surface.TwoSided )
                cellOverride.Cull = StateCull::None;
            const ShaderRenderState cellState = MergeState( defaultPass.State, cellOverride );

            for ( const std::string_view path : kSurfaceVertexPaths )
                for ( const std::string_view pass : kSurfaceCellPasses )
                {
                    if ( !SurfaceBlendHasPass( result.Surface.Blend, pass ) )
                        continue;
                    DShaderPass cell;
                    cell.Name  = SurfaceCellName( path, pass );
                    cell.State = cellState;
                    for ( const ShaderStage stage : { ShaderStage::Vertex, ShaderStage::Fragment } )
                        cell.Stages.emplace(
                             stage, AssembleSurfaceCellStage( stage, path, pass, *surfaceBlock, includeBlock,
                                                              autoDecls, result.Surface.Blend, shadingModelDefine,
                                                              result.Surface.ResponsiveAA, opacityMaskFunction,
                                                              false, heightFunctions, respondsToWeather ) );
                    result.Meta.PassNames.push_back( cell.Name );
                    result.Surface.Cells.push_back( cell.Name );
                    result.Passes.push_back( std::move( cell ) );
                }

            // THE TRANSLUCENT FRONT LAYER CELL (kSurfaceFrontLayerPass): a Translucent template's surface into the
            // front layer's G-buffer, after its Forward cells (their order is unchanged).
            if ( result.Surface.Blend == SurfaceBlendMode::Translucent )
            {
                DShaderPass cell;
                cell.Name  = SurfaceCellName( kSurfaceFrontLayerPath, kSurfaceFrontLayerPass );
                cell.State = cellState;
                for ( const ShaderStage stage : { ShaderStage::Vertex, ShaderStage::Fragment } )
                    cell.Stages.emplace(
                         stage, AssembleSurfaceCellStage( stage, kSurfaceFrontLayerPath, kSurfaceFrontLayerPass,
                                                          *surfaceBlock, includeBlock, autoDecls,
                                                          result.Surface.Blend, shadingModelDefine,
                                                          result.Surface.ResponsiveAA, opacityMaskFunction,
                                                          false, heightFunctions, false ) );
                result.Meta.PassNames.push_back( cell.Name );
                result.Surface.Cells.push_back( cell.Name );
                result.Passes.push_back( std::move( cell ) );
            }

            // THE RAY TRACING HIT CELL (UE: the material's ray tracing hit group, FRayTracingMeshCommand's
            // closest-hit / any-hit shaders), after the mesh cells: the same surface function between the shared
            // types and Mesh/Surface/Pass_RayHit.glslh. Closest hit evaluates the surface into the ray's payload;
            // a Masked template adds the any-hit stage, its OpacityMask test. A Translucent template's closest hit
            // also writes the payload's transmission and IOR (Pass_RayHit.glslh under DESERT_SURFACE_TRANSLUCENT):
            // its primitives carry the translucent instance mask alone (RayTracingScene.hpp kMaskTranslucent), so
            // only the path tracer's rays meet them (UE: the path tracer's translucent hit shaders). Additive has
            // no transmission to give and no cell. A template that binds a texture outside its row has nothing to
            // put in a shader record: no cell, and the reason is kept for the pipeline that asks for it.
            if ( result.Surface.Blend != SurfaceBlendMode::Additive )
            {
                if ( !result.Layout.Textures.empty() )
                    result.Surface.RayHitRefusal = std::format(
                         "texture '{}' is bound outside the material row; a hit group reads its material from the "
                         "shader record, which carries the row alone",
                         result.Layout.Textures.front().Name );
                else
                {
                    DShaderPass cell;
                    cell.Name         = SurfaceCellName( kSurfaceRayHitPath, kSurfaceRayHitPass );
                    cell.State        = cellState;
                    const bool masked = result.Surface.Blend == SurfaceBlendMode::Masked;
                    for ( const ShaderStage stage : { ShaderStage::ClosestHit, ShaderStage::AnyHit } )
                    {
                        if ( stage == ShaderStage::AnyHit && !masked )
                            continue;
                        cell.Stages.emplace(
                             stage, AssembleSurfaceCellStage( stage, kSurfaceRayHitPath, kSurfaceRayHitPass,
                                                              *surfaceBlock, includeBlock, rayHitAutoDecls,
                                                              result.Surface.Blend, shadingModelDefine,
                                                              result.Surface.ResponsiveAA, opacityMaskFunction,
                                                              false, heightFunctions, false ) );
                    }
                    result.Meta.PassNames.push_back( cell.Name );
                    result.Meta.RayTracingPassNames.push_back( cell.Name );
                    result.Surface.Cells.push_back( cell.Name );
                    result.Surface.RayHitCell = cell.Name;
                    result.Passes.push_back( std::move( cell ) );
                }
            }

            // `Usage ParticleSprites`: the sprite cell, after every mesh cell (the mesh cells' order is
            // unchanged).
            if ( result.Surface.UsedWithParticleSprites )
            {
                DShaderPass cell;
                cell.Name  = SurfaceCellName( kSurfaceParticleSpritePath, kSurfaceParticleSpritePass );
                cell.State = cellState;
                for ( const ShaderStage stage : { ShaderStage::Vertex, ShaderStage::Fragment } )
                    cell.Stages.emplace( stage, AssembleSurfaceCellStage(
                                                     stage, kSurfaceParticleSpritePath, kSurfaceParticleSpritePass,
                                                     *surfaceBlock, includeBlock, autoDecls, result.Surface.Blend,
                                                     shadingModelDefine, result.Surface.ResponsiveAA,
                                                     opacityMaskFunction, false, heightFunctions, false ) );
                result.Meta.PassNames.push_back( cell.Name );
                result.Surface.Cells.push_back( cell.Name );
                result.Passes.push_back( std::move( cell ) );
            }

            // `Usage ParticleRibbons`: the ribbon cell, after the sprite cell (same pass, same particle header).
            if ( result.Surface.UsedWithParticleRibbons )
            {
                DShaderPass cell;
                cell.Name  = SurfaceCellName( kSurfaceParticleRibbonPath, kSurfaceParticleSpritePass );
                cell.State = cellState;
                for ( const ShaderStage stage : { ShaderStage::Vertex, ShaderStage::Fragment } )
                    cell.Stages.emplace( stage, AssembleSurfaceCellStage(
                                                     stage, kSurfaceParticleRibbonPath, kSurfaceParticleSpritePass,
                                                     *surfaceBlock, includeBlock, autoDecls, result.Surface.Blend,
                                                     shadingModelDefine, result.Surface.ResponsiveAA,
                                                     opacityMaskFunction, false, heightFunctions, false ) );
                result.Meta.PassNames.push_back( cell.Name );
                result.Surface.Cells.push_back( cell.Name );
                result.Passes.push_back( std::move( cell ) );
            }

            // `Usage GrassBlades`: the blade cells, last, in the mesh passes' order.
            if ( result.Surface.UsedWithGrassBlades )
                for ( const std::string_view pass : kSurfaceCellPasses )
                {
                    if ( !SurfaceBlendHasPass( result.Surface.Blend, pass ) )
                        continue;
                    DShaderPass cell;
                    cell.Name  = SurfaceCellName( kSurfaceGrassBladePath, pass );
                    cell.State = cellState;
                    for ( const ShaderStage stage : { ShaderStage::Vertex, ShaderStage::Fragment } )
                        cell.Stages.emplace(
                             stage, AssembleSurfaceCellStage( stage, kSurfaceGrassBladePath, pass, *surfaceBlock,
                                                              includeBlock, autoDecls, result.Surface.Blend,
                                                              shadingModelDefine, result.Surface.ResponsiveAA,
                                                              opacityMaskFunction, false, heightFunctions,
                                                              false ) );
                    result.Meta.PassNames.push_back( cell.Name );
                    result.Surface.Cells.push_back( cell.Name );
                    result.Passes.push_back( std::move( cell ) );
                }

            // `Height <Texture2D> ... Scale(<Float>)`: every mesh cell again as its `.Parallax` permutation, after every other cell
            // (the base cells' order is unchanged). The ray tracing hit and the sprite have no screen derivatives
            // to size a march with (or no depth to offset) and stay single.
            if ( result.Meta.Height )
                for ( const std::string_view path : kSurfaceVertexPaths )
                    for ( const std::string_view pass : kSurfaceCellPasses )
                    {
                        if ( !SurfaceBlendHasPass( result.Surface.Blend, pass ) )
                            continue;
                        DShaderPass cell;
                        cell.Name  = SurfaceCellName( path, pass ) + std::string( kSurfaceParallaxCellSuffix );
                        cell.State = cellState;
                        for ( const ShaderStage stage : { ShaderStage::Vertex, ShaderStage::Fragment } )
                            cell.Stages.emplace(
                                 stage, AssembleSurfaceCellStage( stage, path, pass, *surfaceBlock, includeBlock,
                                                                  autoDecls, result.Surface.Blend,
                                                                  shadingModelDefine, result.Surface.ResponsiveAA,
                                                                  opacityMaskFunction, true, heightFunctions,
                                                                  respondsToWeather ) );
                        result.Meta.PassNames.push_back( cell.Name );
                        result.Surface.Cells.push_back( cell.Name );
                        result.Passes.push_back( std::move( cell ) );
                    }

            // THE BINNED PERMUTATION (kSurfaceBinnedCellSuffix): every mesh cell of a binned pass again, the
            // `.Parallax` ones too, after every other cell (the earlier cells' order is unchanged). A translucent
            // template has none: its Forward cell is the translucency pass's, which is sorted, not binned.
            if ( !Formats::IsTranslucentBlend( result.Surface.Blend ) )
                for ( const bool parallax : { false, true } )
                {
                    if ( parallax && !result.Meta.Height )
                        continue;
                    for ( const std::string_view path : kSurfaceVertexPaths )
                        for ( const std::string_view pass : kSurfaceBinnedCellPasses )
                        {
                            DShaderPass cell;
                            cell.Name = SurfaceCellName( path, pass ) +
                                        ( parallax ? std::string( kSurfaceParallaxCellSuffix ) : std::string() ) +
                                        std::string( kSurfaceBinnedCellSuffix );
                            cell.State = cellState;
                            for ( const ShaderStage stage : { ShaderStage::Vertex, ShaderStage::Fragment } )
                                cell.Stages.emplace( stage,
                                                     AssembleSurfaceCellStage(
                                                          stage, path, pass, *surfaceBlock, includeBlock,
                                                          autoDecls, result.Surface.Blend, shadingModelDefine,
                                                          result.Surface.ResponsiveAA, opacityMaskFunction,
                                                          parallax, heightFunctions, respondsToWeather, true ) );
                            result.Meta.PassNames.push_back( cell.Name );
                            result.Surface.Cells.push_back( cell.Name );
                            result.Passes.push_back( std::move( cell ) );
                        }
                }
        }

        // The default program is the top-level stage set; when a shader consists solely of
        // named passes, the first pass IS the default program (ShaderProgramMeta::DefaultPassName, below).
        if ( !defaultPass.Blocks.empty() )
            result.Passes.push_back(
                 genericProgram ? assembleGenericPass( defaultPass, defaultPass.State, defaultPass.Name, false )
                                : assemblePass( defaultPass, defaultPass.State ) );

        for ( const auto& pass : namedPasses )
        {
            const ShaderRenderState state = MergeState( defaultPass.State, pass.State );
            result.Passes.push_back( genericProgram ? assembleGenericPass( pass, state, pass.Name, false )
                                                    : assemblePass( pass, state ) );
            result.Meta.PassNames.push_back( pass.Name );
            if ( std::any_of( result.Passes.back().Stages.begin(), result.Passes.back().Stages.end(),
                              []( const auto& stage ) { return Formats::IsRayTracingStage( stage.first ); } ) )
                result.Meta.RayTracingPassNames.push_back( pass.Name );
        }

        // An OPAQUE mesh-path template without a Surface block is drawn forward on the deferred path, after the
        // screen-space readers of the G-buffer (the ray-traced sun's mask, SSAO, SSR); they see it only through
        // its depth-normal pass, so it must declare one (no silent hole in their depth). A blended one (text,
        // ZWrite off) writes no depth and is exempt, as UE's translucency is from the depth prepass.
        if ( result.Surface.Cells.empty() && Formats::DrawnByMeshPath( result.Meta.Domain ) &&
             !defaultPass.State.Blend.value_or( false ) && defaultPass.State.DepthWrite.value_or( true ) &&
             result.FindPass( std::string( kGenericDepthNormalPass ) ) == nullptr )
            return Common::MakeFormattedError<DShaderParseResult>(
                 "DShader '{}': an opaque Domain {} program without a Surface block declares no '{}' pass - the "
                 "deferred path's depth/normal prepass draws it (write depth and DesertWriteDepthNormal, "
                 "Mesh/Generic/DepthNormal.glslh)",
                 result.Name, Formats::ShaderDomainName( result.Meta.Domain ), kGenericDepthNormalPass );

        // THE BINNED TWINS of an opaque generic program (kGenericForwardBinnedPass,
        // kGenericDepthNormalBinnedPass), after every declared pass so the default program and the pass order are
        // unchanged. The forward twin is the default program's (the top-level stage set, or the first named pass
        // that is not the depth-normal one).
        if ( genericProgram && !defaultPass.State.Blend.value_or( false ) &&
             defaultPass.State.DepthWrite.value_or( true ) )
        {
            const PendingPass* forward     = defaultPass.Blocks.empty() ? nullptr : &defaultPass;
            const PendingPass* depthNormal = nullptr;
            for ( const auto& pass : namedPasses )
            {
                if ( pass.Name == kGenericDepthNormalPass )
                    depthNormal = &pass;
                else if ( forward == nullptr )
                    forward = &pass;
            }
            const auto twin = [&]( const PendingPass& pending, const std::string_view name )
            {
                const ShaderRenderState state =
                     &pending == &defaultPass ? defaultPass.State : MergeState( defaultPass.State, pending.State );
                result.Passes.push_back( assembleGenericPass( pending, state, std::string( name ), true ) );
                result.Meta.PassNames.emplace_back( name );
            };
            if ( forward != nullptr )
                twin( *forward, kGenericForwardBinnedPass );
            if ( depthNormal != nullptr )
                twin( *depthNormal, kGenericDepthNormalBinnedPass );
        }

        // A surface template's default program is NAMED, never positional: kSurfaceDefaultCell. A template
        // whose cell set lost it is refused here by name, rather than having some other cell stand in for it.
        if ( !result.Surface.Cells.empty() )
        {
            const DShaderPass* defaultCell = result.FindPass( std::string( kSurfaceDefaultCell ) );
            if ( defaultCell == nullptr )
                return Common::MakeFormattedError<DShaderParseResult>(
                     "DShader '{}': surface template has no default cell '{}'", result.Name, kSurfaceDefaultCell );
            result.Meta.State           = defaultCell->State;
            result.Stages               = defaultCell->Stages;
            result.Meta.DefaultPassName = std::string( kSurfaceDefaultCell );
        }
        // Guarded, because a medium-only shader has no passes at all and `front()` on an empty vector is
        // the kind of crash that reads as a corrupt file rather than as a missing branch.
        else if ( !result.Passes.empty() )
        {
            result.Meta.State = result.Passes.front().State;
            result.Stages     = result.Passes.front().Stages;
            // No top-level stage set: the default program IS the first named pass, one program under two names.
            if ( defaultPass.Blocks.empty() )
                result.Meta.DefaultPassName = result.Passes.front().Name;
        }

        return Common::MakeSuccess( std::move( result ) );
    }

} // namespace Desert::Core::Preprocess
