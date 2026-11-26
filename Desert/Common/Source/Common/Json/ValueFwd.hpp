#pragma once

// The JSON value and node by name only, for a header that passes them by reference and must not compile
// reflect-cpp (Json.hpp is reached by ~700 translation units; a signature needs only this).
namespace rfl
{
    class Generic;
}

namespace Common::Json
{
    // An untyped JSON value; defined with its reader and writer in Json.hpp.
    using Value = rfl::Generic;

    class Node;
} // namespace Common::Json
