#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Common::Crash
{
    inline constexpr std::size_t kCrashWorldSlots = 16;

    // One live world's line in a crash report; owned by the world, move-only, game thread only. The fault handler
    // reads without a lock: a slot is two fixed buffers and an atomic names the published one, so a fault during
    // Update prints the previous text whole. A world past the last slot is counted ("worlds_not_listed").
    class CrashWorld
    {
    public:
        CrashWorld() = default;
        explicit CrashWorld( std::string_view kind ) noexcept;
        ~CrashWorld() noexcept;
        CrashWorld( CrashWorld&& other ) noexcept;
        CrashWorld& operator=( CrashWorld&& other ) noexcept;
        CrashWorld( const CrashWorld& )            = delete;
        CrashWorld& operator=( const CrashWorld& ) = delete;

        // @p path empty: an unsaved level. @p state: "edit", "play", "paused".
        void Update( std::string_view path, std::string_view state ) noexcept;

    private:
        void Release() noexcept;

        static constexpr std::int32_t kUnlisted = -2; // counted, has no slot
        std::int32_t                  m_Slot    = -1; // -1: not a world
    };

    // For the fault handler: no lock, no allocation. The slot's published line, null for a free slot.
    [[nodiscard]] const char*  PublishedCrashWorld( std::size_t slot ) noexcept;
    [[nodiscard]] std::int32_t UnlistedCrashWorlds() noexcept;
} // namespace Common::Crash
