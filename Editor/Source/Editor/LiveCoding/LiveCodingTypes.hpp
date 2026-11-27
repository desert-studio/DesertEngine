#pragma once

#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// LIVE CODING (Live++ / UE Live Coding): the edited .cpp files are compiled alone, linked into the running editor's
// memory against the images that are loaded, and every function they define is redirected at its entry. Data stays
// in the original image; nothing is unloaded; registrations stay valid because they point at redirected entries.
namespace Desert::Editor::LiveCoding
{
    /// One image the editor process runs: Editor.exe or a module DLL, with its /MAP file.
    struct ImageRecord
    {
        std::string           Target; // the CMake target, "DesertVFX"
        std::filesystem::path Image;  // build/Bin/Development/DesertVFX.dll
        std::filesystem::path Map;    // the linker's /MAP file of that image
    };

    /// One source file as Ninja compiled it into an image.
    struct SourceRecord
    {
        std::filesystem::path Source;      // the .cpp the user edits (absolute, generic form)
        std::filesystem::path BuiltObject; // the object in the image: the unity object for a unity member
        std::filesystem::path UnitySource; // the unity_N_cxx.cxx that includes Source; empty when compiled alone
        std::string           Command;     // compile_commands.json "command" of BuiltObject's translation unit
        std::filesystem::path Directory;   // compile_commands.json "directory"
        std::size_t           Image = 0;   // index into BuildIndex::Images()
    };

    /// Where a patch is; `state` prints the words after "Live Coding".
    enum class PatchStage : std::uint8_t
    {
        Idle,
        Scanning,  // finding the changed sources and the sources that inlined them
        Compiling, // cl.exe runs
        Linking,   // objects loaded, symbols resolved, code written into the patch memory
        Ready,     // waiting for the next frame boundary
    };

    inline constexpr const char* kPatchStageNames[] = { "idle", "scanning sources", "compiling", "linking",
                                                        "waiting for the frame boundary" };

    /// What one patch did. Logged whole; Summary() is the palette reply and the `state` outcome.
    struct PatchReport
    {
        std::vector<std::filesystem::path> Edited;         // the changed sources the scan found
        std::vector<std::filesystem::path> Inliners;       // compiled too: their functions carried edited code
        std::vector<std::string>           Redirected;     // original functions now entering the patch
        std::vector<std::string>           NewFunctions;   // defined by the patch only
        std::vector<std::string>           NewData;        // data the image lacked, initialised by the patch
        std::vector<std::string>           NotRerun;       // dynamic initialisers of existing data that changed:
                                                           // in effect after a restart
        std::uintptr_t                     PatchBegin = 0; // the patch memory, for crash addresses in the log
        std::uintptr_t                     PatchEnd   = 0;
        double                             CompileSeconds    = 0.0;
        double                             ApplyMilliseconds = 0.0;

        /// "patched 12 functions in 1 source (+1 inliner), 0 new data, compile 11.2 s, apply 0.4 ms"
        [[nodiscard]] std::string Summary() const;
    };
} // namespace Desert::Editor::LiveCoding
