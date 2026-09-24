#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>

#include <array>
#include <cctype>
#include <charconv>
#include <functional>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>

namespace Desert::Core::Preprocess
{
    using namespace Desert::Core::Formats;

    namespace
    {
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

        // GLSL declaration type for an auto-generated material-parameter field.
        const char* GlslTypeOf( const ShaderParam& p )
        {
            switch ( p.Type )
            {
                case ShaderValueType::Float:
                    return "float";
                case ShaderValueType::Float2:
                    return "vec2";
                case ShaderValueType::Float3:
                    return "vec3";
                case ShaderValueType::Float4:
                    return "vec4";
                case ShaderValueType::Int:
                    return "int";
                case ShaderValueType::Bool:
                    return "int";
                default:
                    return "vec4";
            }
        }

        // How many floats of tail padding a parameter needs to fill its whole 16-byte slot. See
        // Core/Formats/MaterialParamRow.hpp for why every parameter owns a slot instead of being packed:
        // it makes "parameter i is at 16*i" true by construction, so the C++ packer implements no layout
        // rules and cannot disagree with the GLSL about one.
        uint32_t GlslSlotPaddingFloats( const ShaderParam& p )
        {
            switch ( p.Type )
            {
                case ShaderValueType::Float4:
                    return 0;
                case ShaderValueType::Float3:
                    return 1;
                case ShaderValueType::Float2:
                    return 2;
                default: // float, int, bool — 4 bytes of value, 12 of slot
                    return 3;
            }
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
            // ( "Display Name" [, Range(a,b)] [, Category("...")] [, Tooltip("...")] [, Timing(Immediate)] )
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
                else
                {
                    err = { c.Line, "unknown property attribute '" + attr +
                                         "' (expected Range, Category, Tooltip or Timing)" };
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
                    err = { line, "unknown default texture '" + name + "' for property '" + param.Name +
                                       "' (expected one of " + DefaultTextureKindList() + ")" };
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
                    return true;
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
                if ( param.IsAssetRef() && ( info.UBBinding || info.TextureBinding ) )
                {
                    err = { entryLine, "asset-reference property '" + param.Name + "' (" + param.AssetKind +
                                            ") is CPU-side only and cannot appear in a Properties block "
                                            "that declares Binding()/TextureBinding()" };
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
        // shader's source had to change for the swap: both spellings answer to `u_Material.<Name>`, and
        // the difference is only whether that name resolves to a block or to a row.
        //
        // The push constant carrying the row index is an INCLUDE and not text emitted here, because it is
        // identical in every shader in the engine and the offsets have to stay identical too — the same
        // reason the generator emits structure and lets `.glslh` files carry boilerplate.
        std::string BuildAutoDeclarations( const ShaderProgramMeta& meta, const PropertiesInfo& info )
        {
            std::ostringstream out;

            if ( info.UBBinding )
            {
                bool any = false;
                for ( const auto& p : meta.Params )
                    if ( !p.IsTexture && !p.IsAssetRef() )
                        any = true;

                if ( any )
                {
                    out << "#include <" << kParserInjectedIncludes[0] << ">\n";
                    out << "struct MaterialParams\n{\n";
                    uint32_t slot = 0;
                    for ( const auto& p : meta.Params )
                    {
                        if ( p.IsTexture )
                            continue;
                        out << "    " << GlslTypeOf( p ) << " " << p.Name << ";\n";
                        // The pad is a float ARRAY (std430 stride 4), so the value plus its pad is
                        // exactly one 16-byte slot whatever the value's own type is.
                        if ( const uint32_t pad = GlslSlotPaddingFloats( p ); pad > 0 )
                            out << "    float _slotPad" << slot << "[" << pad << "];\n";
                        ++slot;
                    }
                    out << "};\n";
                    out << "layout( std430, binding = " << *info.UBBinding << " ) readonly buffer Materials\n"
                        << "{\n    MaterialParams u_Materials[];\n};\n";
                    out << "#define u_Material u_Materials[m_PushConstants.MaterialIndex]\n";
                }
            }

            if ( info.TextureBinding )
            {
                uint32_t binding = *info.TextureBinding;
                for ( const auto& p : meta.Params )
                {
                    if ( !p.IsTexture )
                        continue;
                    out << "layout( binding = " << binding++ << " ) uniform "
                        << ( p.IsCubeTexture ? "samplerCube" : "sampler2D" ) << " " << p.Name << ";\n";
                }
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

        KeywordSet PresentSugarKeywords( const std::vector<SourceRun>& runs )
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
                 { KwLocalSize, std::regex( R"(\bLocalSize\s*\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\))" ),
                   "layout(local_size_x = $1, local_size_y = $2, local_size_z = $3) in" },
                 { KwPushConstant, std::regex( R"(\bPushConstant\b)" ), "layout(push_constant) uniform" },
            };

            const KeywordSet explicitForms = PresentSugarKeywords( runs );
            for ( auto& run : runs )
            {
                if ( !run.IsCode )
                    continue;
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

            std::string out;
            for ( auto& run : runs )
            {
                if ( run.IsCode )
                {
                    if ( autoIn )
                        replaceFirst( run.Text, kAutoIn,
                                      [&]( const std::smatch& ) {
                                          return "layout(location = " + std::to_string( alloc( usedIn ) ) + ") in";
                                      } );
                    if ( autoOut )
                        replaceFirst(
                             run.Text, kAutoOut, [&]( const std::smatch& )
                             { return "layout(location = " + std::to_string( alloc( usedOut ) ) + ") out"; } );
                    if ( autoBind )
                        replaceFirst( run.Text, kAutoBinding,
                                      [&]( const std::smatch& m )
                                      {
                                          const int         n  = alloc( usedBind );
                                          const std::string kw = m[1].str();
                                          if ( kw == "Uniform" )
                                              return "layout(binding = " + std::to_string( n ) + ") uniform";
                                          if ( kw == "ReadBuffer" )
                                              return "layout(std430, binding = " + std::to_string( n ) +
                                                     ") readonly buffer";
                                          if ( kw == "WriteBuffer" )
                                              return "layout(std430, binding = " + std::to_string( n ) +
                                                     ") writeonly buffer";
                                          return "layout(std430, binding = " + std::to_string( n ) + ") buffer";
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
            out << "#version 450\n";

            if ( !autoDecls.empty() && ( stage == ShaderStage::Fragment || stage == ShaderStage::Compute ) )
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

    std::string DShaderParser::TranslateSugar( const std::string& source )
    {
        return TranslateLayoutSugar( source );
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
        RawBlock                 includeBlock;
        PendingPass              defaultPass; // top-level stage blocks
        std::vector<PendingPass> namedPasses;

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
                else
                {
                    err = { line, "unknown Domain '" + v + "'" };
                    return fail();
                }
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

        // A MEDIUM-ONLY SHADER IS LEGAL AND IS THE ONE EXCEPTION. It has no stages because it is not a
        // program: it is the body compiled into four other programs. Every other file without a stage
        // block is still the error it always was — a shader that draws nothing.
        if ( defaultPass.Blocks.empty() && namedPasses.empty() && !result.Meta.IsMediumProgram() )
        {
            err = { c.Line, "shader defines no stage blocks (Vertex/Fragment/Compute...)" };
            return fail();
        }

        const std::string autoDecls = BuildAutoDeclarations( result.Meta, propInfo );

        const auto assemblePass = [&]( const PendingPass& pending, const ShaderRenderState& state )
        {
            DShaderPass out;
            out.Name  = pending.Name;
            out.State = state;
            for ( const auto& [stage, block] : pending.Blocks )
                out.Stages.emplace( stage, AssembleStage( stage, block, includeBlock, autoDecls ) );
            return out;
        };

        // The default program is the top-level stage set; when a shader consists solely of
        // named passes, the first pass doubles as the default program.
        if ( !defaultPass.Blocks.empty() )
            result.Passes.push_back( assemblePass( defaultPass, defaultPass.State ) );

        for ( const auto& pass : namedPasses )
        {
            result.Passes.push_back( assemblePass( pass, MergeState( defaultPass.State, pass.State ) ) );
            result.Meta.PassNames.push_back( pass.Name );
        }

        // Guarded, because a medium-only shader has no passes at all and `front()` on an empty vector is
        // the kind of crash that reads as a corrupt file rather than as a missing branch.
        if ( !result.Passes.empty() )
        {
            result.Meta.State = result.Passes.front().State;
            result.Stages     = result.Passes.front().Stages;
        }

        return Common::MakeSuccess( std::move( result ) );
    }

} // namespace Desert::Core::Preprocess
