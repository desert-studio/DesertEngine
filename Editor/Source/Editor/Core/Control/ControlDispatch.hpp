#pragma once

#include <Editor/Core/FuzzyMatch.hpp>
#include <Engine/Core/TextTemplate.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <vector>

namespace Desert::Editor::Control
{
    /**
     * @brief ADDRESSING A COMMAND, and nothing else.
     *
     * THE CHANNEL EXECUTES THE COMMAND PALETTE, NOT A LIST OF ITS OWN. That decision is the whole shape
     * of this subsystem, so it is worth writing down where the addressing lives.
     *
     * EditorLayer already fills a dictionary every frame with that frame's candidates — the tool panels,
     * the open documents, the entities in the open scene, the actions (BuildPaletteCommands). A person
     * reaches it with Ctrl+P; the channel reaches the SAME entries by naming a group and a label, and
     * calls the SAME std::function. So everything a person can do, an agent can do, by construction —
     * and a capability added for one arrives for the other with nobody wiring it up twice.
     *
     * The alternative on offer was a command table belonging to the channel. That is a second execution
     * path, and a second path is the defect this codebase spends its days removing: two sides that must
     * agree, one of which falls behind. When the palette turns out not to cover something the channel
     * needs, the palette is what grows — which is also how a person gets the missing capability.
     *
     * NOTHING HERE KNOWS WHAT A COMMAND DOES. It takes any range of things carrying a Group and a Label,
     * which is what makes the addressing rule assertable by a suite without an editor, an ImGui context
     * or a GPU — and it has to be, because EditorLayer.cpp is compiled by no suite at all.
     */

    /// How a request names a palette entry: exactly as the palette shows it.
    struct CommandAddress
    {
        std::string Group;
        std::string Label;

        [[nodiscard]] bool operator==( const CommandAddress& other ) const
        {
            return Group == other.Group && Label == other.Label;
        }
    };

    /// How many near misses a refusal offers. Enough to recognise the one that was meant, few enough
    /// that the message is read rather than scrolled past.
    inline constexpr std::size_t kMaxSuggestions = 6;

    /// The answer to "is this address in that dictionary".
    struct Resolution
    {
        bool        Found = false;
        std::size_t Index = 0; ///< position in the dictionary; meaningful only when Found

        /// Near misses, best first. Populated only when the address was NOT found — a refusal that
        /// merely says "no" leaves the caller guessing at spelling, at capitalisation and at whether the
        /// entry exists at all this frame, and an agent guessing is an agent retrying blind.
        std::vector<CommandAddress> Suggestions;

        /// How big the dictionary was. Part of the refusal because "no such command" reads very
        /// differently against 400 candidates than against 0 — the second means the editor is still
        /// loading, and that is a different problem with a different fix.
        std::size_t Candidates = 0;
    };

    /**
     * @brief Find @p wanted in @p dictionary, or come back with the nearest things to it.
     *
     * The match on the address itself is EXACT, on both halves. Fuzzy matching is what the suggestions
     * are for, and it must not decide what RUNS: "Delete Entity" is one edit away from "Delete Entities",
     * and a channel that helpfully ran the nearest thing would eventually run the wrong one unattended.
     * A person choosing from a ranked list is present to see what they picked; an agent is not.
     *
     * @param dictionary Any range whose elements have `.Group` and `.Label` (PaletteCommand does).
     */
    template <typename Range>
    [[nodiscard]] Resolution ResolveCommand( const Range& dictionary, const CommandAddress& wanted )
    {
        Resolution resolution;

        struct Scored
        {
            CommandAddress Address;
            int            Score = 0;
        };
        std::vector<Scored> nearMisses;

        std::size_t index = 0;
        for ( const auto& entry : dictionary )
        {
            ++resolution.Candidates;

            if ( entry.Group == wanted.Group && entry.Label == wanted.Label )
            {
                resolution.Found = true;
                resolution.Index = index;
                // No early return: Candidates is part of the answer and a caller reading it after a hit
                // should get the real size, not "however far we got".
            }

            if ( !resolution.Found )
            {
                int score = 0;
                if ( FuzzyMatch( wanted.Label, entry.Label, score ) )
                {
                    // Same group first. A label that matches inside the group that was asked for is
                    // almost always the entry meant — a typo in the label with the group right — while
                    // the same label in another group usually means a different thing entirely.
                    if ( entry.Group == wanted.Group )
                        score += 1000;
                    nearMisses.push_back( Scored{ CommandAddress{ entry.Group, entry.Label }, score } );
                }
            }

            ++index;
        }

        if ( resolution.Found )
            return resolution;

        std::stable_sort( nearMisses.begin(), nearMisses.end(),
                          []( const Scored& a, const Scored& b ) { return a.Score > b.Score; } );
        if ( nearMisses.size() > kMaxSuggestions )
            nearMisses.resize( kMaxSuggestions );

        resolution.Suggestions.reserve( nearMisses.size() );
        for ( auto& scored : nearMisses )
            resolution.Suggestions.push_back( std::move( scored.Address ) );

        return resolution;
    }

    /// The refusal, in words, for an address that resolved to nothing. Never empty: a request that did
    /// not run must come back saying so and saying why, or the client cannot tell it from one that did.
    [[nodiscard]] inline std::string DescribeUnknownCommand( const CommandAddress& wanted,
                                                             const Resolution&     resolution )
    {
        const std::string message = std::format( "no command '{}' / '{}' is offered right now ({} available)",
                                                 wanted.Group, wanted.Label, resolution.Candidates );

        if ( resolution.Candidates == 0 )
        {
            return std::format(
                 "{}. The dictionary is EMPTY, which is not the same as your command being "
                 "misspelled: the editor is still loading, or no project is open. Ask 'state' before "
                 "retrying.",
                 message );
        }

        if ( resolution.Suggestions.empty() )
            return std::format( "{}. Nothing resembles it either; ask 'commands' for the full list.", message );

        static const ::Desert::Core::TextTemplate kDidYouMean(
             "UnknownCommandSuggestions",
             "{{ message }}. Did you mean {% for s in suggestions %}{% if not loop.first %}, {% endif %}"
             "'{{ s.group }}' / '{{ s.label }}'{% endfor %}?" );
        Common::Json::Value::Array suggestions;
        suggestions.reserve( resolution.Suggestions.size() );
        for ( const CommandAddress& suggestion : resolution.Suggestions )
            suggestions.emplace_back( Common::Json::ObjectBuilder()
                                           .Set( "group", suggestion.Group )
                                           .Set( "label", suggestion.Label )
                                           .Build() );
        return kDidYouMean.RenderOrLog(
             Common::Json::Value( Common::Json::ObjectBuilder()
                                       .Set( "message", message )
                                       .Set( "suggestions", Common::Json::Value( std::move( suggestions ) ) )
                                       .Build() ) );
    }
} // namespace Desert::Editor::Control
