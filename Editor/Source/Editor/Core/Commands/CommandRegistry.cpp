#include "CommandRegistry.hpp"

#include <utility>

namespace Desert::Editor
{
    void CommandRegistry::Register( std::string owner, Provider provider )
    {
        m_Providers.push_back( { std::move( owner ), std::move( provider ) } );
    }

    void CommandRegistry::Build( std::vector<PaletteCommand>& out ) const
    {
        for ( const Entry& entry : m_Providers )
            entry.Append( out );
    }
} // namespace Desert::Editor
