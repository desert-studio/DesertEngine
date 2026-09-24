#pragma once

// The tool's whole body behind a callable signature, the shape of WorldGen's RunWorldGen, so a suite can drive
// the real argument handling and write path.

#include <iosfwd>
#include <string>
#include <vector>

namespace Desert::WorldCook
{
    // 0 on success, non-zero with a named reason on stderr otherwise.
    int RunWorldCook( const std::vector<std::string>& args, std::ostream& out, std::ostream& err );
} // namespace Desert::WorldCook
