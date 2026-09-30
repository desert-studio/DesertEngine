// AsyncComputeFallbackLog - the once-per-backend announcement of AsyncCompute passes that run on the graphics
// queue because the device has no separate compute queue family (RDG-CONTRACTS B(4)).

#include <Engine/Graphic/RDG/RDGBackend.hpp>

#include <format>
#include <iterator>
#include <string>
#include <utility>

namespace Desert::Graphic::RDG
{
    AsyncComputeFallbackLog::AsyncComputeFallbackLog( Sink sink ) : m_Sink( std::move( sink ) )
    {
    }

    bool AsyncComputeFallbackLog::Report( std::span<const std::string_view> passNames )
    {
        if ( m_LinesLogged > 0 || passNames.empty() )
            return false;
        std::string names;
        for ( std::string_view name : passNames )
            std::format_to( std::back_inserter( names ), "{}{}", names.empty() ? "" : ", ", name );
        const std::string line = std::format(
             "RDG: no separate compute queue family; AsyncCompute passes run on the graphics queue: {}", names );
        if ( m_Sink )
            m_Sink( line );
        ++m_LinesLogged;
        return true;
    }

    uint32_t AsyncComputeFallbackLog::GetLinesLogged() const
    {
        return m_LinesLogged;
    }
} // namespace Desert::Graphic::RDG
