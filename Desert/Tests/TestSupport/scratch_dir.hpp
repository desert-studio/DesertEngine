#pragma once

// WHERE A TEST MAY WRITE: ITS OWN THROWAWAY DIRECTORY, NEVER THE WORKING DIRECTORY.
//
// The runners start every suite in build/TestScratch/<config>/<suite> (scripts/MacOS/RunTests.sh,
// scripts/Windows/RunTests.ps1), but handoff_check, an IDE and a developer's shell start them from the
// repository root — so a probe project built as `current_path() / "RegistryProbe"` landed in the checkout
// (Assets/, DerivedDataCache/, RegistryProbe/ on Windows, 09-27). UE writes its automation output under
// Saved/Automation for the same reason. A test that needs files gets a ScratchDir and passes its path on
// explicitly; the engine is pointed with EngineDirScope / SetProjectRoot, never by moving the process.
// ScratchWorkingDirectory is TRANSITIONAL: it stays only while Common::AssetHandle::StableKeyForPath resolves
// a relative spelling through the working directory (AssetHandleStability, ThumbnailKey measure that).
// Tools/TestScratchCensus holds every suite to that.

#include <gtest/gtest.h>

#include <filesystem>
#include <format>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace Desert::TestSupport
{
    // A unique directory under temp_directory_path() (per-suite under the runners, which point TMPDIR/TMP at
    // the suite's scratch), removed with everything in it on destruction.
    class ScratchDir
    {
    public:
        explicit ScratchDir( const std::string_view tag = "desert-test" )
        {
            std::error_code             ec;
            const std::filesystem::path temp = std::filesystem::temp_directory_path( ec );
            if ( ec )
            {
                ADD_FAILURE() << "no temporary directory: " << ec.message();
                return;
            }
            m_Path = temp / ( std::string( tag ) + "-" + std::to_string( std::random_device{}() ) );
            std::filesystem::remove_all( m_Path, ec );
            std::filesystem::create_directories( m_Path, ec );
            if ( ec )
            {
                ADD_FAILURE() << "could not create the scratch directory '" << m_Path.string()
                              << "': " << ec.message();
                return;
            }
            // Canonical, so it is the same spelling current_path() answers once a ScratchWorkingDirectory
            // enters it: macOS's temp is /var/folders/..., a link to /private/var/folders/..., and a test that
            // compares a relative spelling against Path()-based ones would otherwise see two roots.
            std::filesystem::path canonical = std::filesystem::canonical( m_Path, ec );
            if ( !ec )
                m_Path = std::move( canonical );
        }

        ~ScratchDir()
        {
            std::error_code ec;
            if ( !m_Path.empty() )
                std::filesystem::remove_all( m_Path, ec );
        }

        ScratchDir( const ScratchDir& )            = delete;
        ScratchDir& operator=( const ScratchDir& ) = delete;
        ScratchDir( ScratchDir&& )                 = delete;
        ScratchDir& operator=( ScratchDir&& )      = delete;

        [[nodiscard]] const std::filesystem::path& Path() const
        {
            return m_Path;
        }

    private:
        std::filesystem::path m_Path;
    };

    // A ScratchDir the process works inside for the length of one scope: the previous working directory is
    // restored before the directory is removed, even when an assertion unwinds the test.
    class ScratchWorkingDirectory
    {
    public:
        explicit ScratchWorkingDirectory( const std::string_view tag = "desert-test" ) : m_Dir( tag )
        {
            std::error_code ec;
            m_Previous = std::filesystem::current_path( ec );
            if ( ec )
            {
                ADD_FAILURE() << "could not read the working directory: " << ec.message();
                return;
            }
            if ( !m_Dir.Path().empty() )
                std::filesystem::current_path( m_Dir.Path(), ec );
            if ( ec )
                ADD_FAILURE() << "could not enter the scratch directory '" << m_Dir.Path().string()
                              << "': " << ec.message();
        }

        ~ScratchWorkingDirectory()
        {
            std::error_code ec;
            if ( !m_Previous.empty() )
                std::filesystem::current_path( m_Previous, ec );
        }

        ScratchWorkingDirectory( const ScratchWorkingDirectory& )            = delete;
        ScratchWorkingDirectory& operator=( const ScratchWorkingDirectory& ) = delete;
        ScratchWorkingDirectory( ScratchWorkingDirectory&& )                 = delete;
        ScratchWorkingDirectory& operator=( ScratchWorkingDirectory&& )      = delete;

        [[nodiscard]] const std::filesystem::path& Path() const
        {
            return m_Dir.Path();
        }

    private:
        ScratchDir            m_Dir; // declared first: destroyed last, after the working directory moved back
        std::filesystem::path m_Previous;
    };

    // The checkout, baked into every suite by the build (Desert/Tests/premake5.lua -> DESERT_TEST_REPO_ROOT, an
    // absolute path), never searched for from the working directory: a suite runs from any folder. For READING
    // tracked files.
    inline std::filesystem::path RepositoryRoot()
    {
        return std::filesystem::path( DESERT_TEST_REPO_ROOT );
    }

    // The suite data tree, Desert/Tests/Data (DESERT_TEST_DATA_DIR): assets only tests read — probes, fixtures —
    // which never live in the engine's own content (Editor/Resources).
    inline std::filesystem::path TestDataDir()
    {
        return std::filesystem::path( DESERT_TEST_DATA_DIR );
    }
} // namespace Desert::TestSupport
