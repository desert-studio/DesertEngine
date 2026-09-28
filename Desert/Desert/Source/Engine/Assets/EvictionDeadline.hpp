#pragma once

#include <string>
#include <utility>

namespace Desert::Assets
{
    /**
     * @brief WHEN A REQUESTED EVICTION SWEEP RUNS: A FIXED NUMBER OF FRAMES AFTER THE FIRST REQUEST.
     *
     * The frames of delay exist because loading a level is not one event: the editor builds an empty scene and
     * initialises it, then deserialises the file into it and initialises it again, and those land in different
     * frames. A sweep fired on the first request sees a half-built world (measured: two sweeps 106 ms apart for
     * one scene change, the first seeing 17 roots and the second 5). Two frames cover the second Init.
     *
     * THE DEADLINE IS SET BY THE FIRST REQUEST AND LATER ONES DO NOT MOVE IT (WP13). It used to be re-armed by
     * every request, which is a debounce: fine for a scene change, wrong for world streaming — a flight that lets
     * a cell go every other frame would re-arm it for ever and never sweep, and the assets of every cell left
     * behind would stay resident for the whole flight. A request that arrives after the sweep has run arms the
     * next one, so nothing asked for is lost.
     *
     * Pure and header-only so the eviction suite holds the rule; AssetEvictionSchedule is its one owner.
     */
    class EvictionDeadline final
    {
    public:
        static constexpr int kFramesAfterFirstRequest = 2;

        /// Ask for a sweep. The FIRST reason wins: the request that started the sequence is the one a reader of
        /// the outcome is looking for.
        void Request( std::string why )
        {
            if ( m_FramesLeft > 0 )
                return;
            m_FramesLeft = kFramesAfterFirstRequest;
            m_Reason     = std::move( why );
        }

        /// Called once per frame. True on the frame the sweep is due; @p why then receives the reason and the
        /// deadline is cleared BEFORE the caller sweeps, so a sweep that throws cannot make every frame sweep.
        [[nodiscard]] bool Due( std::string& why )
        {
            if ( m_FramesLeft == 0 || --m_FramesLeft > 0 )
                return false;
            why = std::move( m_Reason );
            m_Reason.clear();
            return true;
        }

        [[nodiscard]] bool Pending() const noexcept
        {
            return m_FramesLeft > 0;
        }

    private:
        int         m_FramesLeft = 0; // 0 = nothing pending
        std::string m_Reason;
    };
} // namespace Desert::Assets
