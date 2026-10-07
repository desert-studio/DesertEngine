#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace Desert::Graphic::Render2D
{
    // ONE PREPARATION PER DRAW PER FRAME (RDG-FAULT1 C3b, UE's FMeshDrawCommand: built in InitViews, recorded
    // later, never rebuilt by the recorder).
    //
    // Render2D's setup (DeclareBindings) decides what every command of the frame's draw list draws with - the
    // pipeline, the executor, and for a UI material the entry UIMaterialCache::DrawableOrDefault PREPARED (row,
    // push, validated, or the default UI material in its place) - and declares one binding block per drawn
    // command. The exec (Flush) records exactly that list: it does not resolve, prepare or validate again, so the
    // block it opens and the thing it draws cannot come from two different decisions.
    //
    // Header-only and device-free so the suite can count the preparations (drawlist2d_test).
    template <class Prepared>
    class PreparedDraws final
    {
    public:
        struct Draw
        {
            uint32_t Command = 0; // index into the draw list's commands
            Prepared Value{};
        };

        // Prepare every command of @p commands ONCE: @p prepare( command ) -> std::optional<Prepared>, nullopt for
        // a command that draws nothing (it declares no block and records nothing). Replaces the previous list.
        template <class Commands, class PrepareFn>
        void Prepare( const Commands& commands, PrepareFn&& prepare )
        {
            m_Draws.clear();
            uint32_t index = 0;
            for ( const auto& command : commands )
            {
                std::optional<Prepared> value = prepare( command );
                if ( value )
                {
                    m_Draws.push_back( Draw{ index, std::move( *value ) } );
                }
                ++index;
            }
            m_Ready = true;
        }

        // Whether the setup prepared the current draw list. Flush refuses to record a list nobody prepared rather
        // than preparing it itself - that second preparation is what this type exists to remove.
        [[nodiscard]] bool Ready() const
        {
            return m_Ready;
        }

        // The prepared draws, in draw order: the n-th opens the n-th declared binding block.
        [[nodiscard]] const std::vector<Draw>& Draws() const
        {
            return m_Draws;
        }

        // Forget the frame's list (a new draw list begins, or the list was recorded).
        void Reset()
        {
            m_Draws.clear();
            m_Ready = false;
        }

    private:
        std::vector<Draw> m_Draws;
        bool              m_Ready = false;
    };
} // namespace Desert::Graphic::Render2D
