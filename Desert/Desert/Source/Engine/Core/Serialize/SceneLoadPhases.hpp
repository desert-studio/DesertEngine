#pragma once

#include <Common/Core/Logger.hpp>

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace Desert::Core
{
    /**
     * @brief PER-PHASE TIMING OF ONE SCENE LOAD: a line per phase with its milliseconds and how many records
     * it walked, and a closing summary, slowest first.
     *
     * WHY IT EXISTS. A 50 000-record world was reported to open in minutes and nobody could say which part
     * of the load was spending them - the only line the load printed was "Loading scene", before any of
     * the work. A guess at the phase is the defect this tool exists to prevent: the cost is rarely where
     * the name says, and the item count next to the milliseconds is what separates "this phase is slow per
     * record" from "this phase is quadratic" (double the records, look at the ratio).
     *
     * WHY NOT BootTimeline. That type answers "which stage of the boot", its lines say Startup and its
     * stages carry no count; a load is repeated many times per session (open, reopen, Stop restore) and
     * each run is its own timeline. The accumulation rule is the same: the total is the SUM OF THE PHASES,
     * so a gap between phases is visible as a total below the wall clock rather than folded into a phase.
     */
    class SceneLoadPhases final
    {
    public:
        struct Phase
        {
            std::string Label;
            double      Ms    = 0.0;
            std::size_t Items = 0;
        };

        /// `what` names the load in every line: the host step ("Open", "Stop restore") and the source.
        explicit SceneLoadPhases( std::string what ) : m_What( std::move( what ) )
        {
        }

        /// Time `work` as one phase. The count is what the phase walked, stated by the caller who knows it.
        template <typename Work>
        decltype( auto ) Run( const std::string& label, const std::size_t items, Work&& work )
        {
            const auto started = std::chrono::steady_clock::now();
            if constexpr ( std::is_void_v<decltype( work() )> )
            {
                work();
                Record( label, items, Since( started ) );
            }
            else
            {
                decltype( auto ) result = work();
                Record( label, items, Since( started ) );
                return result;
            }
        }

        /// Close a phase that began where the previous one ended (or at construction): the form for a sequence
        /// of passes in one function, where wrapping each pass in a lambda would re-indent the whole body.
        void Lap( const std::string& label, const std::size_t items )
        {
            Record( label, items, Since( m_Mark ) );
        }

        void Record( const std::string& label, const std::size_t items, const double ms )
        {
            m_Phases.push_back( { label, ms, items } );
            m_TotalMs += ms;
            LOG_INFO( "[SceneLoad] {} - phase {} '{}': {} item(s) in {:.1f} ms", m_What, m_Phases.size(), label,
                      items, ms );
            // After the line, so the next lap does not carry the cost of printing this one.
            m_Mark = std::chrono::steady_clock::now();
        }

        /// The closing line, slowest first with each phase's share of the total.
        void LogSummary() const;

        [[nodiscard]] const std::vector<Phase>& Phases() const
        {
            return m_Phases;
        }

        [[nodiscard]] double TotalMs() const
        {
            return m_TotalMs;
        }

    private:
        static double Since( const std::chrono::steady_clock::time_point started )
        {
            return std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - started ).count();
        }

        std::string                           m_What;
        std::chrono::steady_clock::time_point m_Mark = std::chrono::steady_clock::now();
        std::vector<Phase>                    m_Phases;
        double                                m_TotalMs = 0.0;
    };

} // namespace Desert::Core
