#pragma once

#include <cstddef>

namespace Desert::Editor
{
    // WHICH PLATFORM CAN THIS EDITOR PACKAGE FOR, AND WHAT DOES THAT IMPLY?
    //
    // Exactly one: its own host. `PackageGame` ships the Runtime binary that was built beside this
    // editor, by this machine's toolchain — there is no cross-compiler in this repository and no
    // Runtime for another platform to copy. The Windows build is msbuild on a Windows runner
    // (.github/workflows/ci.yml), so a Windows package is produced by an editor RUNNING on Windows and
    // in no other way.
    //
    // This header exists because that fact used to be stated in three places and enforced in none. The
    // Build Settings panel drew a three-row "Target platform" chooser whose answer nothing read, so
    // selecting "Windows x64" on macOS produced a macOS package in silence (contract §1.3 and §1.4 at
    // once — П6). The packager separately hard-coded macOS in four places: the binary's name, the .app
    // layout, the bash launcher and the build script named in the "not found" error. Both halves now
    // derive from the ONE description below, which is what makes the panel's claim and the packager's
    // behaviour incapable of disagreeing.
    //
    // WHAT IS DELIBERATELY NOT HERE: a `TargetPlatform` field in `PackageOptions`. A chooser whose only
    // reachable value is the host is the same dead setting in a new place; the platform is DERIVED, in
    // one function, and read by both sides. The day a cross-toolchain exists is the day a second value
    // becomes possible and the field becomes worth having — and not before.
    enum class TargetPlatform
    {
        MacOS,
        Windows,
        Linux,
    };

    struct TargetPlatformInfo
    {
        TargetPlatform Platform;

        // As a person names it in the panel.
        const char* DisplayName;

        // The file PackageGame copies out of build/Bin/<Config>. The extension is the whole reason this
        // is data rather than a literal: the packager looked for `Runtime` unconditionally, which on a
        // Windows host can never exist because the binary there is `Runtime.exe`.
        const char* RuntimeBinary;

        // The script the finished package is started through, and the shell it is written in.
        const char* LauncherName;

        // macOS only: a .app with MoltenVK and the Vulkan loader inside Contents/Frameworks. Everywhere
        // else the concept does not exist, and honouring `PackageOptions::MacAppBundle` there would
        // produce a bundle-shaped directory with a bash launcher in it.
        bool SupportsAppBundle;

        // The script that builds a Runtime for this platform, ON that platform — or nullptr when the
        // engine has no build for it at all. It is named in the packager's "Runtime binary not found"
        // message (which used to say scripts/MacOS/BuildMacOS.sh on every host), so it is only ever
        // read for the HOST, and a host always has one — the static_assert below HostPlatformInfo().
        const char* BuildScript;

        // Shown beside the row when this is NOT the host. It is per-row rather than one shared string
        // because the two non-host rows are not the same answer: the engine really does build for
        // Windows, just not from a macOS machine, whereas it does not build for Linux at all. Both used
        // to be one greyed row saying nothing, which is how "Linux x64 (planned)" managed to look like
        // the same kind of statement as a platform CI builds every day.
        const char* NotHereReason;
    };

    // Every platform the panel lists, host and non-host alike — the non-host rows are shown disabled
    // with their reason rather than hidden, because "we cannot do this here" is information and a row
    // that vanishes is not.
    inline constexpr TargetPlatformInfo kTargetPlatforms[] = {
         { TargetPlatform::MacOS, "macOS (Apple Silicon)", "Runtime", "run.sh", true,
           "scripts/MacOS/BuildMacOS.sh", "build the editor on macOS" },
         { TargetPlatform::Windows, "Windows x64", "Runtime.exe", "run.bat", false,
           "scripts\\Windows\\BuildWindows.bat", "build the editor on Windows" },
         // No build script, and that is the honest entry rather than an invented path: there is no
         // scripts/Linux, no Linux branch in Editor/CMakeLists.txt and no Linux job in CI. The row exists
         // so the panel can say that, which is more than "Linux x64 (planned)" ever said.
         { TargetPlatform::Linux, "Linux x64", "Runtime", "run.sh", false, nullptr,
           "the engine has no Linux build" },
    };

