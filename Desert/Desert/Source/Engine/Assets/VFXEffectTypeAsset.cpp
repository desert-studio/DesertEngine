#include <Engine/Assets/VFXEffectTypeAsset.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

#include <filesystem>

namespace Desert::Assets
{
    VFXEffectTypeAsset::VFXEffectTypeAsset( const Common::Filepath& filepath )
         : AssetBase( filepath, AssetTypeID::VFXEffectType )
    {
        if ( const TextAssetIdentity identity = ReadTextAssetIdentity( m_Metadata.Filepath );
             !identity.Guid.IsNull() )
            AdoptHandleFromFile( identity.Handle(), identity.StableKey() );
    }

    Common::BoolResultStr VFXEffectTypeAsset::LoadFromFile()
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
            return Common::MakeFormattedError<bool>( "VFX effect type '{}' is empty or could not be opened", path );
        auto parsed = Serialization::ParseVFXEffectType( text );
        if ( !parsed )
            return Common::MakeFormattedError<bool>( "VFX effect type '{}' is not usable: {}", path,
                                                     parsed.GetError() );
        m_Data  = parsed.ExtractValue();
        m_Ready = true;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VFXEffectTypeAsset::Unload()
    {
        m_Data  = Serialization::VFXEffectTypeData{};
        m_Ready = false;
        return BOOLSUCCESS;
    }

    std::string VFXEffectTypeAsset::TypeName() const
    {
        return std::filesystem::path( m_Metadata.Filepath ).stem().string();
    }

    Common::BoolResultStr VFXEffectTypeAsset::Save( const Common::Filepath&                 filepath,
                                                    const Serialization::VFXEffectTypeData& data )
    {
        if ( auto ok = Serialization::SaveVFXEffectTypeFile( filepath, data ); !ok )
            return ok;
        LOG_INFO( "[VFX] Effect type written: '{}'.", filepath.string() );
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
