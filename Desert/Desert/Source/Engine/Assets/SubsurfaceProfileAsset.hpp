#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/Serialization/SubsurfaceProfile.hpp>

namespace Desert::Assets
{
    /**
     * @brief A Burley subsurface profile on disk (`.dsssprofile`, SSS-PROFILE): UE USubsurfaceProfile
     * (SubsurfaceProfile.h:177-190). A material of the SubsurfaceProfile shading model names one by header GUID
     * (MaterialData::SubsurfaceProfile); Graphic::SubsurfaceProfileRegistry encodes it into the profile atlas.
     *
     * ITS HANDLE IS HandleForGuid OF ITS HEADER GUID, adopted in the constructor, as every text asset's.
     */
    class SubsurfaceProfileAsset final : public AssetBase
    {
    public:
        SubsurfaceProfileAsset( const Common::Filepath& filepath );

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

        [[nodiscard]] const Serialization::SubsurfaceProfileData& GetData() const
        {
            return m_Data;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::SubsurfaceProfile;
        }

        /// Writes a preset to disk (creating the directory). Static because saving is what CREATES one.
        static Common::BoolResultStr Save( const Common::Filepath&                 filepath,
                                           const Serialization::SubsurfaceProfileData& data );

    private:
        Common::Content::AssetGuid       m_Guid;
        Serialization::SubsurfaceProfileData m_Data;
        bool                             m_Ready = false;
    };
} // namespace Desert::Assets
