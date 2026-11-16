#include <Engine/Graphic/IesProfile.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>

namespace Desert::Graphic
{
    namespace
    {
        // The whitespace- and comma-separated numbers after the TILT block, read in order.
        class IesNumberReader
        {
        public:
            explicit IesNumberReader( std::string_view text ) noexcept : m_Text( text )
            {
            }

            std::optional<float> Next() noexcept
            {
                while ( m_Cursor < m_Text.size() && IsSeparator( m_Text[m_Cursor] ) )
                    ++m_Cursor;
                if ( m_Cursor >= m_Text.size() )
                    return std::nullopt;
                const char* begin        = m_Text.data() + m_Cursor;
                const char* end          = m_Text.data() + m_Text.size();
                float       value        = 0.0f;
                const auto [stop, error] = std::from_chars( begin, end, value );
                if ( error != std::errc() )
                    return std::nullopt;
                m_Cursor += static_cast<std::size_t>( stop - begin );
                return value;
            }

        private:
            static bool IsSeparator( char c ) noexcept
            {
                return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',';
            }

            std::string_view m_Text;
            std::size_t      m_Cursor = 0;
        };

        bool ReadMany( IesNumberReader& reader, std::vector<float>& out, std::size_t count ) noexcept
        {
            out.resize( count );
            for ( std::size_t i = 0; i < count; ++i )
            {
                const std::optional<float> value = reader.Next();
                if ( !value )
                    return false;
                out[i] = *value;
            }
            return true;
        }

        bool Rising( const std::vector<float>& angles ) noexcept
        {
            for ( std::size_t i = 1; i < angles.size(); ++i )
                if ( !( angles[i] > angles[i - 1] ) )
                    return false;
            return true;
        }

        // The bracketing pair of @p angles around @p value and the blend between them; nullopt outside.
        struct Bracket
        {
            std::size_t Low   = 0;
            std::size_t High  = 0;
            float       Blend = 0.0f;
        };

        std::optional<Bracket> BracketOf( const std::vector<float>& angles, float value ) noexcept
        {
            constexpr float kTolerance = 1e-3f;
            if ( angles.size() == 1 )
                return Bracket{};
            if ( value < angles.front() - kTolerance || value > angles.back() + kTolerance )
                return std::nullopt;
            value                  = std::clamp( value, angles.front(), angles.back() );
            const auto  upper      = std::upper_bound( angles.begin(), angles.end(), value );
            std::size_t high       = static_cast<std::size_t>( upper - angles.begin() );
            high                   = std::min( std::max<std::size_t>( high, 1 ), angles.size() - 1 );
            const std::size_t lo   = high - 1;
            const float       span = angles[high] - angles[lo];
            Bracket bracket{ lo, high, 0.0f };
            if ( span > 0.0f )
                bracket.Blend = std::clamp( ( value - angles[lo] ) / span, 0.0f, 1.0f );
            return bracket;
        }

        // Type C symmetry: the file's horizontal range stands for the full turn.
        float FoldHorizontal( const std::vector<float>& horizontal, float degrees ) noexcept
        {
            if ( horizontal.size() == 1 )
                return horizontal.front();
            const float last = horizontal.back();
            if ( last <= 90.0f + 1e-3f )
            {
                float folded = std::fmod( degrees, 180.0f );
                if ( folded > 90.0f )
                    folded = 180.0f - folded;
                return folded;
            }
            if ( last <= 180.0f + 1e-3f )
                return degrees > 180.0f ? 360.0f - degrees : degrees;
            return degrees;
        }
    } // namespace

