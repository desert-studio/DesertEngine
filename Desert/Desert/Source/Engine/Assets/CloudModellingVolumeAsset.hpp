#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/CloudModellingVolume.hpp>

namespace Desert::Assets
{
    /**
     * @brief A sculpted cloud body on disk (`.dcmv`), in the engine's ONE asset system.
     *
     * The same `AssetBase` / `AssetManager` as a texture, a material or a noise volume, so it gets the
     * Content Browser, the drag-and-drop payload, the path-stable scene reference and the hot reload for
     * free. A second asset system for one file type would have been a week of plumbing and a permanent
     * second place to look (contract §2.2).
     *
     * WHAT IT HOLDS. The decoded volume, voxels included — 4 MiB. Kept rather than dropped after upload
     * because the renderer re-uploads it whenever the slot changes or the file is touched on disk, and
     * because the recipe travels with the voxels: the sculpting tool of phase A1 opens a `.dcmv` and
     * edits the lumps that produced it, which is only possible if they were kept.
     *
     * NO GPU HERE. The asset layer knows nothing about `Image3D`; `Runtime::CloudModellingService` is what
     * turns these bytes into a volume texture. That is the layer rule (`Engine/Assets` does not know about
     * Vulkan), and it is also what lets this class be covered by a GPU-free test.
     */
    class CloudModellingVolumeAsset final : public AssetBase
    {
    public:
        CloudModellingVolumeAsset( AssetPriority priority, const Common::Filepath& filepath );

        /// Reads and decodes the container. A file that is missing, truncated, corrupt or from an unknown
        /// version is an ERROR carrying the reason and the numbers — never a quietly empty volume, because
        /// an empty volume renders as a hero cloud that is simply not there.
        Common::BoolResultStr LoadFromFile() override;
        Common::BoolResultStr Unload() override;

        bool IsReadyForUse() const override
        {
            return m_Ready;
        }

        const CloudModellingVolumeData& GetVolume() const
        {
            return m_Volume;
        }

        /**
         * @brief Bumped by every successful Load.
         *
         * The service holds the last revision it uploaded. Comparing the two is the whole re-upload
         * decision, and it answers the right question where a `bool uploaded` would answer the wrong one
         * the moment the file changed underneath a handle that did not.
         */
        uint32_t GetRevision() const
        {
            return m_Revision;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::CloudModellingVolume;
        }

        /**
         * @brief Writes a volume to disk in the container format, creating the directory if needed.
         *
         * Static because saving is what CREATES an asset — `Tools/CloudVolumeBaker` bakes a volume and
         * writes it, and only then does the AssetManager get asked to load it back. Writing through an
         * instance would mean an instance had to exist for a file that does not.
         */
        static Common::BoolResultStr Save( const Common::Filepath&         filepath,
                                           const CloudModellingVolumeData& volume );

        /// The GUID the `.dcmv` envelope header at @p filepath states (VFS first); null when the file is
        /// absent, bare or not a modelling volume. The constructor adopts it; Save keeps it on a re-bake.
        static Common::Content::AssetGuid ReadCloudModellingVolumeGuid( const Common::Filepath& filepath );

    private:
        CloudModellingVolumeData m_Volume;
        bool                     m_Ready    = false;
        uint32_t                 m_Revision = 0;
    };
} // namespace Desert::Assets
