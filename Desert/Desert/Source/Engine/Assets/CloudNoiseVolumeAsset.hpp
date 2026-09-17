#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/CloudNoiseVolume.hpp>

namespace Desert::Assets
{
    /**
     * @brief A cloud noise volume on disk (`.dcnv`), in the engine's ONE asset system.
     *
     * The same `AssetBase` / `AssetManager` as a texture or a material, so it gets the Content Browser, the
     * drag-and-drop payload, the path-stable scene reference and the hot reload for free. A second asset
     * system for one file type would have been a week of plumbing and a permanent second place to look.
     *
     * WHAT IT HOLDS. The decoded volume, voxels included — 8 MiB at the default resolution. Kept rather
     * than dropped after upload because the renderer re-uploads it whenever the slot changes or the file is
     * touched on disk, and because the editor panel previews slices of exactly the bytes that were loaded
     * rather than of a regenerated approximation of them.
     *
     * NO GPU HERE. The asset layer knows nothing about `Image3D`; `Graphic::System::VolumetricCloudRenderer`
     * is what turns these bytes into a volume texture. That is the layer rule (`Engine/Assets` does not
     * know about Vulkan), and it is also what lets this class be covered by a GPU-free test.
     */
    class CloudNoiseVolumeAsset final : public AssetBase
    {
    public:
        CloudNoiseVolumeAsset( AssetPriority priority, const Common::Filepath& filepath );

        /// Reads and decodes the container. A file that is missing, truncated, corrupt or from an unknown
        /// version is an ERROR carrying the reason and the numbers — never a quietly empty volume, because
        /// an empty volume renders as a sky that merely looks wrong.
        Common::BoolResultStr LoadFromFile() override;
        Common::BoolResultStr Unload() override;

        bool IsReadyForUse() const override
        {
            return m_Ready;
        }

        const CloudNoiseVolumeData& GetVolume() const
        {
            return m_Volume;
        }

        /**
         * @brief Bumped by every successful Load.
         *
         * The renderer holds the last revision it uploaded. Comparing the two is the whole re-upload
         * decision, and it answers the right question where a `bool uploaded` would answer the wrong one
         * the moment the file changed underneath a handle that did not.
         */
        uint32_t GetRevision() const
        {
            return m_Revision;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::CloudNoiseVolume;
        }

        /**
         * @brief Writes a volume to disk in the container format, creating the directory if needed.
         *
         * Static because saving is what CREATES an asset — the editor panel bakes a volume and writes it,
         * and only then does the AssetManager get asked to load it back. Writing through an instance would
         * mean an instance had to exist for a file that does not.
         */
        static Common::BoolResultStr Save( const Common::Filepath& filepath, const CloudNoiseVolumeData& volume );

    private:
        CloudNoiseVolumeData m_Volume;
        bool                 m_Ready    = false;
        uint32_t             m_Revision = 0;
    };
} // namespace Desert::Assets
