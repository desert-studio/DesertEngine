#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/Serialization/IesProfile.hpp>

#include <filesystem>

namespace Desert::Assets
{
    /**
     * @brief An IES light profile on disk (`.dies`, LIGHTS-RECT-IES): UE UTextureLightProfile. Imported from an
     * IESNA LM-63 `.ies` (Editor/Import/IesProfileImporter); a point, spot or rect light names it by handle
     * (`Asset<IesProfileAsset>`, stored as {Guid, Path}); Runtime::IesProfileService resamples it into the atlas.
     *
     * ITS HANDLE IS HandleForGuid OF ITS HEADER GUID, adopted in the constructor, as every text asset's.
     */
    class IesProfileAsset final : public AssetBase
    {
    public:
        IesProfileAsset( const Common::Filepath& filepath );

        Common::BoolResultStr LoadFromFile() override;
        Common::BoolResultStr Unload() override;

        [[nodiscard]] bool IsReadyForUse() const override
        {
            return m_Ready;
        }

        [[nodiscard]] const Common::Content::AssetGuid& Guid() const
        {
            return m_Guid;
        }

        [[nodiscard]] const Serialization::IesProfileData& GetData() const
        {
            return m_Data;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::IesProfile;
        }

        struct Parsed
        {
            Common::Content::AssetGuid    Guid;
            Serialization::IesProfileData Data;
        };

        /// Reads and parses a `.dies` (packed VFS first, then the file system) without an asset object: the
        /// light collectors' threads read a profile this way, and the scene load checks a reference's GUID.
        /// Refused: an unreadable file, a malformed one, a header with no GUID.
        [[nodiscard]] static Common::ResultStr<Parsed> ReadFile( const std::filesystem::path& dies );

    private:
        Common::Content::AssetGuid    m_Guid;
        Serialization::IesProfileData m_Data;
        bool                          m_Ready = false;
    };
} // namespace Desert::Assets
