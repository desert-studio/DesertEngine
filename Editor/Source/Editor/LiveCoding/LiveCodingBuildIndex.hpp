#pragma once

#include <Editor/LiveCoding/LiveCodingTypes.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Desert::Editor::LiveCoding
{
    /// What the build made, as the build wrote it: LiveCoding.json (CMake, beside Editor.exe: build directory,
    /// compile_commands.json path, target -> image -> map) + compile_commands.json + the unity files it names.
    class BuildIndex
    {
    public:
        /// Reads @p manifest. Refuses with the reason when a file it names is missing.
        [[nodiscard]] static Common::ResultStr<BuildIndex> Load( const std::filesystem::path& manifest );

        [[nodiscard]] std::span<const ImageRecord>  Images() const { return m_Images; }
        [[nodiscard]] std::span<const SourceRecord> Sources() const { return m_Sources; }
        [[nodiscard]] std::optional<std::size_t>    FindSource( const std::filesystem::path& source ) const;

        /// The sources newer than the later of their image and their last patch (MarkPatched). Refuses when a
        /// header under the source roots is newer than the oldest image (names the header: a header edit needs a
        /// rebuild), or when a source's BuiltObject is newer than its image (a build compiled while the editor held
        /// the image: the objects no longer describe the running code).
        [[nodiscard]] Common::ResultStr<std::vector<std::size_t>> ChangedSources() const;

        /// The members of @p unitySource's batch, in the batch's order.
        [[nodiscard]] std::vector<std::size_t> BatchOf( const std::filesystem::path& unitySource ) const;

        /// The command line that compiles @p source alone into @p object: its translation unit's command with the
        /// unity file replaced by the source and /Fo replaced by @p object. Everything else is kept byte for byte.
        [[nodiscard]] std::string CommandFor( std::size_t source, const std::filesystem::path& object ) const;

        /// After an applied patch: these sources are now what the process runs (their mtime at compile time).
        void MarkPatched( std::span<const std::size_t> sources, std::span<const std::filesystem::file_time_type> at );

        /// <build>/LivePatch/<pid>: the backend compiles each patch into its own numbered directory under it.
        [[nodiscard]] const std::filesystem::path& PatchRoot() const { return m_PatchRoot; }

    private:
        std::vector<ImageRecord>                     m_Images;
        std::vector<SourceRecord>                    m_Sources;
        std::vector<std::filesystem::file_time_type> m_PatchedAt; // per source; min() = never patched
        std::vector<std::filesystem::path>           m_SourceRoots; // LiveCoding.json "SourceRoots"
        std::filesystem::path                        m_PatchRoot;   // <build>/LivePatch/<pid>
    };
} // namespace Desert::Editor::LiveCoding
