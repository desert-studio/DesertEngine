#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/Serialization/VFXEffectType.hpp>

#include <string>

namespace Desert::Assets
{
    /**
     * @brief A VFX effect type on disk (`.dfxtype`) - UE's UNiagaraEffectType asset (VFX-11).
     *
     * ITS HANDLE IS HandleForGuid OF ITS HEADER GUID, adopted in the constructor, for the data channel's reason. A
     * `.dfx` names the type by its file STEM (VFXSystemData::EffectType); VFXWorld holds the asset and reads
     * GetData() every tick, so a re-read (AssetHotReload, the editor's Save) is what every world sees next frame.
     *
     * A FAILED RE-READ KEEPS THE LAST GOOD LEVELS: a hot-reload poll can land mid-write, and a type that turned
     * unreadable for one poll must not stop every system of the family. Only the first read can leave the asset
     * not ready.
     */
    class VFXEffectTypeAsset final : public AssetBase
    {
    public:
        VFXEffectTypeAsset( const Common::Filepath& filepath );

        Common::BoolResultStr LoadFromFile() override;
        Common::BoolResultStr Unload() override;

        [[nodiscard]] bool IsReadyForUse() const override
        {
            return m_Ready;
        }

        [[nodiscard]] const Serialization::VFXEffectTypeData& GetData() const
        {
            return m_Data;
        }

        /// The type's name, what a `.dfx` states: the file stem.
        [[nodiscard]] std::string TypeName() const;

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::VFXEffectType;
        }

        /// Writes a type to disk (creating the directory). Static because saving is what CREATES one.
        static Common::BoolResultStr Save( const Common::Filepath& filepath,
                                           const Serialization::VFXEffectTypeData& data );

    private:
        Serialization::VFXEffectTypeData m_Data;
        bool                             m_Ready = false;
    };
} // namespace Desert::Assets
