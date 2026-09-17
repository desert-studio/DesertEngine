#pragma once

#include <Common/Core/Logger.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace Desert::Core
{
    /**
     * @brief PER-STAGE STARTUP TIMING, ONE FORMAT, FOR EVERY HOST THAT BOOTS.
     *
     * WHY IT IS A TYPE AND NOT A `chrono` PAIR IN EACH LAYER. The editor already had per-stage timing
     * and the comment above it records what it cost to get: a client watched a fresh editor sit for five
     * minutes with no way to say WHICH of eight stages was spending them, because the only startup line
     * the log carried belonged to a phase that was not one of the stages at all. The shipping runtime —
     * `Runtime/Source/RuntimeLayer.cpp`, thirteen preload calls and a scene load, in a flat sequence —
     * had none of it. That is the host the player runs, and §0.4 of the world programme names growth in
     * start-up time as one of four acceptance criteria, so the host we could not measure was the host
     * the criterion was about.
     *
     * Reproducing the editor's block in the runtime would have given two accumulation rules and two line
     * formats for one question, and the numbers would have been compared anyway. So the rule lives here
     * once: what a stage is, how the elapsed total is accumulated, and what the line says.
     *
     * ── THE ACCUMULATION RULE, WHICH IS NOT WALL CLOCK ───────────────────────────────────────────────
     *
     * `ElapsedMs()` is the SUM OF THE STAGES, not the time between the first and the last. The editor
     * runs one stage per frame, so wall clock between them also counts the frames in between — and the
     * question "which stage is spending the boot" is answered by the stages themselves. Keeping that
     * rule here means a caller that runs its stages back-to-back (the runtime does) and a caller that
     * spreads them over frames (the editor does) produce numbers that mean the same thing.
     */
    class BootTimeline final
    {
    public:
        struct Stage
        {
            std::string Label;
            double      Ms = 0.0;
        };

        /// Name the host, for the log prefix. `"Editor"`, `"Runtime"` — so two logs in one directory can
        /// be told apart, which a bare `[Startup]` could not do.
        explicit BootTimeline( std::string host ) : m_Host( std::move( host ) )
        {
        }

        /**
         * @brief Run `work` as a named stage: time it, record it, log one line.
         *
         * Returns whatever `work` returns, so a stage that can fail keeps its result and the caller's
         * control flow is unchanged — the timing must not be the reason an error stops being handled.
         */
        template <typename Work>
        decltype( auto ) Run( const std::string& label, Work&& work )
        {
            const auto started = std::chrono::steady_clock::now();
            if constexpr ( std::is_void_v<decltype( work() )> )
            {
                work();
                Record( label, Since( started ) );
            }
            else
            {
                decltype( auto ) result = work();
                Record( label, Since( started ) );
                return result;
            }
        }

        /**
         * @brief Record a stage somebody else timed.
         *
         * For the editor, whose stages run one per frame out of its own scheduler and cannot be wrapped
         * in a call here. It gets the same line and the same accumulation without giving up the
         * scheduler, which is the part of its arrangement that is not about timing.
         */
        void Record( const std::string& label, const double ms )
        {
            m_Stages.push_back( { label, ms } );
            m_ElapsedMs += ms;
            LOG_INFO( "[{}/Startup] stage {} '{}' took {:.1f} ms ({:.1f} ms into the boot)", m_Host,
                      m_Stages.size(), label, ms, m_ElapsedMs );
        }

        /// The closing line: every stage, slowest first, and the total. Logged by the caller when its
        /// boot is over — the type does not know when that is, and guessing would put the summary in the
        /// middle of a boot that had more to do.
        void LogSummary() const;

        [[nodiscard]] const std::vector<Stage>& Stages() const
        {
            return m_Stages;
        }

        /// Sum of the stages. See the accumulation note above for why this is not wall clock.
        [[nodiscard]] double ElapsedMs() const
        {
            return m_ElapsedMs;
        }

        /// The slowest stage, or an empty label when nothing ran. NOT a zero-length array and not a
        /// dereferenced `begin()`: a timeline with no stages is a legitimate state (a host that failed
        /// before its first stage), and it has to be able to say so.
        [[nodiscard]] Stage Slowest() const;

    private:
        static double Since( const std::chrono::steady_clock::time_point started )
        {
            return std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - started )
                 .count();
        }

        std::string        m_Host;
        std::vector<Stage> m_Stages;
        double             m_ElapsedMs = 0.0;
    };

} // namespace Desert::Core
