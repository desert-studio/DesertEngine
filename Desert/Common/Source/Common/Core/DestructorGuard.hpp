#pragma once

// A DESTRUCTOR CANNOT REPORT, SO IT MUST NOT THROW.
//
// Every destructor is implicitly `noexcept` since C++11. An exception that reaches the closing brace of
// one is therefore not a failure a caller can catch — it is `std::terminate`, called with the stack
// already unwound past whatever threw. Nothing in the process gets to say what happened.
//
// This matters here because eight destructors in this engine REPORT: they log a leaked include result, a
// refused VMA free, a material window closed with unsaved edits, a renderer slot handed back. Logging is
// exactly the kind of work that allocates and formats, so the line whose whole job is to make a failure
// visible is also the line most likely to make the process vanish without one.
//
// The guard is a `catch`, not a policy of "do not log in destructors". The log is worth keeping: a VMA
// free that refused with nothing printed leaked device memory and surfaced much later as an allocation
// failure somewhere unrelated. What is not worth keeping is the silence when the report itself fails, so
// the handler writes to stderr with `fputs` and `fflush` — neither of which allocates, formats, or throws,
// and both of which work when the failure IS the logger.
//
// USE:
//     Foo::~Foo()
//     try
//     {
//         ...
//     }
//     DESERT_DESTRUCTOR_GUARD( "~Foo" )
//
// A function-try-block rather than a `try` inside the body, so a throwing member destructor of a base or
// a data member is covered too — that is the half a body-level try cannot reach.

#include <cstdio>
#include <exception>

namespace Common::Detail
{
    /// Reports a failure that reached a destructor's closing brace. `noexcept` and allocation-free: this
    /// runs when something has already gone wrong enough to unwind into a destructor.
    inline void ReportDestructorFailure( const char* where, const char* what ) noexcept
    {
        std::fputs( "[Desert] an exception escaped ", stderr );
        std::fputs( where, stderr );
        std::fputs( " and was swallowed there: ", stderr );
        std::fputs( what != nullptr ? what : "<unknown exception>", stderr );
        std::fputs( "\n", stderr );
        std::fflush( stderr );
    }
} // namespace Common::Detail

#define DESERT_DESTRUCTOR_GUARD( where )                                                                          \
    catch ( const std::exception& desertDestructorFailure )                                                       \
    {                                                                                                             \
        ::Common::Detail::ReportDestructorFailure( where, desertDestructorFailure.what() );                       \
    }                                                                                                             \
    catch ( ... )                                                                                                 \
    {                                                                                                             \
        ::Common::Detail::ReportDestructorFailure( where, nullptr );                                              \
    }
