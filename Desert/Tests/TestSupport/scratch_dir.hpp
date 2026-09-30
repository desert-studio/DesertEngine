#pragma once

// WHERE A TEST MAY WRITE: ITS OWN THROWAWAY DIRECTORY, NEVER THE WORKING DIRECTORY.
//
// The runners start every suite in build/TestScratch/<config>/<suite> (scripts/MacOS/RunTests.sh,
// scripts/Windows/RunTests.ps1), but handoff_check, an IDE and a developer's shell start them from the
// repository root — so a probe project built as `current_path() / "RegistryProbe"` landed in the checkout
// (Assets/, DerivedDataCache/, RegistryProbe/ on Windows, 09-27). UE writes its automation output under
// Saved/Automation for the same reason. A test that needs files gets a ScratchDir; a test whose code under
// test resolves RELATIVE paths gets a ScratchWorkingDirectory; neither ever joins onto current_path().
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

    // The checkout, found by walking up from the working directory — which is the repository root or a
    // directory beneath it (build/TestScratch/<config>/<suite>) under every runner. For READING tracked
    // files; empty when not found, and the caller asserts on that with its own context.
    inline std::filesystem::path RepositoryRoot()
    {
        std::error_code ec;
        for ( std::filesystem::path at = std::filesystem::current_path( ec ); !ec && !at.empty();
              at                       = at.parent_path() )
        {
            if ( std::filesystem::exists( at / "Desert" / "Tests" / "TestSupport", ec ) &&
                 std::filesystem::exists( at / "Editor" / "Resources", ec ) )
                return at;
            if ( at == at.parent_path() )
                break;
        }
        return {};
    }

    // THE HOST STEP, TAKEN THE WAY THE EDITOR TAKES IT. The engine resolves its own resources — the shader root
    // (Common::Constants::Path::SHADERDIR_PATH) and the shading models in it, fonts, icons, the engine content —
    // against the working directory, and a host answers "where are they" by working FROM that directory
    // (Project::ResolveResourceRoot; the editor moves there in Sandbox.hpp before anything reads content). In a
    // checkout that directory is Editor/. A suite whose code under test reads engine resources — a DShader
    // parse resolving its `ShadingModel`, an include, a key — works from there for this scope; the previous
    // working directory comes back on destruction. READ-ONLY by contract: nothing may be written relative to
    // the working directory inside it (TestScratchCensus), so a test that writes enters a ScratchDir or an
    // AssetsSandbox, which moves the process on again.
    class EngineResourcesWorkingDirectory
    {
    public:
        EngineResourcesWorkingDirectory()
        {
            std::error_code ec;
            m_Previous                       = std::filesystem::current_path( ec );
            const std::filesystem::path root = RepositoryRoot();
            if ( ec || root.empty() )
            {
                m_Error = std::format( "no checkout above the working directory '{}'", m_Previous.string() );
                return;
            }
            std::filesystem::current_path( root / "Editor", ec );
            if ( ec )
                m_Error =
                     std::format( "could not work from '{}': {}", ( root / "Editor" ).string(), ec.message() );
        }

        ~EngineResourcesWorkingDirectory()
        {
            std::error_code ec;
            if ( !m_Previous.empty() )
                std::filesystem::current_path( m_Previous, ec );
        }

        EngineResourcesWorkingDirectory( const EngineResourcesWorkingDirectory& )            = delete;
        EngineResourcesWorkingDirectory& operator=( const EngineResourcesWorkingDirectory& ) = delete;
        EngineResourcesWorkingDirectory( EngineResourcesWorkingDirectory&& )                 = delete;
        EngineResourcesWorkingDirectory& operator=( EngineResourcesWorkingDirectory&& )      = delete;

        // Empty when the process now works from the engine resources; otherwise what went wrong.
        [[nodiscard]] const std::string& Error() const
        {
            return m_Error;
        }

    private:
        std::filesystem::path m_Previous;
        std::string           m_Error;
    };
} // namespace Desert::TestSupport