    inline constexpr std::size_t kTargetPlatformCount = sizeof( kTargetPlatforms ) / sizeof( kTargetPlatforms[0] );

    // INDEXED BY THE ENUM, NOT SEARCHED FOR. A linear search needs a "not found" branch, and the only
    // things such a branch can do here are return the first row — a silent wrong answer, §1.4 — or
    // abort. Making the array positionally the enum removes the branch instead of choosing between two
    // bad answers, and the assertions below are what keep the two in step at compile time.
    inline constexpr const TargetPlatformInfo& PlatformInfo( TargetPlatform platform )
    {
        return kTargetPlatforms[static_cast<std::size_t>( platform )];
    }

    static_assert( kTargetPlatformCount == 3, "a platform was added or removed; give it a row below too" );
    static_assert( kTargetPlatforms[static_cast<std::size_t>( TargetPlatform::MacOS )].Platform ==
                   TargetPlatform::MacOS );
    static_assert( kTargetPlatforms[static_cast<std::size_t>( TargetPlatform::Windows )].Platform ==
                   TargetPlatform::Windows );
    static_assert( kTargetPlatforms[static_cast<std::size_t>( TargetPlatform::Linux )].Platform ==
                   TargetPlatform::Linux );

    // The platform this editor binary is running on, and therefore the only one it can package for.
    //
    // A host the engine has never been built for is a compile error rather than a default: silently
    // answering "macOS" on an unknown system is precisely the silent substitution §1.4 forbids, and it
    // would reach a person as a package with the wrong launcher in it.
    inline constexpr TargetPlatform HostPlatform()
    {
#if defined( DESERT_PLATFORM_MACOS )
        return TargetPlatform::MacOS;
#elif defined( DESERT_PLATFORM_WINDOWS )
        return TargetPlatform::Windows;
#elif defined( DESERT_PLATFORM_LINUX )
        return TargetPlatform::Linux;
#else
#error "PackageTarget.hpp: no DESERT_PLATFORM_* define — the packager cannot name its own host."
#endif
    }

    inline constexpr const TargetPlatformInfo& HostPlatformInfo()
    {
        return PlatformInfo( HostPlatform() );
    }

    // The host's build script is the string PackageGame's "Runtime binary not found" message is made of,
    // and that message is the whole of what a person gets when packaging fails. A null there produces
    // "Build it first: " and stops — §1.4's empty answer, delivered exactly when somebody needs a real
    // one. Asserted at compile time because it is a compile-time fact about THIS host.
    static_assert( HostPlatformInfo().BuildScript != nullptr,
                   "this editor's own host must name the script that builds its Runtime" );

    // Why `platform` cannot be produced by this editor, or nullptr when it can.
    //
    // Kept SHORT because it is shown on the same line as the row it explains, and the panel is 560 px
    // wide: the first version of this string was a full sentence and the rendered frame cut it off mid
    // word, which is a worse answer than none. The full explanation is the wrapped paragraph the panel
    // prints under the rows.
    inline constexpr const char* WhyNotPackageableHere( TargetPlatform platform )
    {
        if ( platform == HostPlatform() )
            return nullptr;
        return PlatformInfo( platform ).NotHereReason;
    }

    // The paragraph under the platform rows. It has to answer the question the disabled rows raise —
    // "then how do I get a Windows build?" — because a refusal that does not say what to do instead is
    // half an answer.
    inline constexpr const char* kWhyOnlyTheHostIsOffered =
         "The package ships the Runtime binary built beside this editor, so the only target is this "
         "editor's own host. For another platform, build the editor there and package from it.";
} // namespace Desert::Editor
