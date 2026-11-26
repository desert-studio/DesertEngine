#pragma once

#include <Editor/LiveCoding/LiveCodingTypes.hpp>

#include <atomic>
#include <memory>
#include <span>

namespace Desert::Editor::LiveCoding
{
    class BuildIndex;

    /// A patch compiled and linked into memory the process does not execute yet.
    class IPreparedPatch
    {
    public:
        virtual ~IPreparedPatch() = default;

        /// MAIN THREAD, at the frame boundary, once. Redirects the original functions with every other thread of
        /// the process suspended, then runs the new data's initialisers. Never allocates while threads are
        /// suspended (a suspended thread may hold the heap lock).
        [[nodiscard]] virtual Common::ResultStr<PatchReport> Apply() noexcept = 0;
    };

    /// The executable-format half of Live Coding: PE/COFF on Windows. One per session.
    class ILiveCodingBackend
    {
    public:
        virtual ~ILiveCodingBackend() = default;

        /// ON A WORKER. Compiles @p edited (indices into index.Sources()) and every source whose functions inlined
        /// their code, loads the objects and links them against the loaded images. Touches no code the process
        /// runs. @p stage is advanced for `state`; @p cancel stops the compilers (editor exit).
        [[nodiscard]] virtual Common::ResultStr<std::unique_ptr<IPreparedPatch>>
        Prepare( const BuildIndex& index, std::span<const std::size_t> edited, std::atomic<PatchStage>& stage,
                 const std::atomic<bool>& cancel ) noexcept = 0;
    };

    /// One definition per platform (the DesktopPlatform pattern): Platform/Windows/LiveCoding/LiveCodingWindows.cpp
    /// returns the PE/COFF backend; Platform/MacOS/LiveCodingMacOS.mm returns the refusal
    /// "Live Coding patches PE/COFF images; macOS images are Mach-O".
    [[nodiscard]] Common::ResultStr<std::unique_ptr<ILiveCodingBackend>> CreateLiveCodingBackend();
} // namespace Desert::Editor::LiveCoding
