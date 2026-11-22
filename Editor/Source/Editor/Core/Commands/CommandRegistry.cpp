#include "CommandRegistry.hpp"
#include <algorithm>
#include <chrono>

#include <functional>
#include <string>
#include <utility>
#include <vector>

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

    void CommandRegistry::Build( std::vector<PaletteCommand>& out, std::vector<ProviderCost>* costs ) const
    {
        using Clock     = std::chrono::steady_clock;
        const auto since = []( const Clock::time_point start )
        { return std::chrono::duration<double, std::milli>( Clock::now() - start ).count(); };
        const auto preludeStart = Clock::now();
        for ( const auto& prelude : m_Preludes )
            prelude();
        if ( costs )
            costs->push_back( { "preludes", since( preludeStart ) } );
        for ( const Entry& entry : m_Providers )
        {
            const auto start = Clock::now();
            entry.Append( out );
            if ( costs )
                costs->push_back( { entry.Owner, since( start ) } );
        }
        if ( costs )
            std::ranges::sort( *costs, []( const ProviderCost& a, const ProviderCost& b ) { return a.Ms > b.Ms; } );
    }
} // namespace Desert::Editor
