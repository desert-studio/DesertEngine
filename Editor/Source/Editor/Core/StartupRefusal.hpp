#pragma once

#include <string>

namespace Desert::Editor
{
    // A REFUSAL THAT CAN BE SEEN. On Windows the editor is a windowed application with no console unless
    // `--console` is given, so stderr goes nowhere: started from Visual Studio it simply "exited with code 1".
    // Every refusal before the first window goes through here, which keeps stderr for scripts and CI and adds
    // the debugger's Output pane and, when no console is attached, a message box. Defined in a .cpp so that
    // <Windows.h> and its macros stay out of every file that includes the application entry.
    [[noreturn]] void RefuseToStart( int exitCode, const std::string& message );
} // namespace Desert::Editor
