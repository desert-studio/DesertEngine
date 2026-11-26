#pragma once

// The fixed simulation step every authoritative loop runs at (the server step, the net frame number, the
// client prediction step). One number, here, because Projects (the Net settings panel shows steps as Hz) and
// Net (the wire counts steps) both need it and neither may include the other.
namespace Common
{
    inline constexpr double kSimulationStepHz = 60.0;
}
