// BuildPool platform, Windows: the one place windows.h is included, plus UTF-8 <-> UTF-16 and error text.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <string_view>
#include <vector>

namespace buildpool::win
{
    std::wstring Widen( std::string_view text );
    std::string  Narrow( std::wstring_view text );
    // "<what>: <FormatMessage text> (error <code>)".
    std::string        ErrorText( std::string_view what, DWORD code );
    inline std::string LastErrorText( std::string_view what )
    {
        return ErrorText( what, GetLastError() );
    }

    // Owns one kernel handle; INVALID_HANDLE_VALUE and nullptr both mean "none".
    class Handle
    {
    public:
        Handle() noexcept = default;
        explicit Handle( HANDLE handle ) noexcept : m_handle( handle )
        {
        }
        Handle( Handle&& other ) noexcept : m_handle( other.Release() )
        {
        }
        Handle& operator=( Handle&& other ) noexcept
        {
            if ( this != &other )
            {
                Reset( other.Release() );
            }
            return *this;
        }
        Handle( const Handle& )            = delete;
        Handle& operator=( const Handle& ) = delete;
        ~Handle()
        {
            Reset();
        }

        HANDLE Get() const noexcept
        {
            return m_handle;
        }
        bool Valid() const noexcept
        {
            return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
        }
        HANDLE Release() noexcept
        {
            HANDLE handle = m_handle;
            m_handle      = nullptr;
            return handle;
        }
        void Reset( HANDLE handle = nullptr ) noexcept
        {
            if ( Valid() )
            {
                CloseHandle( m_handle );
            }
            m_handle = handle;
        }

    private:
        HANDLE m_handle = nullptr;
    };

    // A command line CommandLineToArgvW splits back into exactly `argv`.
    std::wstring QuoteCommandLine( const std::vector<std::string>& argv );
} // namespace buildpool::win
