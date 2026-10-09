#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/Serialization/InputAssets.hpp>

namespace Desert::Assets
{
    /**
     * @brief An input action on disk (`.deinputaction`) — UE's UInputAction (GP1b).
     *
     * ITS HANDLE IS HandleForGuid OF ITS HEADER GUID, adopted in the constructor, so a mapping context's
     * reference (by GUID) and the handle the manager files it under are the same number on every machine.
     * The asset is data only: evaluating it is Input::EnhancedInputSubsystem's, which copies the data in.
     */
    class InputActionAsset final : public AssetBase
    {
    public:
        InputActionAsset( const Common::Filepath& filepath );

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

        [[nodiscard]] const Serialization::InputActionData& GetData() const
        {
            return m_Data;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::InputAction;
        }

        /// Writes an action to disk (creating the directory). Static because saving is what CREATES one.
        static Common::BoolResultStr Save( const Common::Filepath&               filepath,
                                           const Serialization::InputActionData& data );

    private:
        Common::Content::AssetGuid     m_Guid;
        Serialization::InputActionData m_Data;
        bool                           m_Ready = false;
    };

    /**
     * @brief An input mapping context on disk (`.deinputcontext`) — UE's UInputMappingContext (GP1b).
     *
     * Same identity rule as InputActionAsset. Its header's Dependencies are the GUIDs of the actions it maps;
     * the player adds it to its subsystem (EnhancedInputPlayerComponent, Lua Input.addContext).
     */
    class InputMappingContextAsset final : public AssetBase
    {
    public:
        InputMappingContextAsset( const Common::Filepath& filepath );

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

        [[nodiscard]] const Serialization::InputMappingContextData& GetData() const
        {
            return m_Data;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::InputMappingContext;
        }

        static Common::BoolResultStr Save( const Common::Filepath&                       filepath,
                                           const Serialization::InputMappingContextData& data );

    private:
        Common::Content::AssetGuid             m_Guid;
        Serialization::InputMappingContextData m_Data;
        bool                                   m_Ready = false;
    };
} // namespace Desert::Assets
