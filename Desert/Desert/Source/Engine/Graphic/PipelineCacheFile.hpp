#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The driver pipeline-cache blob on disk: WHERE it lives, WHAT header guards it, and WHEN it is rewritten.
// Pure (std only, no device, no file I/O), so `Desert/Tests/Engine/PipelineCacheFile` pins all three without
// Vulkan. VulkanLogicalDevice owns the handle and does the reads and writes.
//
// Pattern from UE 5.8 FPipelineFileCacheManager / FVulkanPipelineCacheManager (not ported line by line: those
// sit on FArchive/IFileManager): the driver blob is a per-user, per-device artefact that lives in the user's
// directory, never in the install. A protected install (Program Files, a read-only .app) cannot be written,
// and before PSO1 the blob went into the project's DerivedDataCache, i.e. the install folder of a package.
//
// WHICH files in the directory live (PlanDirectory): a driver that hands back GPU binaries keeps exactly the
// file of this device + driver, and every other file of the same vendor:device (an older driver, another
// cache UUID) is deleted with its reason. MoltenVK hands back shader SOURCE (compressed MSL): seeding it
// Metal-compiles every entry on the thread that creates the cache, before the window opens (512-562 s
// measured on an 89 MB blob), while Metal's own shader cache already keeps the binaries — so on MoltenVK no
// file is kept, written or seeded, and every file of this vendor:device is deleted. Like UE, where the
// driver's binary cache is the driver's job and the PSO precompile never blocks the first frame.
namespace Desert::Graphic::PipelineCacheFile
{
    // Everything that makes a blob usable by exactly one GPU + driver. vendor/device/UUID are what the
    // driver's own header checks; the driver VERSION is not in that header, and a driver update can keep
    // the UUID while changing the code it generates — so it is part of our identity as well.
    struct DeviceIdentity
    {
        uint32_t                Vendor = 0;
        uint32_t                Device = 0;
        uint32_t                Driver = 0;
        std::array<uint8_t, 16> CacheUuid{};

        bool operator==( const DeviceIdentity& ) const = default;
    };

    // WHICH BINARY is running decides whose per-user directory the blob belongs to (PKG1).
    enum class Host
    {
        Editor, // the engine installation's user directory, `~/.desertengine` (ProjectContext::ConfigDirectory)
        Game    // the product's own, Common::Settings::GameUserDirectory(<.deproj Name>)
    };

    // Declared once by the host's CreateApplication before the Application exists, because the device reads
    // it while it is being created. Undeclared (a suite that makes a device) is a logged refusal to persist,
    // never a guessed directory.
    void                DeclareHost( Host host );
    std::optional<Host> DeclaredHost();

    // Game:   <userDir>/PipelineCache/ — userDir is already this product's, so nothing else shares it.
    // Editor: <userDir>/PipelineCache/<project Name>/ — one editor opens many projects, and two projects'
    //         pipelines have nothing in common: one shared blob would only grow and be rewritten by whichever
    //         project ran last. The Name is the `.deproj` Name, not the folder's (a moved or cloned project
    //         keeps its cache), made one path segment by Common::Settings::SanitizeProductName.
    std::filesystem::path Directory( Host host, const std::filesystem::path& userDir,
                                     std::string_view projectName );

    // "<vendor>-<device>-<driver>-<uuid>.bin", all lowercase hex: two GPUs in one machine, or a driver
    // update, get files of their own instead of overwriting one blob the other will throw away.
    std::string FileName( const DeviceIdentity& identity );

    // Header (magic, format version, identity, payload size, payload hash) followed by the driver's bytes.
    std::string Encode( const DeviceIdentity& identity, std::string_view driverBlob );

    // The driver's bytes, or the reason the file is not this device's intact blob (wrong magic/version,
    // another device or driver, truncated, corrupted). Never a partial payload.
    struct Decoded
    {
        std::optional<std::string> Blob;
        std::string                Refusal; // non-empty exactly when Blob is empty
    };
    Decoded Decode( const DeviceIdentity& identity, std::string_view file );

    uint64_t HashBytes( std::string_view bytes );

    // WHAT the driver hands back from vkGetPipelineCacheData decides whether a file is worth keeping at all.
    enum class DriverBlob : uint8_t
    {
        Binary,      // GPU binaries the driver maps back cheaply (every desktop Vulkan driver)
        ShaderSource // MoltenVK: compressed MSL; seeding Metal-compiles every entry on the calling thread
                     // (MVKPipelineCache::readData), Metal's own cache already keeps the binaries
    };

    // A file in Directory() that this run deletes, and why (one log line each).
    struct StaleFile
    {
        std::filesystem::path Path;
        std::string           Reason;
    };

    // What this run does with the directory: the one file it seeds from and writes (empty = none on this driver,
    // Refusal says why), and every other file of THIS vendor:device that is dead. Files of another vendor:device
    // (a second GPU) and non-".bin" entries are left alone. Pure: the caller lists the directory and deletes.
    struct DirectoryPlan
    {
        std::optional<std::filesystem::path> Keep;
        std::string                          Refusal; // non-empty exactly when Keep is empty
        std::vector<StaleFile>               Delete;
    };
    [[nodiscard]] DirectoryPlan PlanDirectory( DriverBlob blob, const DeviceIdentity& identity,
                                               const std::filesystem::path&              dir,
                                               const std::vector<std::filesystem::path>& filesInDir );

    // WHEN to rewrite the file during a run (not only at a clean exit, which a crash or a kill never reaches):
    // when pipelines were built since the last write AND at least `interval` has passed since it. The first
    // write after start is not throttled.
    class PersistSchedule
    {
    public:
        explicit PersistSchedule( std::chrono::steady_clock::duration interval ) : m_Interval( interval )
        {
        }

        // True when a write is due now; records `builtSoFar`/`now` as the last write when it is.
        bool Due( uint64_t builtSoFar, std::chrono::steady_clock::time_point now );

        [[nodiscard]] std::chrono::steady_clock::duration Interval() const
        {
            return m_Interval;
        }

    private:
        std::chrono::steady_clock::duration                  m_Interval;
        uint64_t                                             m_BuiltAtLastWrite = 0;
        std::optional<std::chrono::steady_clock::time_point> m_LastWrite;
    };
} // namespace Desert::Graphic::PipelineCacheFile