    Common::ResultStr<IesPhotometricData> ParseIesProfile( std::string_view text )
    {
        const std::size_t tilt = text.find( "TILT=" );
        if ( tilt == std::string_view::npos )
            return Common::MakeError<IesPhotometricData>( "IES profile has no TILT= line (not an LM-63 file)" );
        std::size_t lineEnd = text.find( '\n', tilt );
        if ( lineEnd == std::string_view::npos )
            return Common::MakeError<IesPhotometricData>( "IES profile ends at its TILT= line" );
        std::string_view tiltValue = text.substr( tilt + 5, lineEnd - tilt - 5 );
        while ( !tiltValue.empty() && ( tiltValue.back() == '\r' || tiltValue.back() == ' ' ) )
            tiltValue.remove_suffix( 1 );

        IesNumberReader reader( text.substr( lineEnd + 1 ) );
        if ( tiltValue == "INCLUDE" )
        {
            // Lamp-to-luminaire geometry, the pair count, then that many angles and multipliers: not part of
            // the distribution the shaders sample (it scales the lamp by its tilt, which a placed light has not).
            const std::optional<float> geometry = reader.Next();
            const std::optional<float> pairs    = reader.Next();
            if ( !geometry || !pairs || *pairs < 0.0f )
                return Common::MakeError<IesPhotometricData>( "IES profile's TILT=INCLUDE block is truncated" );
            std::vector<float> skipped;
            if ( !ReadMany( reader, skipped, 2 * static_cast<std::size_t>( *pairs ) ) )
                return Common::MakeError<IesPhotometricData>( "IES profile's TILT=INCLUDE block is truncated" );
        }
        else if ( tiltValue != "NONE" )
        {
            return Common::MakeFormattedError<IesPhotometricData>(
                 "IES profile names a separate tilt file '{}'; only TILT=NONE and TILT=INCLUDE are read",
                 std::string( tiltValue ) );
        }

        std::vector<float> header;
        if ( !ReadMany( reader, header, 13 ) )
            return Common::MakeError<IesPhotometricData>( "IES profile's photometric header is truncated" );
        const float candelaMultiplier = header[2];
        const auto  verticalCount     = static_cast<int64_t>( header[3] );
        const auto  horizontalCount   = static_cast<int64_t>( header[4] );
        const auto  photometricType   = static_cast<int32_t>( header[5] );
        if ( verticalCount < 1 || horizontalCount < 1 || verticalCount > 4096 || horizontalCount > 4096 )
            return Common::MakeFormattedError<IesPhotometricData>(
                 "IES profile announces {} vertical x {} horizontal angles", verticalCount, horizontalCount );
        if ( photometricType != 1 )
            return Common::MakeFormattedError<IesPhotometricData>(
                 "IES profile is photometric type {}; only type C (1) is supported", photometricType );

        IesPhotometricData data;
        if ( !ReadMany( reader, data.VerticalAngles, static_cast<std::size_t>( verticalCount ) ) ||
             !ReadMany( reader, data.HorizontalAngles, static_cast<std::size_t>( horizontalCount ) ) ||
             !ReadMany( reader, data.Candela, static_cast<std::size_t>( verticalCount * horizontalCount ) ) )
            return Common::MakeError<IesPhotometricData>(
                 "IES profile has fewer angles or candela than announced" );
        if ( !Rising( data.VerticalAngles ) || !Rising( data.HorizontalAngles ) )
            return Common::MakeError<IesPhotometricData>( "IES profile's angles do not rise" );
        for ( float& candela : data.Candela )
            candela = std::max( candela * candelaMultiplier, 0.0f );
        return Common::MakeSuccess( std::move( data ) );
    }

    Common::ResultStr<IesProfileGrid> ResampleIesProfile( const IesPhotometricData& data )
    {
        const std::size_t verticalCount = data.VerticalAngles.size();
        if ( verticalCount == 0 || data.HorizontalAngles.empty() ||
             data.Candela.size() != verticalCount * data.HorizontalAngles.size() )
            return Common::MakeError<IesProfileGrid>( "IES profile data is empty or inconsistent" );

        IesProfileGrid grid;
        grid.PeakCandela = *std::max_element( data.Candela.begin(), data.Candela.end() );
        if ( !( grid.PeakCandela > 0.0f ) )
            return Common::MakeError<IesProfileGrid>( "IES profile emits nothing (its peak candela is 0)" );

        const float inversePeak = 1.0f / grid.PeakCandela;
        grid.Candela.assign( static_cast<std::size_t>( kIesProfileRows ) * kIesProfileColumns, 0.0f );
        for ( uint32_t row = 0; row < kIesProfileRows; ++row )
        {
            const float horizontal =
                 360.0f * static_cast<float>( row ) / static_cast<float>( kIesProfileRows - 1 );
            const std::optional<Bracket> h =
                 BracketOf( data.HorizontalAngles, FoldHorizontal( data.HorizontalAngles, horizontal ) );
            if ( !h )
                continue;
            for ( uint32_t column = 0; column < kIesProfileColumns; ++column )
            {
                const float vertical =
                     180.0f * static_cast<float>( column ) / static_cast<float>( kIesProfileColumns - 1 );
                const std::optional<Bracket> v = BracketOf( data.VerticalAngles, vertical );
                if ( !v )
                    continue;
                const auto at = [&]( std::size_t hi, std::size_t vi )
                { return data.Candela[hi * verticalCount + vi]; };
                const float low =
                     at( h->Low, v->Low ) + ( at( h->Low, v->High ) - at( h->Low, v->Low ) ) * v->Blend;
                const float high =
                     at( h->High, v->Low ) + ( at( h->High, v->High ) - at( h->High, v->Low ) ) * v->Blend;
                grid.Candela[static_cast<std::size_t>( row ) * kIesProfileColumns + column] =
                     ( low + ( high - low ) * h->Blend ) * inversePeak;
            }
        }
        return Common::MakeSuccess( std::move( grid ) );
    }
} // namespace Desert::Graphic
