#include "ShaderMapCache.hpp"

#include <Common/Content/DerivedDataCache.hpp>

#include <bit>
#include <cstring>
#include <format>
#include <type_traits>

namespace Desert::Core
{
    namespace
    {
        static_assert( std::endian::native == std::endian::little, "the shader map is written little-endian" );

        constexpr uint32_t kMagic = 0x504d5344u; // "DSMP"

        constexpr Common::DDC::Deriver kShaderMapDeriver{
             "ShaderMap", ".dsmap", { 0x3b8e51d0c4a27f19ULL, 0x9d06e2b75a1c48f3ULL + kShaderMapFormatVersion } };

        // ONE field list per struct, used by the writer AND the reader, written as a structured binding so
        // a field added to the struct is a compile error here instead of a value the cache silently drops.
        template <class Self, class Fn>
            requires std::is_same_v<std::remove_const_t<Self>, Formats::ShaderParam>
        void VisitFields( Self& p, Fn&& fn )
        {
            auto& [name, displayName, category, tooltip, type, widget, isTexture, timing, assetKind, isCube, lo,
                   hi, defaultValue, defaultTexture] = p;
            fn( name ), fn( displayName ), fn( category ), fn( tooltip ), fn( type ), fn( widget ),
                 fn( isTexture );
            fn( timing ), fn( assetKind ), fn( isCube ), fn( lo ), fn( hi ), fn( defaultValue ),
                 fn( defaultTexture );
        }

        template <class Self, class Fn>
            requires std::is_same_v<std::remove_const_t<Self>, Formats::ShaderRenderState>
        void VisitFields( Self& s, Fn&& fn )
        {
            auto& [cull, depthTest, depthWrite, depthCompare, blend, topology, patchPoints, blendSrc, blendDst,
                   stencilTest, stencilCompare, stencilRef, stencilFail, stencilPass, stencilDepthFail] = s;
            fn( cull ), fn( depthTest ), fn( depthWrite ), fn( depthCompare ), fn( blend ), fn( topology );
            fn( patchPoints ), fn( blendSrc ), fn( blendDst ), fn( stencilTest ), fn( stencilCompare );
            fn( stencilRef ), fn( stencilFail ), fn( stencilPass ), fn( stencilDepthFail );
        }

        template <class Self, class Fn>
            requires std::is_same_v<std::remove_const_t<Self>, Formats::ShaderProgramMeta>
        void VisitFields( Self& m, Fn&& fn )
        {
            auto& [params, state, domain, passNames, mediumSource] = m;
            fn( params ), fn( state ), fn( domain ), fn( passNames ), fn( mediumSource );
        }

        template <class T>
        struct IsOptional : std::false_type
        {
        };
        template <class T>
        struct IsOptional<std::optional<T>> : std::true_type
        {
        };
        template <class T>
        struct IsVector : std::false_type
        {
        };
        template <class T>
        struct IsVector<std::vector<T>> : std::true_type
        {
        };

        struct Writer
        {
            std::string Bytes;

            template <class T>
            void Raw( const T value )
            {
                Bytes.append( reinterpret_cast<const char*>( &value ), sizeof( T ) );
            }

            template <class T>
            void operator()( const T& v )
            {
                if constexpr ( std::is_same_v<T, bool> )
                    Raw<uint8_t>( v ? 1 : 0 );
                else if constexpr ( std::is_enum_v<T> )
                    Raw<uint32_t>( static_cast<uint32_t>( v ) );
                else if constexpr ( std::is_same_v<T, float> || std::is_same_v<T, uint32_t> )
                    Raw<T>( v );
                else if constexpr ( std::is_same_v<T, std::string> )
                {
                    Raw<uint32_t>( static_cast<uint32_t>( v.size() ) );
                    Bytes.append( v );
                }
                else if constexpr ( IsOptional<T>::value )
                {
                    ( *this )( v.has_value() );
                    if ( v )
                        ( *this )( *v );
                }
                else if constexpr ( IsVector<T>::value )
                {
                    Raw<uint32_t>( static_cast<uint32_t>( v.size() ) );
                    for ( const auto& e : v )
                        ( *this )( e );
                }
                else if constexpr ( std::is_same_v<T, glm::vec4> )
                {
                    for ( int i = 0; i < 4; ++i )
                        Raw<float>( v[i] );
                }
                else
                    VisitFields( v, *this );
            }
        };

        struct Reader
        {
            std::string_view Bytes;
            size_t           Pos = 0;
            std::string      Error;

            bool Fail( std::string what )
            {
                if ( Error.empty() )
                    Error = std::format( "{} at byte {} of {}", what, Pos, Bytes.size() );
                return false;
            }

            template <class T>
            bool Raw( T& value )
            {
                if ( !Error.empty() )
                    return false;
                if ( Bytes.size() - Pos < sizeof( T ) )
                    return Fail( std::format( "truncated: {} byte(s) wanted", sizeof( T ) ) );
                std::memcpy( &value, Bytes.data() + Pos, sizeof( T ) );
                Pos += sizeof( T );
                return true;
            }

            // A count is checked against the bytes left before anything is allocated for it.
            bool Count( uint32_t& n, const size_t minBytesEach )
            {
                if ( !Raw( n ) )
                    return false;
                if ( static_cast<uint64_t>( n ) * minBytesEach > Bytes.size() - Pos )
                    return Fail( std::format( "count {} exceeds the {} byte(s) left", n, Bytes.size() - Pos ) );
                return true;
            }

