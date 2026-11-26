#include "FunctionHotpatch.hpp"

#include <Windows.h>
#include <TlHelp32.h>

#include <cstring>
#include <vector>

namespace Desert::Editor::LiveCoding
{
    namespace
    {
        constexpr std::uint8_t kJmpRel32    = 0xE9;
        constexpr std::uint8_t kShortJmp[2] = { 0xEB, 0xF9 }; // jmp -7: from entry+2 back to entry-5
        constexpr int          kPadding     = 5;
        constexpr int          kAttempts    = 50;

        bool IsPadding( const std::uint8_t* bytes )
        {
            for ( int i = 0; i < kPadding; ++i )
            {
                if ( bytes[i] != 0xCC && bytes[i] != 0x90 )
                {
                    return false;
                }
            }
            return true;
        }

        bool IsEarlierRedirect( const std::uint8_t* entry )
        {
            return entry[-kPadding] == kJmpRel32 && entry[0] == kShortJmp[0] && entry[1] == kShortJmp[1];
        }

        /// The pages a redirect writes, made writable for the write and put back after.
        struct PageGrant
        {
            void* Begin    = nullptr;
            DWORD Previous = 0;
        };

        void SuspendAll( const std::vector<HANDLE>& threads, std::vector<bool>& suspended )
        {
            for ( std::size_t i = 0; i < threads.size(); ++i )
            {
                suspended[i] = SuspendThread( threads[i] ) != DWORD( -1 );
            }
        }

        void ResumeAll( const std::vector<HANDLE>& threads, const std::vector<bool>& suspended )
        {
            for ( std::size_t i = 0; i < threads.size(); ++i )
            {
                if ( suspended[i] )
                {
                    ResumeThread( threads[i] );
                }
            }
        }
    } // namespace

