#include "BootTimeline.hpp"

#include <algorithm>

namespace Desert::Core
{
    BootTimeline::Stage BootTimeline::Slowest() const
    {
        Stage worst;
        for ( const Stage& stage : m_Stages )
        {
            if ( stage.Ms > worst.Ms )
                worst = stage;
        }
        return worst;
    }

    void BootTimeline::LogSummary() const
    {
        if ( m_Stages.empty() )
        {
            // A BOOT WITH NO STAGES IS REPORTED AS SUCH. "0 stages in 0.0 ms" reads like a boot that was
            // free; a host that died before its first stage is the case that actually produces this, and
            // it is the one worth noticing.
            LOG_WARN( "[{}/Startup] no stage ran. Either the host was torn down before its boot began or "
                      "nobody wrapped the boot in stages.",
                      m_Host );
            return;
        }

        std::vector<Stage> ordered = m_Stages;
        std::sort( ordered.begin(), ordered.end(),
                   []( const Stage& a, const Stage& b ) { return a.Ms > b.Ms; } );

        std::string lines;
        for ( const Stage& stage : ordered )
        {
            // The share, because the absolute number alone never answered the question anybody asked.
            // The editor's own experience is the precedent: eight stages costing 6.0 s of a 51 s boot
            // looked like the whole boot until somebody divided.
            const double share = m_ElapsedMs > 0.0 ? 100.0 * stage.Ms / m_ElapsedMs : 0.0;
            lines += fmt::format( "\n  {:>9.1f} ms  {:>5.1f}%  {}", stage.Ms, share, stage.Label );
        }

        LOG_INFO( "[{}/Startup] {} stage(s) in {:.1f} ms, slowest first:{}", m_Host, m_Stages.size(),
                  m_ElapsedMs, lines );
    }

} // namespace Desert::Core
