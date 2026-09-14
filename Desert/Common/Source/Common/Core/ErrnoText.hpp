#pragma once

// WHAT errno MEANS, AS A STRING YOU OWN.
//
// `std::strerror` returns a pointer into a buffer the C library owns and may reuse. Two calls in one
// expression — `LOG_ERROR( "{} then {}", std::strerror( a ), std::strerror( b ) )` — can therefore print
// one message twice, and a call from another thread can rewrite a string this one is still formatting.
// Neither failure shows up as a crash: it shows up as a log line that names the wrong error, which is
// the single most expensive kind of wrong answer a diagnostic can give.
//
// `strerror_r` writes into a caller-supplied buffer and has no shared state. It has two incompatible
// signatures — POSIX returns int, GNU returns char* — and Windows spells it `strerror_s`, so the
// selection lives here once rather than at each call site.

#include <cerrno>
#include <cstring>
#include <string>

namespace Common
{
    /// The message for @p code (default: the current `errno`), as an owned string.
    inline std::string ErrnoText( int code = errno )
    {
        char buffer[256] = { 0 };

#if defined( _WIN32 )
        if ( strerror_s( buffer, sizeof( buffer ), code ) != 0 )
            return "errno " + std::to_string( code );
        return buffer;
#elif defined( __GLIBC__ ) && defined( _GNU_SOURCE )
        // The GNU variant may answer in its own static storage instead of the buffer, and returns that
        // pointer; ignoring the return value there is how a caller gets an empty string.
        const char* text = strerror_r( code, buffer, sizeof( buffer ) );
        return text != nullptr ? std::string( text ) : "errno " + std::to_string( code );
#else
        if ( strerror_r( code, buffer, sizeof( buffer ) ) != 0 )
        {
            return "errno " + std::to_string( code );
        }
        return buffer;
#endif
    }
} // namespace Common