    Common::BoolResultStr ApplyRedirects( std::span<const Redirect> redirects ) noexcept
    {
        if ( redirects.empty() )
        {
            return Common::MakeSuccess( true );
        }

        // Everything that allocates happens here, before any thread is suspended: a suspended thread may hold the
        // heap lock, and this thread would then wait on it forever.
        std::vector<std::int32_t> displacement( redirects.size() );
        for ( std::size_t i = 0; i < redirects.size(); ++i )
        {
            const Redirect& redirect = redirects[i];
            const auto*     entry    = reinterpret_cast<const std::uint8_t*>( redirect.Original );
            if ( !IsPadding( entry - kPadding ) && !IsEarlierRedirect( entry ) )
            {
                return Common::MakeFormattedError<bool>(
                     "{} has no hotpatch padding before its entry (the image was linked without /FUNCTIONPADMIN)",
                     redirect.Name );
            }
            const std::int64_t distance = std::int64_t( redirect.Replacement ) - std::int64_t( redirect.Original );
            if ( distance < INT32_MIN || distance > INT32_MAX )
            {
                return Common::MakeFormattedError<bool>( "{}: the patch is more than 2 GB from the image",
                                                         redirect.Name );
            }
            displacement[i] = std::int32_t( distance );
        }

        std::vector<PageGrant> grants;
        grants.reserve( redirects.size() * 2 );
        SYSTEM_INFO system{};
        GetSystemInfo( &system );
        const std::uintptr_t pageMask = ~std::uintptr_t( system.dwPageSize - 1 );
        const auto           grant    = [&]( std::uintptr_t address )
        {
            void* page = reinterpret_cast<void*>( address & pageMask );
            for ( const PageGrant& existing : grants )
            {
                if ( existing.Begin == page )
                {
                    return true;
                }
            }
            DWORD previous = 0;
            if ( !VirtualProtect( page, system.dwPageSize, PAGE_EXECUTE_READWRITE, &previous ) )
            {
                return false;
            }
            grants.push_back( PageGrant{ page, previous } );
            return true;
        };
        const auto revoke = [&]()
        {
            for ( const PageGrant& page : grants )
            {
                DWORD ignored = 0;
                VirtualProtect( page.Begin, system.dwPageSize, page.Previous, &ignored );
            }
        };
        for ( const Redirect& redirect : redirects )
        {
            if ( !grant( redirect.Original - kPadding ) || !grant( redirect.Original + 1 ) )
            {
                const DWORD error = GetLastError();
                revoke();
                return Common::MakeFormattedError<bool>( "VirtualProtect refused the entry of {} ({})",
                                                         redirect.Name, error );
            }
        }

        std::vector<HANDLE> threads;
        {
            const DWORD  process  = GetCurrentProcessId();
            const DWORD  self     = GetCurrentThreadId();
            const HANDLE snapshot = CreateToolhelp32Snapshot( TH32CS_SNAPTHREAD, 0 );
            if ( snapshot == INVALID_HANDLE_VALUE )
            {
                revoke();
                return Common::MakeFormattedError<bool>( "CreateToolhelp32Snapshot failed ({})", GetLastError() );
            }
            THREADENTRY32 entry{};
            entry.dwSize = sizeof( entry );
            for ( BOOL more = Thread32First( snapshot, &entry ); more; more = Thread32Next( snapshot, &entry ) )
            {
                if ( entry.th32OwnerProcessID != process || entry.th32ThreadID == self )
                {
                    continue;
                }
                const HANDLE thread =
                     OpenThread( THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, entry.th32ThreadID );
                if ( thread != nullptr )
                {
                    threads.push_back( thread );
                }
            }
            CloseHandle( snapshot );
        }
        std::vector<bool> suspended( threads.size(), false );
        const auto        closeThreads = [&]()
        {
            for ( HANDLE thread : threads )
            {
                CloseHandle( thread );
            }
        };

        // Suspend everyone; a thread stopped inside the 7 bytes about to change would resume into a torn
        // instruction, so then everyone is resumed and the whole set retried a millisecond later.
        bool clear = false;
        for ( int attempt = 0; attempt < kAttempts && !clear; ++attempt )
        {
            SuspendAll( threads, suspended );
            clear = true;
            for ( std::size_t t = 0; t < threads.size() && clear; ++t )
            {
                if ( !suspended[t] )
                {
                    continue;
                }
                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL;
                if ( !GetThreadContext( threads[t], &context ) )
                {
                    continue; // the thread is exiting
                }
                for ( const Redirect& redirect : redirects )
                {
                    if ( context.Rip >= redirect.Original - kPadding && context.Rip < redirect.Original + 2 )
                    {
                        clear = false;
                        break;
                    }
                }
            }
            if ( !clear )
            {
                ResumeAll( threads, suspended );
                Sleep( 1 );
            }
        }
        if ( !clear )
        {
            closeThreads();
            revoke();
            return Common::MakeFormattedError<bool>(
                 "a thread stayed on a patched entry through {} attempts; nothing was redirected", kAttempts );
        }

        for ( std::size_t i = 0; i < redirects.size(); ++i )
        {
            auto* entry = reinterpret_cast<std::uint8_t*>( redirects[i].Original );
            // The jump into the patch first, in the padding nothing executes; then the entry, two bytes at once.
            std::uint8_t jump[kPadding] = { kJmpRel32 };
            std::memcpy( jump + 1, &displacement[i], sizeof( std::int32_t ) );
            std::memcpy( entry - kPadding, jump, kPadding );
            std::uint16_t shortJump = 0;
            std::memcpy( &shortJump, kShortJmp, sizeof( shortJump ) );
            if ( ( redirects[i].Original & 1 ) == 0 )
            {
                InterlockedExchange16( reinterpret_cast<volatile SHORT*>( entry ), SHORT( shortJump ) );
            }
            else
            {
                std::memcpy( entry, &shortJump, sizeof( shortJump ) ); // every other thread is suspended
            }
            FlushInstructionCache( GetCurrentProcess(), entry - kPadding, kPadding + 2 );
        }

        ResumeAll( threads, suspended );
        closeThreads();
        revoke();
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Editor::LiveCoding