            template <class T>
            void operator()( T& v )
            {
                if constexpr ( std::is_same_v<T, bool> )
                {
                    uint8_t b = 0;
                    if ( Raw( b ) && b > 1 )
                        Fail( std::format( "bool byte {}", b ) );
                    v = b == 1;
                }
                else if constexpr ( std::is_enum_v<T> )
                {
                    uint32_t raw = 0;
                    Raw( raw );
                    v = static_cast<T>( raw );
                }
                else if constexpr ( std::is_same_v<T, float> || std::is_same_v<T, uint32_t> )
                    Raw( v );
                else if constexpr ( std::is_same_v<T, std::string> )
                {
                    uint32_t n = 0;
                    if ( Count( n, 1 ) )
                    {
                        v.assign( Bytes.data() + Pos, n );
                        Pos += n;
                    }
                }
                else if constexpr ( IsOptional<T>::value )
                {
                    bool present = false;
                    ( *this )( present );
                    v.reset();
                    if ( present )
                    {
                        typename T::value_type value{};
                        ( *this )( value );
                        v = std::move( value );
                    }
                }
                else if constexpr ( IsVector<T>::value )
                {
                    uint32_t n = 0;
                    v.clear();
                    if ( Count( n, 1 ) )
                    {
                        v.resize( n );
                        for ( auto& e : v )
                            ( *this )( e );
                    }
                }
                else if constexpr ( std::is_same_v<T, glm::vec4> )
                {
                    for ( int i = 0; i < 4; ++i )
                        Raw( v[i] );
                }
                else
                    VisitFields( v, *this );
            }
        };
    } // namespace

    std::string SerializeShaderMap( const ShaderMap& map )
    {
        Writer out;
        out.Raw( kMagic );
        out.Raw( kShaderMapFormatVersion );
        out( map.Meta );
        out.Raw( static_cast<uint32_t>( map.Stages.size() ) );
        for ( const auto& stage : map.Stages )
        {
            out.Raw( static_cast<uint32_t>( stage.Stage ) );
            out.Raw( static_cast<uint32_t>( stage.Spirv.size() ) );
            out.Bytes.append( reinterpret_cast<const char*>( stage.Spirv.data() ),
                              stage.Spirv.size() * sizeof( uint32_t ) );
        }
        return std::move( out.Bytes );
    }

    Common::ResultStr<ShaderMap> DeserializeShaderMap( const std::string_view bytes )
    {
        Reader   in{ bytes };
        uint32_t magic = 0, version = 0;
        in.Raw( magic );
        in.Raw( version );
        if ( in.Error.empty() && magic != kMagic )
            in.Fail( std::format( "magic {:08x}, expected {:08x}", magic, kMagic ) );
        if ( in.Error.empty() && version != kShaderMapFormatVersion )
            in.Fail( std::format( "format version {}, this build reads {}", version, kShaderMapFormatVersion ) );

        ShaderMap map;
        in( map.Meta );
        uint32_t stages = 0;
        in.Count( stages, 2 * sizeof( uint32_t ) );
        for ( uint32_t i = 0; in.Error.empty() && i < stages; ++i )
        {
            uint32_t stage = 0, words = 0;
            in.Raw( stage );
            if ( !in.Count( words, sizeof( uint32_t ) ) )
                break;
            ShaderMapStage& out = map.Stages.emplace_back();
            out.Stage           = static_cast<Formats::ShaderStage>( stage );
            out.Spirv.resize( words );
            std::memcpy( out.Spirv.data(), bytes.data() + in.Pos, words * sizeof( uint32_t ) );
            in.Pos += words * sizeof( uint32_t );
        }
        if ( in.Error.empty() && in.Pos != bytes.size() )
            in.Fail( std::format( "{} trailing byte(s)", bytes.size() - in.Pos ) );
        if ( !in.Error.empty() )
            return Common::MakeError<ShaderMap>( "shader map: " + in.Error );
        return Common::MakeSuccess( std::move( map ) );
    }

    ShaderMapLookup TryLoadShaderMap( const uint64_t key )
    {
        const uint64_t ddcKey = Common::DDC::MakeKey( kShaderMapDeriver, key, nullptr, 0 );
        const auto     bytes  = Common::DDC::Get( kShaderMapDeriver, ddcKey );
        if ( !bytes )
            return {};
        auto parsed = DeserializeShaderMap( *bytes );
        if ( !parsed.IsSuccess() )
            return { std::nullopt,
                     std::format( "{}: {}", Common::DDC::PathFor( kShaderMapDeriver, ddcKey ).string(),
                                  parsed.GetError() ) };
        return { std::move( parsed.GetValue() ), {} };
    }

    Common::BoolResultStr StoreShaderMap( const uint64_t key, const ShaderMap& map )
    {
        // DDC::Put writes a temporary and renames it into place, so a killed process leaves either the old
        // entry or the new one — never a truncated blob the next start would have to reject.
        return Common::DDC::Put( kShaderMapDeriver, Common::DDC::MakeKey( kShaderMapDeriver, key, nullptr, 0 ),
                                 SerializeShaderMap( map ) );
    }
} // namespace Desert::Core
