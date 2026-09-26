#pragma once

// A THROWAWAY DERIVED DATA CACHE FOR ONE TEST (or one suite, held in main).
//
// Common::DDC::Put refuses when no project is open and machine.json names no absolute DerivedDataCachePath:
// the root would be relative to the working directory, and a suite run from the tree root used to leave
// DerivedDataCache/Buckets/... in the checkout (09-26, found on Windows). A test that needs the cache says so
// explicitly: this points the machine setting at <temp>/<suite>-ddc-<n>, and on destruction restores the
// setting and removes the directory. The name carries a random part: several suites run in parallel and
// must not remove each other's cache.

#include <Common/Settings/MachineSettings.hpp>

#include <atomic>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

namespace Desert::TestSupport
{
    class DerivedDataSandbox
    {
    public:
        explicit DerivedDataSandbox( std::string_view suite )
             : m_Previous( Common::Settings::MachineSettings::Get().DerivedDataCachePath )
        {
            static std::atomic<int> sequence = 0;
            m_Root                           = std::filesystem::temp_directory_path() /
                     ( std::string( suite ) + "-ddc-" + std::to_string( std::random_device{}() ) + "-" +
                       std::to_string( sequence++ ) );
            std::error_code ec;
            std::filesystem::remove_all( m_Root, ec );
            Common::Settings::MachineSettings::Get().DerivedDataCachePath = m_Root.string();
        }

        ~DerivedDataSandbox()
        {
            Common::Settings::MachineSettings::Get().DerivedDataCachePath = m_Previous;
            std::error_code ec;
            std::filesystem::remove_all( m_Root, ec );
        }

        [[nodiscard]] const std::filesystem::path& Root() const
        {
            return m_Root;
        }

        DerivedDataSandbox( const DerivedDataSandbox& )            = delete;
        DerivedDataSandbox& operator=( const DerivedDataSandbox& ) = delete;
        DerivedDataSandbox( DerivedDataSandbox&& )                 = delete;
        DerivedDataSandbox& operator=( DerivedDataSandbox&& )      = delete;

    private:
        std::string           m_Previous;
        std::filesystem::path m_Root;
    };
} // namespace Desert::TestSupport
