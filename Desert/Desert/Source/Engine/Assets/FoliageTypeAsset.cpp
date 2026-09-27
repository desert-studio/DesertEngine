#include <Engine/Assets/FoliageTypeAsset.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

#include <filesystem>

namespace Desert::Assets
{
    FoliageTypeAsset::FoliageTypeAsset( AssetPriority priority, const Common::Filepath& filepath )
         : AssetBase( priority, filepath, AssetTypeID::FoliageType )
    {
        if ( const TextAssetIdentity identity = ReadTextAssetIdentity( m_Metadata.Filepath );
             !identity.Guid.IsNull() )
        {
            m_Guid = identity.Guid;
            AdoptHandleFromFile( identity.Handle(), identity.StableKey() );
        }
    }

    Common::BoolResultStr FoliageTypeAsset::LoadFromFile()
    {
        const std::filesystem::path file = ContentRegistry::FileToOpen( m_Metadata.Filepath );
        const std::string           path = file.string();

        std::string text;
        if ( const auto packed =
                  Common::Utils::VFS::Exists( file ) ? Common::Utils::VFS::ReadFile( file ) : std::nullopt;
             packed.has_value() )
            text = packed.value();
        else if ( auto read = Common::Utils::FileSystem::ReadFileContent( file ); read )
            text = read.ExtractValue();

        if ( text.empty() )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "foliage type '{}' is empty or could not be opened", path );
        }
        auto parsed = Serialization::ParseFoliageType( text );
        if ( !parsed )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "foliage type '{}' is not usable: {}", path,
                                                     parsed.GetError() );
        }
        m_Data = parsed.ExtractValue();
        if ( m_Data.Header )
            if ( const auto guid = Common::Content::AssetGuidFromText( m_Data.Header->Guid ); guid )
                m_Guid = guid.GetValue();
        m_Ready = true;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr FoliageTypeAsset::Unload()
    {
        m_Data  = Serialization::FoliageTypeData{};
        m_Ready = false;
        return BOOLSUCCESS;
    }

    AssetHandle FoliageTypeAsset::GetMeshHandle() const
    {
        if ( m_Data.Mesh.Guid.empty() )
            return AssetHandle::Null();
        const auto guid = Common::Content::AssetGuidFromText( m_Data.Mesh.Guid );
        if ( !guid )
            return AssetHandle::Null();
        return AssetHandle( static_cast<uint64_t>( Common::Content::HandleForGuid( guid.GetValue() ) ) );
    }

    Common::BoolResultStr FoliageTypeAsset::Save( const Common::Filepath&               filepath,
                                                  const Serialization::FoliageTypeData& data )
    {
        if ( auto ok = Serialization::SaveFoliageTypeFile( filepath, data ); !ok )
            return ok;
        LOG_INFO( "[Foliage] Foliage type written: '{}', mesh '{}'.", filepath.string(), data.Mesh.Path );
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
