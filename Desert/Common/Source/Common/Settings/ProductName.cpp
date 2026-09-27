#include "ProductName.hpp"

#include <array>
#include <cctype>

namespace Common::Settings
{
    namespace
    {
        bool IsForbidden( const char c )
        {
            const auto byte = static_cast<unsigned char>( c );
            if ( byte < 0x20 || byte == 0x7F )
                return true;
            return std::string_view( "<>:\"/\\|?*" ).find( c ) != std::string_view::npos;
        }

        bool EqualsIgnoringCase( std::string_view inA, std::string_view inB )
        {
            if ( inA.size() != inB.size() )
                return false;
            for ( std::size_t i = 0; i < inA.size(); ++i )
                if ( std::toupper( static_cast<unsigned char>( inA[i] ) ) !=
                     std::toupper( static_cast<unsigned char>( inB[i] ) ) )
                    return false;
            return true;
        }

        // Windows compares the part before the FIRST dot, with trailing spaces ignored ("CON .txt" is
        // still the console).
        bool IsReservedDeviceStem( std::string_view inStem )
        {
            while ( !inStem.empty() && inStem.back() == ' ' )
                inStem.remove_suffix( 1 );

            static constexpr std::array<std::string_view, 4> kPlain = { "CON", "PRN", "AUX", "NUL" };
            for ( const std::string_view name : kPlain )
                if ( EqualsIgnoringCase( inStem, name ) )
                    return true;

            if ( inStem.size() == 4 && inStem[3] >= '1' && inStem[3] <= '9' )
            {
                const std::string_view prefix = inStem.substr( 0, 3 );
                return EqualsIgnoringCase( prefix, "COM" ) || EqualsIgnoringCase( prefix, "LPT" );
            }
            return false;
        }
    } // namespace

    std::string SanitizeProductName( std::string_view inName )
    {
        std::string safe( inName );
        for ( char& c : safe )
            if ( IsForbidden( c ) )
                c = '_';

        while ( !safe.empty() && ( safe.back() == '.' || safe.back() == ' ' ) )
            safe.pop_back();

        // "." and ".." are already empty after the trim; the explicit test keeps the rule readable
        // where it is stated rather than relying on the order of the steps above.
        if ( safe.empty() || safe == "." || safe == ".." )
            return kDefaultProductDirectoryName;

        const std::size_t stemEnd = safe.find( '.' );
        if ( IsReservedDeviceStem( std::string_view( safe ).substr( 0, stemEnd ) ) )
            safe.insert( stemEnd == std::string::npos ? safe.size() : stemEnd, "_" );
        return safe;
    }
} // namespace Common::Settings
