#pragma once

#include <Common/Core/ResultStr.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Desert::Core
{
    // THE ONE WAY A GAME CHANGES LEVEL. Port of UE's pattern: UGameplayStatics::OpenLevel (GameplayStatics.cpp
    // :981) validates the map and calls UEngine::SetClientTravel, which only RECORDS the travel URL on the
    // world context; UEngine::TickWorldTravel performs it at the head of the next engine tick, never in the
    // middle of a frame (a level change tears down every entity, GPU resources included, so doing it from a
    // script callback, a UI walk or a physics contact would free what the frame being built still points
    // at). Everything that wants another level -- C++ gameplay, Lua `level.open(...)`, a UI button whose
    // action is LoadScene -- calls OpenLevel; the host applies the request with TickTravel between frames.
    //
    // The level is named the way the project names its DefaultScene: a path RELATIVE TO THE PROJECT FOLDER
    // (Content/Scenes/Arena.desce), or an absolute path. An EMPTY name is the project's default map, as an
    // empty FURL map is UGameMapsSettings::GetGameDefaultMap() in UE (URL.cpp:136). A name that does not
    // resolve to an existing file is REFUSED with the path that was tried -- the running level is untouched
    // and nothing is queued; there is no fallback map.
    [[nodiscard]] Common::ResultStr<std::string> ResolveLevelPath( std::string_view level );

    class LevelTravel
    {
    public:
        static LevelTravel& Get();

        // Resolve and queue. A second request before the boundary REPLACES the first (SetClientTravel
        // overwrites TravelURL): the last thing the game asked for in a frame is where it goes.
        [[nodiscard]] Common::BoolResultStr Open( std::string_view level );

        // The host's frame-boundary step (UEngine::TickWorldTravel): takes the queued travel, if any, and
        // hands its resolved path to `load`. A travel requested WHILE `load` runs (a new level's BeginPlay
        // script that immediately opens another) stays queued for the NEXT boundary rather than recursing.
        // Returns false when nothing was pending; the load's own error when the load failed.
        [[nodiscard]] Common::BoolResultStr
        TickTravel( const std::function<Common::BoolResultStr( const std::string& )>& load );

        [[nodiscard]] bool HasPending() const
        {
            return m_Pending.has_value();
        }
        [[nodiscard]] const std::optional<std::string>& Pending() const
        {
            return m_Pending;
        }

        // Drops a queued travel unapplied. For a host that ends the session the request belonged to (the
        // editor's Play stopping) -- a travel must not outlive the world that asked for it.
        void Cancel()
        {
            m_Pending.reset();
        }

    private:
        std::optional<std::string> m_Pending; // resolved path
    };

    // UGameplayStatics::OpenLevel.
    [[nodiscard]] inline Common::BoolResultStr OpenLevel( std::string_view level )
    {
        return LevelTravel::Get().Open( level );
    }
} // namespace Desert::Core
