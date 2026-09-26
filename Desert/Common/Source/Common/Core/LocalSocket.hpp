#pragma once

// A LOCAL SOCKET ON EVERY PLATFORM THE EDITOR RUNS ON — the DIFFERENCES between them, and nothing else.
//
// IN Common BECAUSE IT HAS TWO CONSUMERS AND ONE OF THEM IS NOT THE EDITOR. The control channel's server
// lives in Editor/Source/Editor/Core/Control; its client is Tools/DesertCtl, a program that links neither
// the editor nor anything else, on purpose. Both open the same kind of socket, and the platform differences
// below were written twice before this header existed. ONLY THE TRANSPORT IS HERE: the protocol, the
// framing and the command set stay in Editor/Core/Control, where the editor is the only thing that needs
// them. Header-only, so nothing links a socket library for including it — the shipped game pulls in neither
// consumer and must not gain Winsock (see the ShippingBoundary suite).
//
// The control channel is a unix-domain socket rather than a port on localhost: "listens only on localhost"
// is a promise about a configuration, a filesystem socket is a promise about the kernel. Windows 10 1803
// and later has AF_UNIX too (afunix.h over Winsock), so the transport there is the SAME transport, and what
// differs is a short list: the handle type, the value that means "no socket", how one is closed, how it is
// made non-blocking, what the last error says, and how the socket FILE is restricted to its owner.
//
// NOT A SOCKET WRAPPER. socket, bind, listen, accept, connect, send and recv are spelled identically on
// both and are called directly by the two ends of the channel; wrapping them would add a layer whose whole
// content is a forwarding call, and would hide from the reader that this is ordinary Berkeley-socket code.
// What lives here is what is genuinely not the same.
//
// The guard is `_WIN32` and not DESERT_PLATFORM_WINDOWS on purpose: the compiler always defines it, while
// the project define has to be remembered by every premake file that reaches this header — and the failure
// mode of forgetting it here is a Windows build that compiles the POSIX branch. Common/Core/ErrnoText.hpp
// chose it first, for the same reason.
//
// Header-only, and dependency-free beyond ErrnoText.hpp: DesertCtl includes this and deliberately links no
// engine at all, so that it still builds on a machine that cannot build a renderer.

#include <Common/Core/ErrnoText.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#if defined( _WIN32 )
// winsock2.h BEFORE windows.h: windows.h includes the original winsock.h, whose declarations then collide
// with winsock2's (WSADATA redefined, sockaddr_in declared twice). afunix.h is what brings sockaddr_un, and
// it needs winsock2's types to already be in scope. aclapi.h is the DACL written in RestrictToOwner.
#include <winsock2.h>

#include <afunix.h>
#include <windows.h>

#include <aclapi.h>

