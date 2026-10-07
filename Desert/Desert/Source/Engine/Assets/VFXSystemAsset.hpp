#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/Serialization/VFXSystem.hpp>

namespace Desert::Assets
{
    /**
     * @brief A VFX system on disk (`.dfx`) — UE's UNiagaraSystem with its emitters embedded (VFX-02).
     *
     * ITS HANDLE IS HandleForGuid OF ITS HEADER GUID, adopted in the constructor, so a reference survives
     * renames and is the same on every machine. No component names one yet (VFXComponent is VFX-03); the
     * asset is data only — compiling the stack and simulating belong to VFXWorld (VFX-01/04).
     */
    class VFXSystemAsset final : public AssetBase
    {
    public:
        VFXSystemAsset( const Common::Filepath& filepath );

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

        [[nodiscard]] const Serialization::VFXSystemData& GetData() const
        {
            return m_Data;
        }

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::VFXSystem;
        }

        /// Writes a VFX system to disk (creating the directory). Static because saving is what CREATES one.
        static Common::BoolResultStr Save( const Common::Filepath&                      filepath,
                                           const Serialization::VFXSystemData& data );

    private:
        Common::Content::AssetGuid            m_Guid;
        Serialization::VFXSystemData m_Data;
        bool                                  m_Ready = false;
    };
} // namespace Desert::Assets
