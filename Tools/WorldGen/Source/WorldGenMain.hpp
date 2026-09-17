#pragma once

// The tool's whole body behind a callable signature, so Desert/Tests/Tools/WorldSceneGenerator drives the
// REAL write path - argument parsing, the material table, the canonical Settings block, the bytes - rather
// than a re-implementation of it that could agree with itself and disagree with the tool. Same shape, and
// for the same reason, as Tools/SceneMigrator's RunSceneMigrator.

#include <iosfwd>
#include <string>
#include <vector>

namespace Desert::WorldGen
{
    // 0 on success, non-zero with a named reason on stderr otherwise. Never partially writes: the scene is
    // built whole in memory and goes to disk through the write-then-rename primitive.
    int RunWorldGen( const std::vector<std::string>& args, std::ostream& out, std::ostream& err );
} // namespace Desert::WorldGen
