#include <Editor/LiveCoding/ILiveCodingBackend.hpp>

#include <cerrno>
#include <csignal>
#include <unistd.h>

namespace Desert::Editor::LiveCoding
{
    // THE macOS DEFINITION (the DesktopPlatform pattern: one definition per platform). The backend reads PE/COFF
    // objects, a /MAP file and hotpatch padding; none of them exists for a Mach-O image, so the session refuses
    // every request with this reason and the editor runs unchanged.
    Common::ResultStr<std::unique_ptr<ILiveCodingBackend>> CreateLiveCodingBackend()
    {
        return Common::MakeError<std::unique_ptr<ILiveCodingBackend>>(
             "Live Coding patches PE/COFF images; macOS images are Mach-O" );
    }

    std::uint32_t CurrentProcessId()
    {
        return static_cast<std::uint32_t>( ::getpid() );
    }

    bool ProcessRuns( const std::uint32_t pid )
    {
        return ::kill( static_cast<pid_t>( pid ), 0 ) == 0 || errno == EPERM;
    }
} // namespace Desert::Editor::LiveCoding
