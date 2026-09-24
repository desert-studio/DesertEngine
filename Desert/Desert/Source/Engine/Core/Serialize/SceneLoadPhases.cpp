#include "SceneLoadPhases.hpp"

#include <algorithm>

namespace Desert::Core
{
    void SceneLoadPhases::LogSummary() const
    {
        if ( m_Phases.empty() )
        {
            // Said, not skipped: a load that timed nothing either refused before its first phase or was not
            // wrapped in phases, and a silent summary would read like a free load.
            LOG_WARN( "[SceneLoad] {} - no phase ran.", m_What );
            return;
        }

        std::vector<Phase> ordered = m_Phases;
        std::stable_sort( ordered.begin(), ordered.end(),
                          []( const Phase& a, const Phase& b ) { return a.Ms > b.Ms; } );

        std::string lines;
        for ( const Phase& phase : ordered )
        {
            const double share = m_TotalMs > 0.0 ? 100.0 * phase.Ms / m_TotalMs : 0.0;
            lines +=
                 fmt::format( "\n  {:>9.1f} ms  {:>5.1f}%  {:>8}  {}", phase.Ms, share, phase.Items, phase.Label );
        }

        LOG_INFO( "[SceneLoad] {} - {} phase(s) in {:.1f} ms, slowest first (ms, share, items, phase):{}", m_What,
                  m_Phases.size(), m_TotalMs, lines );
    }

} // namespace Desert::Core
