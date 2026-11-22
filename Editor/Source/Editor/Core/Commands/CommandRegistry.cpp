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

    namespace
    {
        using Clock = std::chrono::steady_clock;
        double Since( const Clock::time_point start )
        {
            return std::chrono::duration<double, std::milli>( Clock::now() - start ).count();
        }
        void SlowestFirst( std::vector<CommandRegistry::ProviderCost>* costs )
        {
            if ( costs )
                std::ranges::sort( *costs, []( const CommandRegistry::ProviderCost& a,
                                               const CommandRegistry::ProviderCost& b ) { return a.Ms > b.Ms; } );
        }
    } // namespace

    void CommandRegistry::RunPreludes( std::vector<ProviderCost>* costs ) const
    {
        const auto preludeStart = Clock::now();
        for ( const auto& prelude : m_Preludes )
            prelude();
        if ( costs )
            costs->push_back( { "preludes", Since( preludeStart ) } );
    }

    void CommandRegistry::Build( std::vector<PaletteCommand>& out, std::vector<ProviderCost>* costs ) const
    {
        RunPreludes( costs );
        // The index is re-learnt whole from every full Build: a provider that stopped producing a group drops out.
        m_GroupProviders.clear();
        for ( std::size_t provider = 0; provider < m_Providers.size(); ++provider )
        {
            const Entry&      entry = m_Providers[provider];
            const auto        start = Clock::now();
            const std::size_t first = out.size();
            entry.Append( out );
            if ( costs )
                costs->push_back( { entry.Owner, Since( start ) } );
            for ( std::size_t i = first; i < out.size(); ++i )
            {
                std::vector<std::size_t>& owners = m_GroupProviders[out[i].Group];
                if ( owners.empty() || owners.back() != provider )
                    owners.push_back( provider );
            }
        }
        SlowestFirst( costs );
    }

    bool CommandRegistry::BuildGroup( const std::string& group, std::vector<PaletteCommand>& out,
                                      std::vector<ProviderCost>* costs ) const
    {
        const auto known = m_GroupProviders.find( group );
        if ( known == m_GroupProviders.end() )
            return false;
        // Copied: a provider may not re-enter the registry, but the index must not be read through a reference
        // a nested full Build could clear.
        const std::vector<std::size_t> providers = known->second;
        RunPreludes( costs );
        for ( const std::size_t provider : providers )
        {
            const Entry& entry = m_Providers[provider];
            const auto   start = Clock::now();
            entry.Append( out );
            if ( costs )
                costs->push_back( { entry.Owner, Since( start ) } );
        }
        SlowestFirst( costs );
        return true;
    }
} // namespace Desert::Editor
