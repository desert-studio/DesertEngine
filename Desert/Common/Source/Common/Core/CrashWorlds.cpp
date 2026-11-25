#include "CrashWorlds.hpp"

#include <atomic>
#include <utility>

namespace Common::Crash
{
    namespace
    {
        constexpr std::size_t  kLineChars = 1024;
        constexpr std::size_t  kKindChars = 32;
        constexpr std::int32_t kFree      = -1;

        struct Slot
        {
            char                      kind[kKindChars]    = {};
            char                      text[2][kLineChars] = {};
            std::atomic<std::int32_t> published{ kFree };
        };

        // Process-wide on purpose: the fault handler finds the worlds with no pointer it could trust.
        Slot                      g_Slots[kCrashWorldSlots];
        std::atomic<std::int32_t> g_Unlisted{ 0 };

        // Appends at @p at, truncating; a line break would end the report's record, so it becomes a space.
        std::size_t Append( char* out, std::size_t capacity, std::size_t at, std::string_view value ) noexcept
        {
            for ( const char c : value )
            {
                if ( at + 1 >= capacity )
                    break;
                out[at++] = ( c == '\n' || c == '\r' ) ? ' ' : c;
            }
            out[at] = '\0';
            return at;
        }

        void Publish( Slot& slot, std::string_view path, std::string_view state ) noexcept
        {
            const std::int32_t next = slot.published.load( std::memory_order_relaxed ) == 0 ? 1 : 0;
            char*              line = slot.text[next];
            std::size_t        at   = Append( line, kLineChars, 0, slot.kind );
            at                      = Append( line, kLineChars, at, " " );
            at                      = Append( line, kLineChars, at, state );
            at                      = Append( line, kLineChars, at, " " );
            Append( line, kLineChars, at, path.empty() ? std::string_view( "(unsaved)" ) : path );
            slot.published.store( next, std::memory_order_release );
        }
    } // namespace

    CrashWorld::CrashWorld( std::string_view kind ) noexcept
    {
        for ( std::size_t i = 0; i < kCrashWorldSlots; ++i )
        {
            Slot& slot = g_Slots[i];
            if ( slot.published.load( std::memory_order_relaxed ) != kFree )
                continue;
            Append( slot.kind, kKindChars, 0, kind );
            Publish( slot, {}, "edit" );
            m_Slot = static_cast<std::int32_t>( i );
            return;
        }
        m_Slot = kUnlisted;
        g_Unlisted.fetch_add( 1, std::memory_order_relaxed );
    }

    CrashWorld::~CrashWorld() noexcept
    {
        Release();
    }

    CrashWorld::CrashWorld( CrashWorld&& other ) noexcept : m_Slot( std::exchange( other.m_Slot, -1 ) )
    {
    }

    CrashWorld& CrashWorld::operator=( CrashWorld&& other ) noexcept
    {
        if ( this != &other )
        {
            Release();
            m_Slot = std::exchange( other.m_Slot, -1 );
        }
        return *this;
    }

    void CrashWorld::Update( std::string_view path, std::string_view state ) noexcept
    {
        if ( m_Slot >= 0 )
            Publish( g_Slots[m_Slot], path, state );
    }

    void CrashWorld::Release() noexcept
    {
        if ( m_Slot >= 0 )
            g_Slots[m_Slot].published.store( kFree, std::memory_order_release );
        else if ( m_Slot == kUnlisted )
            g_Unlisted.fetch_sub( 1, std::memory_order_relaxed );
        m_Slot = -1;
    }

    const char* PublishedCrashWorld( std::size_t slot ) noexcept
    {
        const std::int32_t published = g_Slots[slot].published.load( std::memory_order_acquire );
        return published == kFree ? nullptr : g_Slots[slot].text[published];
    }

    std::int32_t UnlistedCrashWorlds() noexcept
    {
        return g_Unlisted.load( std::memory_order_relaxed );
    }
} // namespace Common::Crash
