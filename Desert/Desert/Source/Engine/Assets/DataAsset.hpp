#pragma once

#include <Engine/Assets/AssetBase.hpp>
#include <Engine/Assets/DataAssetTypeRegistry.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/TextAssetHeader.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <typeindex>

namespace Desert::Assets
{
    /**
     * @brief A game's typed data asset on disk (`.dasset`, ASSET-TYPES-OPEN; UE UPrimaryDataAsset): one payload
     * of a reflected struct the file names (Serialization/DataAsset.hpp), registered by a game module in
     * DataAssetTypeRegistry. A reader asks for its struct: `asset->Get<WeaponDef>()` answers the payload or an
     * error naming both types - never a reinterpretation.
     *
     * ITS HANDLE IS HandleForGuid OF ITS HEADER GUID, adopted in the constructor, so a component's reference
     * survives renames and is the same on every machine.
     *
     * A RELOAD KEEPS THE OBJECT: a re-read of the same type assigns into the payload every reader already points
     * at (DataAssetType::Assign), so a `const T*` taken before a hot reload reads the new values after it.
     */
    class DataAsset final : public AssetBase
    {
    public:
        explicit DataAsset( const Common::Filepath& filepath );

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

        /// The registered payload type, or nullptr before a load.
        [[nodiscard]] const DataAssetType* PayloadType() const
        {
            return m_Type;
        }

        /// The payload as T: refused (as a value) when the asset is not loaded or holds another type.
        template <typename T>
        [[nodiscard]] Common::ResultStr<const T*> Get() const
        {
            if ( auto checked = Check( std::type_index( typeid( T ) ) ); !checked )
                return Common::MakeError<const T*>( checked.GetError() );
            return Common::MakeSuccess( static_cast<const T*>( m_Payload.get() ) );
        }

        /// The payload for an editor that changes it in place and then calls Save(); null before a load.
        [[nodiscard]] void* MutablePayload()
        {
            return m_Payload.get();
        }

        /// Counts loads and reloads, so a reader that derived something from the payload can tell it is stale.
        [[nodiscard]] uint64_t Revision() const
        {
            return m_Revision;
        }

        /// Writes the current payload back to this asset's file, keeping its GUID.
        Common::BoolResultStr Save() const;

        /// Creates a `.dasset` holding a default-constructed @p typeName (the Content Browser's
        /// «New Data Asset»). Refused for an unregistered type or an existing file.
        static Common::BoolResultStr CreateDefault( const Common::Filepath& filepath,
                                                    const std::string&      typeName );

        static AssetTypeID GetTypeID()
        {
            return AssetTypeID::DataAsset;
        }

    private:
        Common::BoolResultStr Check( std::type_index wanted ) const;

        Common::Content::AssetGuid                                m_Guid;
        std::optional<Common::Content::TextAssetHeaderSerialized> m_Header;
        const DataAssetType*                                      m_Type = nullptr;
        std::shared_ptr<void>                                     m_Payload;
        uint64_t                                                  m_Revision = 0;
        bool                                                      m_Ready    = false;
    };
} // namespace Desert::Assets
