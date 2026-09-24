#pragma once

// HOW FAR THE START IS, MEASURED IN WORK RATHER THAN IN STEPS — the model behind the splash's bar, its
// percentage and its two lines of text. Shaped after UE's FScopedSlowTask / FFeedbackContext:
//
//   * The start is a list of STAGES, each weighted by the amount of work it is — a count of items (shader
//     programs, textures, scene assets) times what one such item costs relative to the others. A stage
//     whose count is not known when the plan is made (the textures are counted only once the registry is
//     read) carries an estimate until `BeginStage` states the real count, before any of its work is done.
//   * Inside the running stage, `Step` names the item being worked on and how many are done: the second
//     line reads "Cooking texture T_Rock_Albedo (37 / 212)".
//   * The bar is the weighted sum: finished stages count whole, the running one by its done share.
//
// The bar NEVER moves back. A count stated at `BeginStage` that is larger than the estimate would shrink
// the share already shown; the model keeps the largest value it has handed out instead, and the next
// work done catches up with it. A bar that jumps back reads as a start that failed and restarted.
//
// Pure: time comes in as an argument, so a suite can pin a stage's reported duration exactly. The
// editor layer (EditorLayer::MakeSplashPlan) owns the clock, the splash and the log line.

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Desert::Editor::Splash
{
    /// What the splash draws, all of it derived from one `ProgressModel`.
    struct ProgressSnapshot
    {
        std::string Stage;        // first line: the stage running
        std::string Item;         // second line: the item inside it, with its n / N (may be empty)
        double      Fraction = 0; // [0, 1], never smaller than any value handed out before
    };

    /// A stage that has finished: what the log line says about it.
    struct StageTiming
    {
        std::string Name;
        std::size_t Units   = 0;
        double      Seconds = 0.0;
    };

    class ProgressModel
    {
    public:
        /// Adds a stage to the plan and returns its id. @p unitCost is what ONE item of this stage costs
        /// relative to the items of the other stages (a shader program compile against a texture load);
        /// @p units is how many items there are, or the best estimate while the real count is unknown.
        /// A stage of zero units weighs one unit: it still happens and still takes time.
        std::size_t AddStage( std::string name, const double unitCost, const std::size_t units )
        {
            m_Stages.push_back( { std::move( name ), unitCost, units, 0, false } );
            return m_Stages.size() - 1;
        }

        /// Starts @p stage (finishing the one running, if any, at @p nowSeconds). @p units, when given,
        /// replaces the plan's estimate with the real count.
        std::optional<StageTiming> BeginStage( const std::size_t stage, const double nowSeconds,
                                               const std::optional<std::size_t> units = std::nullopt )
        {
            std::optional<StageTiming> finished = EndStage( nowSeconds );
            if ( stage >= m_Stages.size() )
                return finished;
            Stage& s = m_Stages[stage];
            if ( units )
                s.Units = *units;
            s.Done      = 0;
            m_Running   = stage;
            m_Item      = {};
            m_StartedAt = nowSeconds;
            return finished;
        }

        /// The running stage is working on @p item, @p done of its items already finished (0-based: the
        /// first item is `Step( name, 0 )`). A step past the count is clamped, never overdrawn. @p units,
        /// when given, is the stage's count as the work itself states it (a list that grew since the plan).
        void Step( std::string item, const std::size_t done,
                   const std::optional<std::size_t> units = std::nullopt )
        {
            if ( !m_Running )
                return;
            Stage& s = m_Stages[*m_Running];
            if ( units )
                s.Units = *units;
            s.Done = std::min( done, s.Units );
            m_Item = std::move( item );
        }

        /// Finishes the running stage; its whole weight counts as done. Returns what the log reports.
        std::optional<StageTiming> EndStage( const double nowSeconds )
        {
            if ( !m_Running )
                return std::nullopt;
            Stage& s   = m_Stages[*m_Running];
            s.Finished = true;
            s.Done     = s.Units;
            m_Running.reset();
            m_Item = {};
            return StageTiming{ s.Name, s.Units, nowSeconds - m_StartedAt };
        }

        /// The whole start is over: every stage counts as done, whatever was skipped.
        std::optional<StageTiming> Finish( const double nowSeconds )
        {
            std::optional<StageTiming> finished = EndStage( nowSeconds );
            for ( Stage& s : m_Stages )
                s.Finished = true;
            return finished;
        }

        /// The weighted share of the start that is done, never smaller than a value returned before.
        [[nodiscard]] double Fraction()
        {
            double total = 0.0;
            double done  = 0.0;
            for ( const Stage& s : m_Stages )
            {
                const double weight = Weight( s );
                total += weight;
                if ( s.Finished )
                    done += weight;
                else if ( s.Units > 0 )
                    done += weight * static_cast<double>( s.Done ) / static_cast<double>( s.Units );
            }
            const double now = total > 0.0 ? std::clamp( done / total, 0.0, 1.0 ) : 0.0;
            m_Shown          = std::max( m_Shown, now );
            return m_Shown;
        }

        [[nodiscard]] ProgressSnapshot Snapshot()
        {
            ProgressSnapshot snapshot;
            snapshot.Fraction = Fraction();
            if ( m_Running )
            {
                const Stage& s = m_Stages[*m_Running];
                snapshot.Stage = s.Name;
                if ( !m_Item.empty() )
                    snapshot.Item = FormatItem( m_Item, s.Done, s.Units );
            }
            return snapshot;
        }

        /// "T_Rock_Albedo (37 / 212)": the item being worked on is number done+1, so the last one reads
        /// N / N. A stage of one item names it without a counter — "(1 / 1)" says nothing.
        [[nodiscard]] static std::string FormatItem( const std::string& item, const std::size_t done,
                                                     const std::size_t units )
        {
            if ( units <= 1 )
                return item;
            const std::size_t current = std::min( done + 1, units );
            return item + " (" + std::to_string( current ) + " / " + std::to_string( units ) + ")";
        }

        [[nodiscard]] std::size_t StageCount() const
        {
            return m_Stages.size();
        }

    private:
        struct Stage
        {
            std::string Name;
            double      UnitCost = 1.0;
            std::size_t Units    = 0;
            std::size_t Done     = 0;
            bool        Finished = false;
        };

        [[nodiscard]] static double Weight( const Stage& s )
        {
            return s.UnitCost * static_cast<double>( std::max<std::size_t>( s.Units, 1 ) );
        }

        std::vector<Stage>         m_Stages;
        std::optional<std::size_t> m_Running;
        std::string                m_Item;
        double                     m_StartedAt = 0.0;
        double                     m_Shown     = 0.0;
    };

    /// "47%" — whole percent, rounded down so 100 % appears only when everything is done.
    [[nodiscard]] inline std::string FormatPercent( const double fraction )
    {
        const double clamped = std::clamp( fraction, 0.0, 1.0 );
        return std::to_string( static_cast<int>( clamped * 100.0 ) ) + "%";
    }
} // namespace Desert::Editor::Splash
