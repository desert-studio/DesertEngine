#pragma once

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace Common::Utils
{
    // THE DLLs A WINDOWS IMAGE IMPORTS, read from the image itself — the same list `dumpbin /dependents`
    // prints, for code that has no dumpbin (the packager, and every suite on macOS).
    //
    // WHY THE BINARY AND NOT A LIST. The packaged game is built with the DLL C runtime (/MD: the Vulkan
    // SDK's shaderc and spirv-cross libraries are stamped RuntimeLibrary=MD_DynamicRelease, so /MT cannot
    // link), and the Visual C++ runtime DLLs it needs travel next to Runtime.exe. WHICH DLLs that is
    // depends on what the compiler emitted (msvcp140.dll only if the C++ library is referenced,
    // vcruntime140_1.dll only for x64 exception handling of a certain shape, concrt140.dll only with
    // <future> in some toolsets), so a hand-kept list is a guess that goes stale with the toolset. The
    // import table is the answer.
    //
    // Both tables are read: the normal import directory (data directory 1) and the delay-load directory
    // (13), because a delay-loaded DLL that is absent fails exactly as late and as badly for the player.
    // PE32 and PE32+ are both accepted. Names come back as the image spells them, in table order, without
    // duplicates. Every malformation is an error naming the offset, never a shorter list.
    [[nodiscard]] Common::ResultStr<std::vector<std::string>> ReadPeImports( std::span<const std::uint8_t> bytes );

    [[nodiscard]] Common::ResultStr<std::vector<std::string>>
    ReadPeImportsOfFile( const std::filesystem::path& file );

    // THE APP-LOCAL RUNTIME CLOSURE: which files of `crtDir` (a Visual C++ redistributable directory,
    // `VC\Redist\MSVC\<ver>\x64\Microsoft.VC143.CRT`) must sit next to `exe` for it to start on a Windows
    // install that never ran the redistributable installer. That is every DLL in `crtDir` the exe imports,
    // plus every DLL in `crtDir` THOSE import (msvcp140.dll imports vcruntime140.dll), and nothing else —
    // shipping the whole directory would put DLLs next to the game that no code loads.
    //
    // Matching is case-insensitive (import tables say VCRUNTIME140.dll, the directory says
    // vcruntime140.dll). The result is sorted by file name.
    //
    // THE DEBUG CRT IS REFUSED, by name. A Debug build imports vcruntime140d.dll / msvcp140d.dll /
    // ucrtbased.dll, which Microsoft does not license for redistribution and which exist only on machines
    // with Visual Studio installed — a package carrying them is a package that starts on the developer's
    // machine and nowhere else. A debug CRT DLL is recognised as `<name>d.dll` where `<name>.dll` is in
    // `crtDir`, plus ucrtbased.dll (the UCRT's debug build, which ships in the Windows SDK, not here).
    //
    // An exe whose import table lists nothing is refused: every Windows image imports KERNEL32 at the
    // least, so an empty list means the reader failed, not that there is nothing to ship.
    [[nodiscard]] Common::ResultStr<std::vector<std::filesystem::path>>
    AppLocalRuntimeClosure( const std::filesystem::path& exe, const std::filesystem::path& crtDir );
} // namespace Common::Utils
