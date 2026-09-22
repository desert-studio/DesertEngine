#pragma once

#include <memory>
#include <utility>
#include "Logger.hpp"
#include "Constants.hpp"
#include <Common/Core/UUID.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/ResultWithCodes.hpp>

#define BUILD_ID "v0.1a"

// __VA_ARGS__ expansion to get past MSVC "bug"
#define RE_EXPAND_VARGS( x ) x

#define BIT( x ) ( 1u << x )
#define BIND_FN( fn )                                                                                             \
    [this]( auto&&... args ) -> decltype( auto ) { return this->fn( std::forward<decltype( args )>( args )... ); }

#define RA_HAS_VALUE( var ) var.has_value()

template <typename T>
decltype( auto ) initializeDefaultValue()
{
    if constexpr ( std::is_pointer_v<T> )
    {
        return nullptr;
    }
    else
    {
        return T{};
    }
}

#define RA_GET_VALUE( var ) var.value_or( initializeDefaultValue<decltype( var )::value_type>() )
#define IS_DERIVED_bool( T, X ) ( std::is_base_of<X, T>::value )
#define NO_DISCARD [[nodiscard]]
#define BOOLSUCCESS Common::MakeSuccess( true );

// THE LAST BRANCH USED TO BE `__debugbreak()`, WHICH IS AN MSVC INTRINSIC. Every project file in the
// tree carries its own `filter "system:*" defines { DESERT_PLATFORM_* }` block, and any target that
// forgets one — every test suite does, and test suites compile engine sources — fell into that branch
// and failed to COMPILE with "use of undeclared identifier '__debugbreak'". So a header using
// DESERT_VERIFY could not be reached from a test at all, and the diagnostic named an MSVC builtin on
// macOS, which points at nothing.
//
// The platform branches below are unchanged, so every configured build breaks exactly as it did. What
// is new is that the fallback asks the COMPILER instead of giving up: a debug break is a compiler
// facility, not a platform one, and DESERT_VERIFY calls std::abort() immediately afterwards in any
// case — so the worst the last branch can cost is the breakpoint, never the abort.
#if defined( DESERT_PLATFORM_WINDOWS )
#define DESERT_DEBUG_BREAK __debugbreak()
#elif defined( DESERT_PLATFORM_MACOS )
#define DESERT_DEBUG_BREAK __builtin_debugtrap()
#elif defined( DESERT_PLATFORM_LINUX )
#include <signal.h>
#define DESERT_DEBUG_BREAK raise( SIGTRAP )
#elif defined( _MSC_VER )
#define DESERT_DEBUG_BREAK __debugbreak()
#elif defined( __clang__ ) || defined( __GNUC__ )
#define DESERT_DEBUG_BREAK __builtin_trap()
#else
#define DESERT_DEBUG_BREAK ( (void)0 )
#endif

namespace Common::Detail
{
    // Forwarders for DESERT_VERIFY's optional message. An overload pair rather than __VA_OPT__
    // because the Windows build uses MSVC's TRADITIONAL preprocessor (no /Zc:preprocessor — see the
    // RE_EXPAND_VARGS workaround above), which does not implement __VA_OPT__; a plain __VA_ARGS__
    // forwarded into a call works on every compiler this project builds with.
    inline void LogVerifyMessage()
    {
    }
    template <typename... Args>
    void LogVerifyMessage( fmt::format_string<Args...> message, Args&&... args )
    {
        Common::Logger::LogError( message, std::forward<Args>( args )... );
    }
    inline void WarnVerifyMessage()
    {
    }
    template <typename... Args>
    void WarnVerifyMessage( fmt::format_string<Args...> message, Args&&... args )
    {
        Common::Logger::LogWarn( message, std::forward<Args>( args )... );
    }
} // namespace Common::Detail

// DESERT_VERIFY is for INVARIANTS — states the program cannot continue from — and it aborts in EVERY
// configuration, Release included. It is not an error channel: a missing file, a failed parse, a
// resource that did not load all go through Common::BoolResultStr / LOG_ERROR so the caller can
// refuse with a name instead of dying (the read primitives in FileSystem used to VERIFY on a missing
// file, which crashed a packaged game over one absent asset).
// The optional message args used to be silently DISCARDED — "Could not read file! {}" never once
// reached a log — so they are forwarded to the logger now.
#define DESERT_VERIFY( cond, ... )                                                                                \
    do                                                                                                            \
    {                                                                                                             \
        if ( !( cond ) )                                                                                          \
        {                                                                                                         \
            Common::Logger::LogError( "Verify failed: {} at {}:{}", #cond, __FILE__, __LINE__ );                  \
            Common::Detail::LogVerifyMessage( __VA_ARGS__ );                                                      \
            DESERT_DEBUG_BREAK;                                                                                   \
            std::abort();                                                                                         \
        }                                                                                                         \
    } while ( false )

#define DESERT_VERIFY_WARN( cond, ... )                                                                           \
    if ( !( cond ) )                                                                                              \
    {                                                                                                             \
        Common::Logger::LogWarn( "Verify failed: {} at {}:{}", #cond, __FILE__, __LINE__ );                       \
        Common::Detail::WarnVerifyMessage( __VA_ARGS__ );                                                         \
    }

// #define MAKE_SHARED_OBJECT( type, value ) Memory::Shared<type>::Create( value );

namespace Common
{
    template <typename T>
    using Unique = std::unique_ptr<T>;
}

namespace Common
{
    using serialized_str = std::string;
    // AssetHandle is a real class (see Common/Core/AssetHandle.hpp, included above), not an alias.
    using Filepath = std::filesystem::path;
} // namespace Common

template <typename T, typename U>
constexpr std::shared_ptr<T> sp_cast( const std::shared_ptr<U>& ptr )
{
    return std::static_pointer_cast<T>( ptr );
}

#define SP_CAST(T, ptr) sp_cast<T>(ptr)
#define UNIQUE_GET_AS(T, ptr) static_cast<T*>((ptr).get())