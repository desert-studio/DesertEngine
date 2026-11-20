#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/SparseVolume.hpp>

#include <filesystem>

namespace Desert::Assets
{
    /**
     * @brief A baked sparse volume on disk (`.dsvol`): UE USparseVolumeTexture. A heterogeneous volume names it by
     * handle (`Asset<SparseVolumeAsset>`, stored as {Guid, Path}); Graphic::System::HeterogeneousVolumeRenderer
     * reads it (ReadFile, on a background job) into its shared brick atlas the first time a volume using it is on
     * screen. The format, the brick builder and the `.vol` importer are SparseVolume.hpp.
     *
     * ITS HANDLE IS HandleForGuid OF ITS ENVELOPE GUID, adopted in the constructor, as the cloud noise volume's.
     */
    class SparseVolumeAsset final : public AssetBase
    {
    public:
        SparseVolumeAsset( const Common::Filepath& filepath );

        Common::BoolResultStr LoadFromFile() override;
        Common::BoolResultStr Unload() override;

        [[nodiscard]] bool IsReadyForUse() const override
        {
            return m_Ready;
        }

        [[nodiscard]] const SparseVolumeData& GetData() const
        {
            return m_Data;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::SparseVolume;
        }

        /// Reads and decodes a `.dsvol` (packed VFS first, then the file system) without an asset object: the
        /// renderer's residency jobs read a volume this way. Refused: an unreadable file, a malformed one.
        [[nodiscard]] static Common::ResultStr<SparseVolumeData> ReadFile( const std::filesystem::path& dsvol );

        /// The envelope GUID of a `.dsvol`, header only; null when the file has no readable SparseVolume header.
        [[nodiscard]] static Common::Content::AssetGuid ReadGuid( const std::filesystem::path& dsvol );

    private:
        SparseVolumeData m_Data;
        bool             m_Ready = false;
    };
} // namespace Desert::Assets
