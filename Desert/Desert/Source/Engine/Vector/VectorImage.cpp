#include "VectorImage.hpp"

#include <Engine/Core/Formats/SdfAtlasEncoding.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace Desert::Vector
{
    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;

        // --- tiny XML scanner -------------------------------------------------------------------------
        // Icons are machine-generated XML, so a full parser is not needed: walk tag by tag, pulling
        // attributes out of the raw text. Unknown tags/attributes are skipped rather than rejected.

        struct Attr
        {
            std::string Name, Value;
        };

        struct Tag
        {
            std::string       Name;
            std::vector<Attr> Attrs;
            bool              Closing     = false; // </g>
            bool              SelfClosing = false; // <rect ... />
        };

        bool IsSpace( char c )
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r';
        }

        const std::string* Find( const Tag& t, const char* name )
        {
            for ( const Attr& a : t.Attrs )
                if ( a.Name == name )
                    return &a.Value;
            return nullptr;
        }

        // Reads the next tag starting at/after `i`. Returns false at end of input.
        bool NextTag( const char* s, size_t n, size_t& i, Tag& out )
        {
            while ( i < n && s[i] != '<' )
                ++i;
            if ( i >= n )
                return false;
            ++i;                                           // '<'
            if ( i < n && ( s[i] == '?' || s[i] == '!' ) ) // <?xml ...?>, <!-- --> , <!DOCTYPE>
            {
                while ( i < n && s[i] != '>' )
                    ++i;
                if ( i < n )
                    ++i;
                return NextTag( s, n, i, out );
            }

            out = Tag{};
            if ( i < n && s[i] == '/' )
            {
                out.Closing = true;
                ++i;
            }
            while ( i < n && !IsSpace( s[i] ) && s[i] != '>' && s[i] != '/' )
                out.Name += s[i++];

            while ( i < n && s[i] != '>' )
            {
                while ( i < n && IsSpace( s[i] ) )
                    ++i;
                if ( i < n && s[i] == '/' )
                {
                    out.SelfClosing = true;
                    ++i;
                    continue;
                }
                if ( i >= n || s[i] == '>' )
                    break;

                Attr a;
                while ( i < n && !IsSpace( s[i] ) && s[i] != '=' && s[i] != '>' && s[i] != '/' )
                    a.Name += s[i++];
                while ( i < n && IsSpace( s[i] ) )
                    ++i;
                if ( i < n && s[i] == '=' )
                {
                    ++i;
                    while ( i < n && IsSpace( s[i] ) )
                        ++i;
                    const char quote = ( i < n && ( s[i] == '"' || s[i] == '\'' ) ) ? s[i++] : '\0';
                    while ( i < n && ( quote ? s[i] != quote : !IsSpace( s[i] ) && s[i] != '>' ) )
                        a.Value += s[i++];
                    if ( quote && i < n )
                        ++i;
                }
                if ( !a.Name.empty() )
                    out.Attrs.push_back( std::move( a ) );
            }
            if ( i < n )
                ++i; // '>'
            return !out.Name.empty();
        }

        // --- number / list scanning -------------------------------------------------------------------

        void SkipSep( const char*& p, const char* end )
        {
            while ( p < end && ( IsSpace( *p ) || *p == ',' ) )
                ++p;
        }

        bool ReadNumber( const char*& p, const char* end, float& out )
        {
            SkipSep( p, end );
            if ( p >= end )
                return false;
            char*       stop = nullptr;
            const float v    = std::strtof( p, &stop );
            if ( stop == p )
                return false;
            p   = stop;
            out = v;
            return true;
        }

        std::vector<float> ReadNumbers( const std::string& s )
        {
            std::vector<float> out;
            const char*        p   = s.c_str();
            const char* const  end = p + s.size();
            float              v   = 0.0f;
            while ( ReadNumber( p, end, v ) )
                out.push_back( v );
            return out;
        }

        float AttrFloat( const Tag& t, const char* name, float fallback )
        {
            const std::string* v = Find( t, name );
            if ( !v )
                return fallback;
            const char*       p   = v->c_str();
            const char* const end = p + v->size();
            float             out = fallback;
            return ReadNumber( p, end, out ) ? out : fallback;
        }

        // --- 2x3 affine transform ---------------------------------------------------------------------

        struct Xform
        {
            // | A C E |
            // | B D F |
            float A = 1, B = 0, C = 0, D = 1, E = 0, F = 0;

            Vec2 Apply( Vec2 p ) const
            {
                return { A * p.X + C * p.Y + E, B * p.X + D * p.Y + F };
            }
            static Xform Mul( const Xform& m, const Xform& n ) // m then n applied as n*m
            {
                Xform r;
                r.A = m.A * n.A + m.B * n.C;
                r.B = m.A * n.B + m.B * n.D;
                r.C = m.C * n.A + m.D * n.C;
                r.D = m.C * n.B + m.D * n.D;
                r.E = m.E * n.A + m.F * n.C + n.E;
                r.F = m.E * n.B + m.F * n.D + n.F;
                return r;
            }
        };

        Xform ParseTransform( const std::string& s )
        {
            Xform  result;
            size_t i = 0;
            while ( i < s.size() )
            {
                const size_t open = s.find( '(', i );
                if ( open == std::string::npos )
                    break;
                std::string name = s.substr( i, open - i );
                name.erase(
                     std::remove_if( name.begin(), name.end(), []( char c ) { return IsSpace( c ) || c == ','; } ),
                     name.end() );
                const size_t close = s.find( ')', open );
                if ( close == std::string::npos )
                    break;
                const std::vector<float> a = ReadNumbers( s.substr( open + 1, close - open - 1 ) );
                i                          = close + 1;

                Xform m;
                if ( name == "translate" && !a.empty() )
                {
                    m.E = a[0];
                    m.F = a.size() > 1 ? a[1] : 0.0f;
                }
                else if ( name == "scale" && !a.empty() )
                {
                    m.A = a[0];
                    m.D = a.size() > 1 ? a[1] : a[0];
                }
                else if ( name == "matrix" && a.size() >= 6 )
                {
                    m.A = a[0], m.B = a[1], m.C = a[2], m.D = a[3], m.E = a[4], m.F = a[5];
                }
                else if ( name == "rotate" && !a.empty() )
                {
                    const float r = a[0] * kPi / 180.0f;
                    m.A = std::cos( r ), m.B = std::sin( r );
                    m.C = -m.B, m.D = m.A;
                    if ( a.size() >= 3 ) // rotate about (cx, cy)
                    {
                        Xform t1, t2;
                        t1.E = -a[1], t1.F = -a[2];
                        t2.E = a[1], t2.F = a[2];
                        m = Xform::Mul( Xform::Mul( t1, m ), t2 );
                    }
                }
                result = Xform::Mul( result, m );
            }
            return result;
        }

        uint32_t ParseFill( const std::string& v )
        {
            if ( v.empty() || v == "none" )
                return 0u; // no fill — the shape contributes nothing to the SDF
            if ( v[0] == '#' )
            {
                const std::string hex = v.substr( 1 );
                auto              nib = []( char c ) -> uint32_t
                {
                    if ( c >= '0' && c <= '9' )
                        return static_cast<uint32_t>( c - '0' );
                    if ( c >= 'a' && c <= 'f' )
                        return static_cast<uint32_t>( c - 'a' + 10 );
                    if ( c >= 'A' && c <= 'F' )
                        return static_cast<uint32_t>( c - 'A' + 10 );
                    return 0u;
                };
                if ( hex.size() >= 6 )
                    return ( nib( hex[0] ) << 28 ) | ( nib( hex[1] ) << 24 ) | ( nib( hex[2] ) << 20 ) |
                           ( nib( hex[3] ) << 16 ) | ( nib( hex[4] ) << 12 ) | ( nib( hex[5] ) << 8 ) | 0xFFu;
                if ( hex.size() >= 3 ) // #rgb
                {
                    const uint32_t r = nib( hex[0] ), g = nib( hex[1] ), b = nib( hex[2] );
                    return ( r << 28 ) | ( r << 24 ) | ( g << 20 ) | ( g << 16 ) | ( b << 12 ) | ( b << 8 ) |
                           0xFFu;
                }
            }
            return 0xFFFFFFFFu; // named colours / currentColor: opaque, the SDF is monochrome anyway
        }

        // --- path flattening ---------------------------------------------------------------------------

        struct Flattener
        {
            float                          Tol = 0.05f;
            std::vector<std::vector<Vec2>> Contours;
            std::vector<Vec2>              Current;

            void MoveTo( Vec2 p )
            {
                Close( false );
                Current.push_back( p );
            }
            void LineTo( Vec2 p )
            {
                if ( Current.empty() )
                    Current.push_back( p );
                else if ( std::abs( p.X - Current.back().X ) > 1e-6f ||
                          std::abs( p.Y - Current.back().Y ) > 1e-6f )
                    Current.push_back( p );
            }
            void CubicTo( Vec2 c1, Vec2 c2, Vec2 p )
            {
                if ( Current.empty() )
                    return;
                const Vec2 p0 = Current.back();
                // Segment count from the control polygon's length — plenty for an icon at bake size.
                const float len = Dist( p0, c1 ) + Dist( c1, c2 ) + Dist( c2, p );
                const int   n   = std::clamp( static_cast<int>( len / std::max( Tol, 1e-4f ) ), 2, 96 );
                for ( int i = 1; i <= n; ++i )
                {
                    const float t = static_cast<float>( i ) / static_cast<float>( n );
                    const float u = 1.0f - t;
                    Vec2        q;
                    q.X = u * u * u * p0.X + 3 * u * u * t * c1.X + 3 * u * t * t * c2.X + t * t * t * p.X;
                    q.Y = u * u * u * p0.Y + 3 * u * u * t * c1.Y + 3 * u * t * t * c2.Y + t * t * t * p.Y;
                    LineTo( q );
                }
            }
            void QuadTo( Vec2 c, Vec2 p )
            {
                if ( Current.empty() )
                    return;
                const Vec2 p0 = Current.back();
                CubicTo( { p0.X + 2.0f / 3.0f * ( c.X - p0.X ), p0.Y + 2.0f / 3.0f * ( c.Y - p0.Y ) },
                         { p.X + 2.0f / 3.0f * ( c.X - p.X ), p.Y + 2.0f / 3.0f * ( c.Y - p.Y ) }, p );
            }
            void Close( bool explicitClose )
            {
                (void)explicitClose;
                if ( Current.size() >= 3 )
                    Contours.push_back( Current );
                Current.clear();
            }
            static float Dist( Vec2 a, Vec2 b )
            {
                return std::sqrt( ( a.X - b.X ) * ( a.X - b.X ) + ( a.Y - b.Y ) * ( a.Y - b.Y ) );
            }
        };

        // SVG endpoint-parameterised arc -> centre parameterisation, then flattened as small cubics.
        void ArcTo( Flattener& f, Vec2 p0, float rx, float ry, float xRotDeg, bool largeArc, bool sweep, Vec2 p1 )
        {
            if ( rx == 0.0f || ry == 0.0f )
            {
                f.LineTo( p1 );
                return;
            }
            rx               = std::abs( rx );
            ry               = std::abs( ry );
            const float phi  = xRotDeg * kPi / 180.0f;
            const float cosP = std::cos( phi ), sinP = std::sin( phi );

            const float dx2 = ( p0.X - p1.X ) * 0.5f, dy2 = ( p0.Y - p1.Y ) * 0.5f;
            const float x1p = cosP * dx2 + sinP * dy2;
            const float y1p = -sinP * dx2 + cosP * dy2;

            float lambda = ( x1p * x1p ) / ( rx * rx ) + ( y1p * y1p ) / ( ry * ry );
            if ( lambda > 1.0f )
            {
                const float s = std::sqrt( lambda );
                rx *= s;
                ry *= s;
            }

            const float sign = ( largeArc != sweep ) ? 1.0f : -1.0f;
            float       num  = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
            const float den  = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
            num              = std::max( num, 0.0f );
            const float coef = den > 0.0f ? sign * std::sqrt( num / den ) : 0.0f;
            const float cxp  = coef * rx * y1p / ry;
            const float cyp  = -coef * ry * x1p / rx;

            const float cx = cosP * cxp - sinP * cyp + ( p0.X + p1.X ) * 0.5f;
            const float cy = sinP * cxp + cosP * cyp + ( p0.Y + p1.Y ) * 0.5f;

            auto angle = [&]( float ux, float uy, float vx, float vy )
            {
                const float dot = ux * vx + uy * vy;
                const float len = std::sqrt( ( ux * ux + uy * uy ) * ( vx * vx + vy * vy ) );
                float       a   = len > 0.0f ? std::acos( std::clamp( dot / len, -1.0f, 1.0f ) ) : 0.0f;
                if ( ux * vy - uy * vx < 0.0f )
                    a = -a;
                return a;
            };
            const float ux = ( x1p - cxp ) / rx, uy = ( y1p - cyp ) / ry;
            const float vx = ( -x1p - cxp ) / rx, vy = ( -y1p - cyp ) / ry;
            const float theta1 = angle( 1.0f, 0.0f, ux, uy );
            float       dTheta = angle( ux, uy, vx, vy );
            if ( !sweep && dTheta > 0.0f )
                dTheta -= 2.0f * kPi;
            else if ( sweep && dTheta < 0.0f )
                dTheta += 2.0f * kPi;

            const int n =
                 std::clamp( static_cast<int>( std::ceil( std::abs( dTheta ) / ( kPi * 0.25f ) ) ), 1, 32 );
            for ( int i = 1; i <= n; ++i )
            {
                const float t  = theta1 + dTheta * static_cast<float>( i ) / static_cast<float>( n );
                const float ex = rx * std::cos( t ), ey = ry * std::sin( t );
                f.LineTo( { cosP * ex - sinP * ey + cx, sinP * ex + cosP * ey + cy } );
            }
        }

        void ParsePathData( const std::string& d, Flattener& f )
        {
            const char*       p   = d.c_str();
            const char* const end = p + d.size();
            char              cmd = 0;
            Vec2              cur{ 0, 0 }, start{ 0, 0 }, lastC{ 0, 0 }, lastQ{ 0, 0 };
            char              prev = 0;

            auto num = [&]( float& v ) { return ReadNumber( p, end, v ); };

            while ( p < end )
            {
                SkipSep( p, end );
                if ( p >= end )
                    break;
                if ( std::isalpha( static_cast<unsigned char>( *p ) ) )
                    cmd = *p++;
                else if ( cmd == 'M' )
                    cmd = 'L'; // implicit line-to after a move-to
                else if ( cmd == 'm' )
                    cmd = 'l';

                const bool rel = ( cmd >= 'a' && cmd <= 'z' );
                const char c   = static_cast<char>( std::toupper( static_cast<unsigned char>( cmd ) ) );
                float      a[7]{};

                switch ( c )
                {
                    case 'M':
                        if ( !num( a[0] ) || !num( a[1] ) )
                            return;
                        cur = rel ? Vec2{ cur.X + a[0], cur.Y + a[1] } : Vec2{ a[0], a[1] };
                        f.MoveTo( cur );
                        start = cur;
                        break;
                    case 'L':
                        if ( !num( a[0] ) || !num( a[1] ) )
                            return;
                        cur = rel ? Vec2{ cur.X + a[0], cur.Y + a[1] } : Vec2{ a[0], a[1] };
                        f.LineTo( cur );
                        break;
                    case 'H':
                        if ( !num( a[0] ) )
                            return;
                        cur.X = rel ? cur.X + a[0] : a[0];
                        f.LineTo( cur );
                        break;
                    case 'V':
                        if ( !num( a[0] ) )
                            return;
                        cur.Y = rel ? cur.Y + a[0] : a[0];
                        f.LineTo( cur );
                        break;
                    case 'C':
                    {
                        for ( int k = 0; k < 6; ++k )
                            if ( !num( a[k] ) )
                                return;
                        const Vec2 c1 = rel ? Vec2{ cur.X + a[0], cur.Y + a[1] } : Vec2{ a[0], a[1] };
                        const Vec2 c2 = rel ? Vec2{ cur.X + a[2], cur.Y + a[3] } : Vec2{ a[2], a[3] };
                        const Vec2 p1 = rel ? Vec2{ cur.X + a[4], cur.Y + a[5] } : Vec2{ a[4], a[5] };
                        f.CubicTo( c1, c2, p1 );
                        lastC = c2;
                        cur   = p1;
                        break;
                    }
                    case 'S':
                    {
                        for ( int k = 0; k < 4; ++k )
                            if ( !num( a[k] ) )
                                return;
                        const bool smooth = ( prev == 'C' || prev == 'S' );
                        const Vec2 c1     = smooth ? Vec2{ 2 * cur.X - lastC.X, 2 * cur.Y - lastC.Y } : cur;
                        const Vec2 c2     = rel ? Vec2{ cur.X + a[0], cur.Y + a[1] } : Vec2{ a[0], a[1] };
                        const Vec2 p1     = rel ? Vec2{ cur.X + a[2], cur.Y + a[3] } : Vec2{ a[2], a[3] };
                        f.CubicTo( c1, c2, p1 );
                        lastC = c2;
                        cur   = p1;
                        break;
                    }
                    case 'Q':
                    {
                        for ( int k = 0; k < 4; ++k )
                            if ( !num( a[k] ) )
                                return;
                        const Vec2 cq = rel ? Vec2{ cur.X + a[0], cur.Y + a[1] } : Vec2{ a[0], a[1] };
                        const Vec2 p1 = rel ? Vec2{ cur.X + a[2], cur.Y + a[3] } : Vec2{ a[2], a[3] };
                        f.QuadTo( cq, p1 );
                        lastQ = cq;
                        cur   = p1;
                        break;
                    }
                    case 'T':
                    {
                        for ( int k = 0; k < 2; ++k )
                            if ( !num( a[k] ) )
                                return;
                        const bool smooth = ( prev == 'Q' || prev == 'T' );
                        const Vec2 cq     = smooth ? Vec2{ 2 * cur.X - lastQ.X, 2 * cur.Y - lastQ.Y } : cur;
                        const Vec2 p1     = rel ? Vec2{ cur.X + a[0], cur.Y + a[1] } : Vec2{ a[0], a[1] };
                        f.QuadTo( cq, p1 );
                        lastQ = cq;
                        cur   = p1;
                        break;
                    }
                    case 'A':
                    {
                        for ( int k = 0; k < 7; ++k )
                            if ( !num( a[k] ) )
                                return;
                        const Vec2 p1 = rel ? Vec2{ cur.X + a[5], cur.Y + a[6] } : Vec2{ a[5], a[6] };
                        ArcTo( f, cur, a[0], a[1], a[2], a[3] != 0.0f, a[4] != 0.0f, p1 );
                        cur = p1;
                        break;
                    }
                    case 'Z':
                        f.LineTo( start );
                        f.Close( true );
                        cur = start;
                        break;
                    default:
                        return; // unknown command: stop rather than mis-read the rest
                }
                prev = c;
            }
            f.Close( false );
        }

        void AddRect( Flattener& f, float x, float y, float w, float h, float rx, float ry )
        {
            if ( w <= 0.0f || h <= 0.0f )
                return;
            rx = std::min( rx, w * 0.5f );
            ry = std::min( ry, h * 0.5f );
            if ( rx <= 0.0f || ry <= 0.0f )
            {
                f.MoveTo( { x, y } );
                f.LineTo( { x + w, y } );
                f.LineTo( { x + w, y + h } );
                f.LineTo( { x, y + h } );
                f.LineTo( { x, y } );
                f.Close( true );
                return;
            }
            const float k = 0.5522847498f; // circle-arc cubic approximation
            f.MoveTo( { x + rx, y } );
            f.LineTo( { x + w - rx, y } );
            f.CubicTo( { x + w - rx + rx * k, y }, { x + w, y + ry - ry * k }, { x + w, y + ry } );
            f.LineTo( { x + w, y + h - ry } );
            f.CubicTo( { x + w, y + h - ry + ry * k }, { x + w - rx + rx * k, y + h }, { x + w - rx, y + h } );
            f.LineTo( { x + rx, y + h } );
            f.CubicTo( { x + rx - rx * k, y + h }, { x, y + h - ry + ry * k }, { x, y + h - ry } );
            f.LineTo( { x, y + ry } );
            f.CubicTo( { x, y + ry - ry * k }, { x + rx - rx * k, y }, { x + rx, y } );
            f.Close( true );
        }

        void AddEllipse( Flattener& f, float cx, float cy, float rx, float ry )
        {
            if ( rx <= 0.0f || ry <= 0.0f )
                return;
            const float k = 0.5522847498f;
            f.MoveTo( { cx + rx, cy } );
            f.CubicTo( { cx + rx, cy + ry * k }, { cx + rx * k, cy + ry }, { cx, cy + ry } );
            f.CubicTo( { cx - rx * k, cy + ry }, { cx - rx, cy + ry * k }, { cx - rx, cy } );
            f.CubicTo( { cx - rx, cy - ry * k }, { cx - rx * k, cy - ry }, { cx, cy - ry } );
            f.CubicTo( { cx + rx * k, cy - ry }, { cx + rx, cy - ry * k }, { cx + rx, cy } );
            f.Close( true );
        }
    } // namespace

    VectorImage ParseSvg( const char* xml, size_t size, float curveTolerance )
    {
        VectorImage out;
        if ( !xml || size == 0 )
            return out;

        std::vector<Xform> stack{ Xform{} }; // <g> transform stack
        size_t             i = 0;
        Tag                tag;
        float              vbX = 0, vbY = 0, vbW = 0, vbH = 0;

        while ( NextTag( xml, size, i, tag ) )
        {
            if ( tag.Closing )
            {
                if ( tag.Name == "g" && stack.size() > 1 )
                    stack.pop_back();
                continue;
            }

            if ( tag.Name == "svg" )
            {
                if ( const std::string* vb = Find( tag, "viewBox" ) )
                {
                    const std::vector<float> v = ReadNumbers( *vb );
                    if ( v.size() >= 4 )
                        vbX = v[0], vbY = v[1], vbW = v[2], vbH = v[3];
                }
                if ( vbW <= 0.0f || vbH <= 0.0f ) // no viewBox: fall back to width/height
                {
                    vbW = AttrFloat( tag, "width", 0.0f );
                    vbH = AttrFloat( tag, "height", 0.0f );
                }
                continue;
            }

            Xform xf = stack.back();
            if ( const std::string* t = Find( tag, "transform" ) )
                xf = Xform::Mul( ParseTransform( *t ), xf );

            if ( tag.Name == "g" )
            {
                if ( !tag.SelfClosing )
                    stack.push_back( xf );
                continue;
            }

            Flattener f;
            f.Tol = curveTolerance;
            if ( tag.Name == "path" )
            {
                if ( const std::string* d = Find( tag, "d" ) )
                    ParsePathData( *d, f );
            }
            else if ( tag.Name == "rect" )
            {
                const float rx = AttrFloat( tag, "rx", 0.0f );
                const float ry = AttrFloat( tag, "ry", rx );
                AddRect( f, AttrFloat( tag, "x", 0.0f ), AttrFloat( tag, "y", 0.0f ),
                         AttrFloat( tag, "width", 0.0f ), AttrFloat( tag, "height", 0.0f ), rx, ry );
            }
            else if ( tag.Name == "circle" )
            {
                const float r = AttrFloat( tag, "r", 0.0f );
                AddEllipse( f, AttrFloat( tag, "cx", 0.0f ), AttrFloat( tag, "cy", 0.0f ), r, r );
            }
            else if ( tag.Name == "ellipse" )
            {
                AddEllipse( f, AttrFloat( tag, "cx", 0.0f ), AttrFloat( tag, "cy", 0.0f ),
                            AttrFloat( tag, "rx", 0.0f ), AttrFloat( tag, "ry", 0.0f ) );
            }
            else if ( tag.Name == "polygon" || tag.Name == "polyline" )
            {
                const std::string* pts = Find( tag, "points" );
                if ( !pts )
                    continue;
                const std::vector<float> v = ReadNumbers( *pts );
                for ( size_t k = 0; k + 1 < v.size(); k += 2 )
                {
                    if ( k == 0 )
                        f.MoveTo( { v[0], v[1] } );
                    else
                        f.LineTo( { v[k], v[k + 1] } );
                }
                f.Close( true );
            }
            else
            {
                continue; // text, defs, clipPath, ... — ignored on purpose
            }

            const uint32_t fill = Find( tag, "fill" ) ? ParseFill( *Find( tag, "fill" ) ) : 0xFFFFFFFFu;
            if ( fill == 0u || f.Contours.empty() )
                continue; // fill="none" (a stroke-only path) contributes nothing to a filled SDF

            Shape shape;
            shape.FillRGBA = fill;
            shape.Contours.reserve( f.Contours.size() );
            for ( auto& c : f.Contours )
            {
                for ( Vec2& p : c )
                {
                    p = xf.Apply( p );
                    p.X -= vbX;
                    p.Y -= vbY;
                }
                shape.Contours.push_back( std::move( c ) );
            }
            out.Shapes.push_back( std::move( shape ) );
        }

        out.Width  = vbW;
        out.Height = vbH;
        return out;
    }

    std::vector<uint8_t> RasterizeSdf( const VectorImage& image, uint32_t size, int padding, size_t shapeBegin,
                                       size_t shapeEnd )
    {
        std::vector<uint8_t> out;
        if ( !image.Valid() || size == 0 || padding < 0 )
            return out;
        shapeEnd = std::min( shapeEnd, image.Shapes.size() );
        if ( shapeBegin >= shapeEnd )
            return out;

        // Fit the viewBox into the inner box, preserving aspect and centring the shorter axis.
        const float    s    = static_cast<float>( size ) / std::max( image.Width, image.Height );
        const float    offX = static_cast<float>( padding ) + ( size - image.Width * s ) * 0.5f;
        const float    offY = static_cast<float>( padding ) + ( size - image.Height * s ) * 0.5f;
        const uint32_t dim  = size + 2u * static_cast<uint32_t>( padding );

        struct Seg
        {
            float X0, Y0, X1, Y1;
        };
        std::vector<Seg> segs;
        for ( size_t si = shapeBegin; si < shapeEnd; ++si )
            for ( const auto& c : image.Shapes[si].Contours )
                for ( size_t k = 0; k < c.size(); ++k )
                {
                    const Vec2 a = c[k];
                    const Vec2 b = c[( k + 1 ) % c.size()];
                    segs.push_back( { a.X * s + offX, a.Y * s + offY, b.X * s + offX, b.Y * s + offY } );
                }
        if ( segs.empty() )
            return out;

        // The byte range spans exactly kSdfAtlasDistanceRangeTexels texels — the same band the glyph
        // atlas uses and the number the shader divides by. It used to be derived from `padding` instead,
        // which gave icons a 12-texel band against the glyphs' 10 while a comment claimed they matched.
        const float pixelDistScale = 255.0F / Core::Formats::kSdfAtlasDistanceRangeTexels;

        out.resize( static_cast<size_t>( dim ) * dim );
        for ( uint32_t y = 0; y < dim; ++y )
            for ( uint32_t x = 0; x < dim; ++x )
            {
                const float px = static_cast<float>( x ) + 0.5f;
                const float py = static_cast<float>( y ) + 0.5f;

                float best    = 1e30f;
                int   winding = 0;
                for ( const Seg& sg : segs )
                {
                    // Distance to the segment.
                    const float dx = sg.X1 - sg.X0, dy = sg.Y1 - sg.Y0;
                    const float ll = dx * dx + dy * dy;
                    float       t  = ll > 0.0f ? ( ( px - sg.X0 ) * dx + ( py - sg.Y0 ) * dy ) / ll : 0.0f;
                    t              = std::clamp( t, 0.0f, 1.0f );
                    const float qx = sg.X0 + t * dx - px, qy = sg.Y0 + t * dy - py;
                    best = std::min( best, qx * qx + qy * qy );

                    // Non-zero winding: count signed crossings of the ray going +X from the texel.
                    if ( ( sg.Y0 <= py ) != ( sg.Y1 <= py ) )
                    {
                        const float xInt = sg.X0 + ( py - sg.Y0 ) / ( sg.Y1 - sg.Y0 ) * dx;
                        if ( xInt > px )
                            winding += ( sg.Y1 > sg.Y0 ) ? 1 : -1;
                    }
                }

                const float dist  = std::sqrt( best ) * ( winding != 0 ? 1.0f : -1.0f );
                const float value =
                     static_cast<float>( Core::Formats::kSdfAtlasOnEdgeByte ) + dist * pixelDistScale;
                out[static_cast<size_t>( y ) * dim + x] =
                     static_cast<uint8_t>( std::clamp( value, 0.0f, 255.0f ) );
            }
        return out;
    }
} // namespace Desert::Vector
