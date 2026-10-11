#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/Serialization/VFXDataChannel.hpp>

#include <filesystem>
#include <string_view>

namespace Desert::Assets
{
    /**
     * @brief A VFX data channel on disk (`.dfxch`) - UE's UNiagaraDataChannel asset (VFX-10b).
     *
     * ITS HANDLE IS HandleForGuid OF ITS HEADER GUID, adopted in the constructor, so a reference survives renames
     * and is the same on every machine. The asset is the channel's LAYOUT only; the entries of a frame live in
     * the scene's VFXWorld (VFX::VFXDataChannels::Register takes this asset or its handle). The channel's name -
     * what a module binding ("DataChannel.<name>") and VFX.writeChannel say - is the file stem.
     */
    class VFXDataChannelAsset final : public AssetBase
    {
    public:
        VFXDataChannelAsset( const Common::Filepath& filepath );

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

        [[nodiscard]] const Serialization::VFXDataChannelData& GetData() const
        {
            return m_Data;
        }

        /// The channel's name: the file stem.
        [[nodiscard]] std::string ChannelName() const;

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::VFXDataChannel;
        }

        /// The file of the channel NAMED @p name (UE's asset-name convention, as a script names `IA_Jump`): the
        /// project's VFX content root + `<name>.dfxch`. A name that is a path, has an extension or is not an
        /// identifier is refused - scripts name channels, they do not address files.
        static Common::ResultStr<std::filesystem::path> PathForName( std::string_view name );

        /// Writes a channel to disk (creating the directory). Static because saving is what CREATES one.
        static Common::BoolResultStr Save( const Common::Filepath&                  filepath,
                                           const Serialization::VFXDataChannelData& data );

    private:
        Common::Content::AssetGuid        m_Guid;
        Serialization::VFXDataChannelData m_Data;
        bool                              m_Ready = false;
    };
} // namespace Desert::Assets
