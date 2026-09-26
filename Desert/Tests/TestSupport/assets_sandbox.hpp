#pragma once

// A THROWAWAY WORKING DIRECTORY WITH A `Resources/Assets/` TREE IN IT.
//
// The migrator's whole-chain tests hand `MigrateScene` the RELATIVE root "Resources/Assets": the material
// path step (v7 -> v8) finds that root as a run of components inside the absolute paths old scenes stored,
// so it cannot be swapped for an absolute temporary directory. Since SCNE 27 the chain also OPENS every
// `.demat` a slot names by path alone, to read its header GUID - and a synthetic scene's materials exist
// nowhere. This sandbox makes the relative root real for the length of one test: it creates the named files
// under <temp>/Resources/Assets/, moves the process into <temp>, and on destruction moves back and removes
// the tree. The files carry no header, which is exactly a `.demat` whose own header step has not run yet.

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <system_error>

namespace Desert::TestSupport
{
    class AssetsSandbox
    {
    public:
        AssetsSandbox( std::string_view suite, std::initializer_list<const char*> headerlessFiles )
        {
            static std::atomic<int> sequence = 0;
            m_Root                           = std::filesystem::temp_directory_path() /
                     ( std::string( suite ) + "-" + std::to_string( sequence++ ) );
            std::error_code ec;
            std::filesystem::remove_all( m_Root, ec );
            for ( const char* relative : headerlessFiles )
            {
                const std::filesystem::path file = m_Root / "Resources/Assets" / relative;
                std::filesystem::create_directories( file.parent_path() );
                std::ofstream( file, std::ios::binary ) << "{}\n";
                EXPECT_TRUE( std::filesystem::exists( file ) )
                     << "cannot create '" << file.generic_string() << "'";
            }
            std::filesystem::create_directories( m_Root / "Resources/Assets" );
            m_Previous = std::filesystem::current_path();
            std::filesystem::current_path( m_Root );
        }

        ~AssetsSandbox()
        {
            std::error_code ec;
            std::filesystem::current_path( m_Previous, ec );
            std::filesystem::remove_all( m_Root, ec );
        }

        AssetsSandbox( const AssetsSandbox& )            = delete;
        AssetsSandbox& operator=( const AssetsSandbox& ) = delete;
        AssetsSandbox( AssetsSandbox&& )                 = delete;
        AssetsSandbox& operator=( AssetsSandbox&& )      = delete;

    private:
        std::filesystem::path m_Root;
        std::filesystem::path m_Previous;
    };
} // namespace Desert::TestSupport