#include <vector>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace Common::LocalSocket
{
#if defined( _WIN32 )
    /// What the platform's own calls take. `SOCKET` is a UINT_PTR, not a descriptor.
    using Native = SOCKET;
#else
    using Native = int;
#endif

    /// A socket as the channel's two ends carry it around, and NOT `Native`. `SOCKET` is unsigned on
    /// Windows, so `handle >= 0` — the check every one of these call sites writes — is true for
    /// INVALID_SOCKET as well, and a closed socket would read as a live one. A signed type wide enough to
    /// hold either platform's handle, with exactly one invalid value, keeps that idiom correct on both.
    using Handle = std::intptr_t;

    inline constexpr Handle kInvalid = -1;

    [[nodiscard]] inline Native Raw( Handle handle ) noexcept
    {
        return static_cast<Native>( handle );
    }

    namespace Detail
    {
        /// The text for a Win32 or Winsock code. Both live in the same numbering as far as FormatMessage is
        /// concerned, which is why one function answers for both.
        [[nodiscard]] inline std::string SystemErrorText( unsigned long code )
        {
#if defined( _WIN32 )
            // THE WIDE CALL AND THEN UTF-8, not FormatMessageA. The narrow one answers in the machine's ANSI
            // codepage, and this codebase's narrow strings are UTF-8 everywhere (/utf-8 is set workspace-wide
            // in BuildScripts/PlatformWindows.lua) -- the log, the channel's JSON replies and desertctl's
            // stderr all read as UTF-8. Measured on this machine, whose messages are Russian: `desertctl`
            // against a dead socket printed "?????????? ?? ???????????" where the reason should have been.
            // The number survived that, which is why it is always appended, but a diagnostic nobody can read
            // is most of a diagnostic wasted.
            wchar_t*    wide    = nullptr;
            const DWORD written = ::FormatMessageW(
                 FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, static_cast<DWORD>( code ), MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),
                 reinterpret_cast<LPWSTR>( &wide ), 0, nullptr );

            std::string message;
            if ( written > 0 && wide != nullptr )
            {
                const int bytes = ::WideCharToMultiByte( CP_UTF8, 0, wide, static_cast<int>( written ), nullptr, 0,
                                                         nullptr, nullptr );
                if ( bytes > 0 )
                {
                    message.resize( static_cast<std::size_t>( bytes ) );
                    (void)::WideCharToMultiByte( CP_UTF8, 0, wide, static_cast<int>( written ), message.data(),
                                                 bytes, nullptr, nullptr );
                }
            }
            if ( wide != nullptr )
                ::LocalFree( wide );

            // FormatMessage ends its sentences with ".\r\n", which reads badly inside one of ours.
            while ( !message.empty() && ( message.back() == '\n' || message.back() == '\r' ||
                                          message.back() == '.' || message.back() == ' ' ) )
                message.pop_back();

            // THE NUMBER IS ALWAYS THERE, even when the sentence is: a code is searchable and a translated
            // message is not, and this machine's messages are not necessarily in English.
            if ( message.empty() )
                return "system error " + std::to_string( code );
            return message + " (system error " + std::to_string( code ) + ")";
#else
            return ErrnoText( static_cast<int>( code ) );
#endif
        }
    } // namespace Detail

    /// What went wrong with the last socket call on this thread.
    [[nodiscard]] inline std::string LastErrorText()
    {
#if defined( _WIN32 )
        // Winsock keeps its own last error, NOT errno: strerror on a value like 10035 answers with
        // something about an unknown error, which is the kind of diagnostic that sends a reader to the
        // wrong place entirely.
        return Detail::SystemErrorText( static_cast<unsigned long>( ::WSAGetLastError() ) );
#else
        return ErrnoText();
#endif
    }

    /// Did the last call fail only because a non-blocking socket had nothing to do? Its own question
    /// because this is the one failure that is not a failure — it means "try again next frame".
    [[nodiscard]] inline bool LastErrorIsWouldBlock()
    {
#if defined( _WIN32 )
        return ::WSAGetLastError() == WSAEWOULDBLOCK;
#else
        return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
    }

    /// Was the last call cut short by a signal, so that repeating it is the right answer?
    [[nodiscard]] inline bool LastErrorIsInterrupted()
    {
#if defined( _WIN32 )
        return ::WSAGetLastError() == WSAEINTR;
#else
        return errno == EINTR;
#endif
    }

    /// Winsock has to be started before the first socket call in a PROCESS. Empty on success, the reason
    /// otherwise; every entry point that opens a socket calls this first, and it is safe to call again.
    ///
    /// NO WSACleanup. It is reference-counted per process and the last call invalidates EVERY socket in it,
    /// including ones another subsystem still holds — spdlog's remote sinks and Optick's server both open
    /// their own. Process exit releases the same resources with none of that risk.
    [[nodiscard]] inline std::string EnsureLibraryReady()
    {
#if defined( _WIN32 )
        // A function-local static is initialised exactly once, and the standard makes that thread-safe.
        static const int failure = []
        {
            WSADATA data{};
            return ::WSAStartup( MAKEWORD( 2, 2 ), &data );
        }();

        if ( failure != 0 )
        {
            // WSAStartup answers with the code itself rather than through WSAGetLastError — which cannot
            // be asked before the library is up.
            return "WSAStartup failed: " + Detail::SystemErrorText( static_cast<unsigned long>( failure ) );
        }
#endif
        return {};
    }

    /// A stream socket in the unix domain, or kInvalid having set the last error.
    [[nodiscard]] inline Handle Open()
    {
        // INVALID_SOCKET is (SOCKET)(~0), which is exactly -1 once it is signed — so the one invalid value
        // survives the conversion and callers need not know which platform they are on.
        return static_cast<Handle>( ::socket( AF_UNIX, SOCK_STREAM, 0 ) );
    }

    inline void Close( Handle handle )
    {
#if defined( _WIN32 )
        (void)::closesocket( Raw( handle ) );
#else
        (void)::close( Raw( handle ) );
#endif
    }

    /// Make @p handle non-blocking, so that accept and recv answer "nothing yet" instead of parking the
    /// caller's thread. False having set the last error.
    [[nodiscard]] inline bool SetNonBlocking( Handle handle )
    {
#if defined( _WIN32 )
        // Winsock has no fcntl; FIONBIO is the whole of its non-blocking switch.
        u_long nonBlocking = 1;
        return ::ioctlsocket( Raw( handle ), FIONBIO, &nonBlocking ) == 0;
#else
        const int flags = ::fcntl( Raw( handle ), F_GETFL, 0 );
        if ( flags < 0 )
            return false;
        return ::fcntl( Raw( handle ), F_SETFL, flags | O_NONBLOCK ) == 0;
#endif
    }

    /// The kernel's limit on a socket path, not ours, and it is short: 104 bytes on macOS, 108 on Windows
    /// and Linux. A path one byte over would be TRUNCATED and bound as a DIFFERENT socket — success
    /// reported while listening somewhere no client looks — so both ends check it and say the number.
    [[nodiscard]] constexpr std::size_t MaxPathLength() noexcept
    {
        return sizeof( sockaddr_un::sun_path ) - 1;
    }

    /// Point @p address at @p path, or false if it does not fit (see MaxPathLength).
    [[nodiscard]] inline bool FillAddress( sockaddr_un& address, const std::string& path ) noexcept
    {
        if ( path.size() > MaxPathLength() )
            return false;

        address.sun_family = AF_UNIX;
        std::memcpy( address.sun_path, path.c_str(), path.size() + 1 );
        return true;
    }

    /// @p address as the generic `sockaddr` that bind and connect take. The ONE place this cast lives:
    /// the Berkeley API is C's polymorphism, every family's address is passed through `sockaddr*` and told
    /// apart by its leading family field, so there is no cast-free spelling of these calls.
    [[nodiscard]] inline const sockaddr* AsSockaddr( const sockaddr_un& address ) noexcept
    {
        // Required by the socket API itself (see above); sockaddr_un begins with the family field it reads.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        return reinterpret_cast<const sockaddr*>( &address );
    }

    /// Is there a file at @p path? Asked with the platform's plainest call on purpose: a bound AF_UNIX
    /// socket is a REPARSE POINT on Windows, and the parts of <filesystem> that resolve a reparse point
    /// have to open it, which the filesystem refuses for this tag (ERROR_CANT_ACCESS_FILE).
    [[nodiscard]] inline bool FileExists( const std::string& path )
    {
#if defined( _WIN32 )
        return ::GetFileAttributesA( path.c_str() ) != INVALID_FILE_ATTRIBUTES;
#else
        return ::access( path.c_str(), F_OK ) == 0;
#endif
    }

    /// Delete the file at @p path — the socket file a bound socket leaves behind, which NEITHER platform
    /// removes when the socket is closed. Empty on success, the reason otherwise.
    [[nodiscard]] inline std::string RemoveFile( const std::string& path )
    {
#if defined( _WIN32 )
        if ( ::DeleteFileA( path.c_str() ) == 0 )
            return Detail::SystemErrorText( ::GetLastError() );
#else
        if ( ::unlink( path.c_str() ) != 0 )
            return ErrnoText();
#endif
        return {};
    }

    /// OWNER ONLY. Empty on success, the reason otherwise — and a caller that cannot get this must REFUSE
    /// to listen rather than listen anyway: this socket runs commands in somebody's editor, every command
    /// the palette offers, which includes saving over their scene.
    ///
    /// POSIX spells it in mode bits. Windows has none, and the socket file is created with whatever DACL
    /// its directory hands down — under a shared temp directory that can include Everyone — so the same
    /// promise is written out explicitly there: a DACL with ONE entry, for this process's own user, and
    /// PROTECTED so that the inherited entries are dropped instead of being merged back in.
    [[nodiscard]] inline std::string RestrictToOwner( const std::string& path )
    {
#if defined( _WIN32 )
        HANDLE token = nullptr;
        if ( ::OpenProcessToken( ::GetCurrentProcess(), TOKEN_QUERY, &token ) == 0 )
            return "OpenProcessToken failed: " + Detail::SystemErrorText( ::GetLastError() );

        // A TOKEN_USER is a header followed by a variable-length SID, so its size is ASKED for. The first
        // call is expected to fail with ERROR_INSUFFICIENT_BUFFER and is only there to fill `needed`.
        DWORD needed = 0;
        (void)::GetTokenInformation( token, TokenUser, nullptr, 0, &needed );
        std::vector<unsigned char> buffer( needed );
        if ( needed == 0 || ::GetTokenInformation( token, TokenUser, buffer.data(), needed, &needed ) == 0 )
        {
            const std::string reason = Detail::SystemErrorText( ::GetLastError() );
            ::CloseHandle( token );
            return "GetTokenInformation( TokenUser ) failed: " + reason;
        }
        ::CloseHandle( token );

        EXPLICIT_ACCESS_W entry{};
        entry.grfAccessPermissions = GENERIC_ALL;
        entry.grfAccessMode        = SET_ACCESS;
        entry.grfInheritance       = NO_INHERITANCE;
        entry.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
        entry.Trustee.TrusteeType  = TRUSTEE_IS_USER;
        entry.Trustee.ptstrName =
             reinterpret_cast<LPWSTR>( reinterpret_cast<TOKEN_USER*>( buffer.data() )->User.Sid );

        PACL        dacl     = nullptr;
        const DWORD composed = ::SetEntriesInAclW( 1, &entry, nullptr, &dacl );
        if ( composed != ERROR_SUCCESS || dacl == nullptr )
            return "SetEntriesInAcl failed: " + Detail::SystemErrorText( composed );

        // THE WIDE CALL, because SetNamedSecurityInfoA would re-encode the path through the ANSI codepage
        // a second time and a path outside it would name a different file than the one just bound. The
        // conversion is CP_ACP for the same reason the rest of this header uses the narrow calls: the path
        // arrived in argv, which Windows hands over in the machine's own codepage and not in UTF-8.
        std::wstring wide( path.size() + 1, L'\0' );
        const int    converted = ::MultiByteToWideChar( CP_ACP, 0, path.c_str(), static_cast<int>( path.size() ),
                                                        wide.data(), static_cast<int>( wide.size() ) );
        if ( converted <= 0 && !path.empty() )
        {
            const std::string reason = Detail::SystemErrorText( ::GetLastError() );
            ::LocalFree( dacl );
            return "the socket path could not be read as a wide string: " + reason;
        }
        wide.resize( static_cast<std::size_t>( converted ) );

        const DWORD applied = ::SetNamedSecurityInfoW(
             wide.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
             nullptr, dacl, nullptr );
        ::LocalFree( dacl );

        if ( applied != ERROR_SUCCESS )
            return "SetNamedSecurityInfo failed: " + Detail::SystemErrorText( applied );
#else
        if ( ::chmod( path.c_str(), S_IRUSR | S_IWUSR ) != 0 )
            return ErrnoText();
#endif
        return {};
    }
} // namespace Common::LocalSocket
