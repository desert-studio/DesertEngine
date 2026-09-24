#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/CloudLayout.hpp>

namespace Desert::Assets
{
    /**
     * @brief A painted cloud layout on disk (`.dclayout`), in the engine's ONE asset system.
     *
     * The same `AssetBase` / `AssetManager` as a texture, a material, a noise volume or a sculpted body, so
     * it gets the Content Browser, the drag-and-drop payload, the path-stable scene reference and the hot
     * reload for free. A second asset system for one file type would be a permanent second place to look
     * (contract §2.2).
     *
     * WHAT IT HOLDS. The decoded tables, pixels included — at the shipped 512 square that is 1.25 MiB, and
     * the ceiling of 1024 is 5.0 MiB. Kept rather than dropped after the first bake, because the BAKE is
     * what reads them and the bake re-runs whenever the region shifts: a layout dropped after upload would
     * have to be re-read from disk every time the camera walked far enough, which is the one thing a
     * camera-centric volume does constantly.
     *
     * NO GPU HERE, and unlike the noise and modelling volumes there is no GPU side at all: the painting is
     * consumed on the CPU by Assets::BakeCloudProceduralVolume and never reaches a sampler. That is the
     * whole cost argument of the phase — the march reads the baked volume it already read, and the painting
     * costs the hottest pass of the frame nothing.
     *
     * ITS HANDLE IS HandleForGuid OF ITS ENVELOPE GUID (container 2), adopted in the constructor like a
     * mesh's: the asset manager keys its handle lookup at creation. A file whose header cannot be read
     * (absent: Save is about to create it; or a bare version-1 file) keeps the path-derived handle
     * AssetBase gives it, and its load is refused by name.
     */
    class CloudLayoutAsset final : public AssetBase
    {
    public:
        CloudLayoutAsset( AssetPriority priority, const Common::Filepath& filepath );

        /// The envelope GUID this layout was created from; null when the file states none.
        [[nodiscard]] const Common::Content::AssetGuid& Guid() const
        {
            return m_Guid;
        }

        /// Reads and decodes the container. A file that is missing, truncated, corrupt or from an unknown
        /// version is an ERROR carrying the reason and the numbers — never a quietly empty layout, because
        /// an empty layout renders as a sky the artist's painting simply did not reach.
        Common::BoolResultStr LoadFromFile() override;
        Common::BoolResultStr Unload() override;

        bool IsReadyForUse() const override
        {
            return m_Ready;
        }

        const CloudLayoutData& GetLayout() const
        {
            return m_Layout;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::CloudLayout;
        }

        /**
         * @brief Writes a layout to disk in the container format, creating the directory if needed.
         *
         * Static because saving is what CREATES an asset — the Cloud Layout panel (View > Cloud Layout)
         * and Tools/CloudLayoutBaker both build a layout from a picture and write it, and only then does
         * the AssetManager get asked to load it back.
         */
        static Common::BoolResultStr Save( const Common::Filepath& filepath, const CloudLayoutData& layout );

    private:
        Common::Content::AssetGuid m_Guid;
        CloudLayoutData m_Layout;
        bool            m_Ready = false;
    };
} // namespace Desert::Assets
