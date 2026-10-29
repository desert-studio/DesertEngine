# Writes run_tests.bat (Desert/Tests RunAllTests, Windows): a three-line shim that names the configuration just
# built and hands the run to scripts/Windows/RunTests.ps1, under pwsh when it is on PATH and Windows PowerShell 5.1
# otherwise, the same script with the same arguments either way. No exit check: the .bat returns the exit code of
# its last command, the interpreter, so a missing interpreter (9009) can never come back as a pass. The runner's
# path is %~dp0, the .bat's own folder (the repository root). Why each line looks as it does: the postbuild comment
# in Desert/Tests/premake5.lua. Here the text is data, written in one piece, so nothing passes through cmd's echo.
#   cmake -D OUTPUT=<run_tests.bat> -D CONFIG=<Debug|Release> -P WriteRunTestsBat.cmake
if(NOT OUTPUT OR NOT CONFIG)
    message(FATAL_ERROR "WriteRunTestsBat.cmake: OUTPUT and CONFIG are required")
endif()
set(_args "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"%~dp0scripts\Windows\RunTests.ps1\" -Config ${CONFIG}")
file(WRITE "${OUTPUT}"
    "@echo off\r\n"
    "where /q pwsh\r\n"
    "if errorlevel 1 (powershell.exe ${_args}) else (pwsh ${_args})\r\n")
