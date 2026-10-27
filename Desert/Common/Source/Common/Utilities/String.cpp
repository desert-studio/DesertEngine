#include <Common/Utilities/String.hpp>

#include <format>

namespace Common::Utils
{
    const std::string String::BytesToString( const uint64_t bytes )
    {
        const size_t KB = 1024;
        const size_t MB = KB * 1024;
        const size_t GB = MB * 1024;
        const size_t TB = GB * 1024;

        double      size = static_cast<double>( bytes );
        std::string result;

        if ( bytes >= TB )
        {
            result = std::format( "{:f} TB", size / TB );
        }
        else if ( bytes >= GB )
        {
            result = std::format( "{:f} GB", size / GB );
        }
        else if ( bytes >= MB )
        {
            result = std::format( "{:f} MB", size / MB );
        }
        else if ( bytes >= KB )
        {
            result = std::format( "{:f} KB", size / KB );
        }
        else
        {
            result = std::format( "{} B", bytes );
        }

        return result;
    }

    const std::string String::GetPathHash( const Common::Filepath& path )
    {
        return std::to_string( std::hash<std::string>{}( path.string() ) );
    }

} // namespace Common::Utils
