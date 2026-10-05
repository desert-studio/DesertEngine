#include "CommandRegistry.hpp"

#include <utility>

namespace Desert::Editor
{
    void CommandRegistry::Register( std::string owner, Provider provider )
    {
        m_Providers.push_back( { std::move( owner ), std::move( provider ) } );
    }

    void CommandRegistry::OnBuildBegin( std::function<void()> prelude )
    {
        m_Preludes.push_back( std::move( prelude ) );
    }

    void CommandRegistry::Build( std::vector<PaletteCommand>& out ) const
    {
        for ( const auto& prelude : m_Preludes )
            prelude();
        for ( const Entry& entry : m_Providers )
            entry.Append( out );
    }
} // namespace Desert::Editor
