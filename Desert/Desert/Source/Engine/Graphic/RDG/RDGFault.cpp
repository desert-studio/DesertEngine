#include <Engine/Graphic/RDG/RDGFault.hpp>

#include <algorithm>
#include <format>
#include <iterator>
#include <set>

namespace Desert::Graphic::RDG
{
    namespace
    {
        // One shape per kind of line, so the Logs panel's repeat collapse and a reader's grep see one form.
        constexpr std::string_view kFaultFormat     = "RDG graph '{}' pass '{}' {}: {}";
        constexpr std::string_view kCascadeFormat   = "; culled with it: {}";
        constexpr std::string_view kRecoveredFormat = "RDG graph '{}' pass '{}' recovered";
        constexpr std::string_view kRemovedFormat   = "RDG graph '{}' pass '{}' is no longer added; its fault is "
                                                      "forgotten";
        constexpr std::string_view kFramePass       = "<frame>";
        constexpr std::string_view kFrameStage      = "frame";
    } // namespace

    std::string_view GetPassFaultStageName( PassFaultStage stage )
    {
        switch ( stage )
        {
            case PassFaultStage::Declaration:
                return "declaration";
            case PassFaultStage::Validation:
                return "validation";
            case PassFaultStage::Execution:
                return "execution";
            case PassFaultStage::Dependency:
                return "dependency";
        }
        return "unknown";
    }

    PassFaultReporter::PassFaultReporter( Sink sink ) : m_Sink( std::move( sink ) )
    {
    }

    uint32_t PassFaultReporter::Report( std::string_view graph, std::span<const std::string_view> addedPasses,
                                        const ExecuteReport& report )
    {
        uint32_t   lines = 0;
        const auto emit  = [&]( Severity severity, std::string_view line )
        {
            m_Sink( severity, line );
            ++lines;
        };
        // Logs @p text under (graph, pass) unless that exact key already said it. The remembered value is the
        // stage and reason only: a cascade that grows by a pass is the same defect, not news.
        std::set<std::string, std::less<>> faultedNow;
        const auto say = [&]( std::string_view pass, std::string_view stage, std::string_view reason,
                              std::string_view cascade )
        {
            faultedNow.emplace( pass );
            std::string key = std::format( "{}: {}", stage, reason );
            auto        it  = m_Active.find( std::make_pair( std::string( graph ), std::string( pass ) ) );
            if ( it != m_Active.end() && it->second == key )
                return;
            std::string line = std::format( kFaultFormat, graph, pass, stage, reason );
            line += cascade;
            emit( Severity::Error, line );
            if ( it != m_Active.end() )
                it->second = std::move( key );
            else
                m_Active.emplace( std::make_pair( std::string( graph ), std::string( pass ) ), std::move( key ) );
        };

        if ( report.Frame )
        {
            // The frame line names its root passes in its reason: one line for the whole frame, and the pass keys
            // already remembered stay as they are (this execute drew nothing, so it proves no recovery).
            say( kFramePass, kFrameStage, report.Frame->Reason, {} );
            m_LinesLogged += lines;
            return lines;
        }

        for ( const PassFault& fault : report.Faults )
        {
            const bool rooted =
                 fault.Stage == PassFaultStage::Dependency && fault.RootPass &&
                 std::any_of( report.Faults.begin(), report.Faults.end(), [&]( const PassFault& root )
                              { return root.Pass == *fault.RootPass && root.Stage != PassFaultStage::Dependency; } );
            if ( rooted )
                continue; // said in its root's line
            std::string names;
            for ( const PassFault& dependant : report.Faults )
            {
                if ( dependant.Stage != PassFaultStage::Dependency || dependant.RootPass != fault.Pass ||
                     dependant.Pass == fault.Pass )
                    continue;
                std::format_to( std::back_inserter( names ), "{}'{}'", names.empty() ? "" : ", ",
                                dependant.PassName );
            }
            const std::string cascade = names.empty() ? std::string() : std::format( kCascadeFormat, names );
            say( fault.PassName, GetPassFaultStageName( fault.Stage ), fault.Reason, cascade );
        }

        // Recovery is judged per graph: only this graph's keys, and only those this execute did not fault.
        for ( auto it = m_Active.begin(); it != m_Active.end(); )
        {
            const auto& [key, reason] = *it;
            if ( key.first != graph || faultedNow.contains( key.second ) )
            {
                ++it;
                continue;
            }
            const bool added = key.second == kFramePass ||
                               std::find( addedPasses.begin(), addedPasses.end(), key.second ) != addedPasses.end();
            emit( Severity::Recovered, added ? std::format( kRecoveredFormat, graph, key.second )
                                             : std::format( kRemovedFormat, graph, key.second ) );
            it = m_Active.erase( it );
        }
        m_LinesLogged += lines;
        return lines;
    }

    uint32_t PassFaultReporter::GetActiveCount() const
    {
        return static_cast<uint32_t>( m_Active.size() );
    }

    uint32_t PassFaultReporter::GetLinesLogged() const
    {
        return m_LinesLogged;
    }
} // namespace Desert::Graphic::RDG
