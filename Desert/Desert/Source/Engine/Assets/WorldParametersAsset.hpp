#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/Serialization/WorldParameters.hpp>

namespace Desert::Assets
{
    /**
     * @brief A world parameter collection on disk (`.dworldparams`) — UE's UMaterialParameterCollection as an
     * asset of the engine's one asset system (Serialization/WorldParameters.hpp). The scene names one
     * (Core::SceneSettings::WorldParameters); Runtime::WorldParametersService reads it for the world's instance.
     *
     * ITS HANDLE IS HandleForGuid OF ITS HEADER GUID, adopted in the constructor, so a scene's reference
     * survives renames and is the same on every machine.
     */
    class WorldParametersAsset final : public AssetBase
    {
    public:
        WorldParametersAsset( const Common::Filepath& filepath );

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

        [[nodiscard]] const Serialization::WorldParametersData& GetData() const
        {
            return m_Data;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::WorldParameters;
        }

        /// Writes a collection to disk (creating the directory). Static because saving is what CREATES one.
        static Common::BoolResultStr Save( const Common::Filepath&                   filepath,
                                           const Serialization::WorldParametersData& data );

    private:
        Common::Content::AssetGuid         m_Guid;
        Serialization::WorldParametersData m_Data;
        bool                               m_Ready = false;
    };
} // namespace Desert::Assets
