#include <Engine/Assets/Skybox/SkyboxAsset.hpp>

#include <Engine/Assets/TextureSourceAsset.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

namespace Desert::Assets
{
    SkyboxAsset::SkyboxAsset( AssetPriority priority, const Common::Filepath& filepath )
         : AssetBase( priority, filepath, AssetTypeID::Skybox )
    {
        // THE SKYBOX'S IDENTITY IS ITS PANORAMA ASSET'S HEADER GUID, adopted here for TextureAsset's reason
        // (TextureAsset.cpp): the manager keys its lookup at creation. A material's cube slot names the
        // skybox by that GUID (MATL 3), so a path-derived handle would be a number no slot can reach. An
        // absent file keeps the path-derived handle and Load refuses it by name.
        if ( !Common::Utils::FileSystem::Exists( m_Metadata.Filepath ) )
            return;
        const auto key = ReadTextureAssetKey( m_Metadata.Filepath );
        if ( !key.IsSuccess() )
        {
            LOG_ERROR( "[SkyboxAsset] '{}' states no readable identity: {}", m_Metadata.Filepath.string(),
                       key.GetError() );
            return;
        }
        if ( key.GetValue().Guid.IsNull() )
            return;
        m_Guid = key.GetValue().Guid;
        AdoptHandleFromFile( Common::UUID( static_cast<uint64_t>( Common::Content::HandleForGuid( m_Guid ) ) ),
                             Common::AssetHandle::StableKeyForPath( m_Metadata.Filepath ) );
    }

    Common::BoolResultStr SkyboxAsset::LoadFromFile()
    {
        // THE PANORAMA IS NOT READ HERE, and that is by design: EnvironmentManager::Create uploads the
        // .hdr and runs the radiance/irradiance/prefilter bakes on the GPU, so this asset holds nothing
        // but the identity of a file. What it OWES its caller, therefore, is the one thing it can
        // establish without a device — that the file it names is actually there.
        //
        // It used to owe nothing: the body was `m_ReadyForUse = true; return BOOLSUCCESS;` with the only
        // check commented out (against an m_TextureAsset member this class does not have). A skybox
        // whose .hdr had been moved, renamed or left out of a package therefore loaded, registered,
        // and reported ready — and the sky came out black with every diagnostic in the editor saying
        // the skybox was fine. Exists() asks the mounted .dpak as well as the disk, so a packaged game
        // answers this identically.
        if ( !Common::Utils::FileSystem::Exists( m_Metadata.Filepath ) )
        {
            m_ReadyForUse = false;
            LOG_ERROR( "[SkyboxAsset] '{}' is not on disk and not in a mounted pak — the skybox is not "
                       "loaded and the scene has no environment from it.",
                       m_Metadata.Filepath.string() );
            return Common::MakeFormattedError( "skybox '{}' is not on disk and not in a mounted pak",
                                               m_Metadata.Filepath.string() );
        }

        m_ReadyForUse = true;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr SkyboxAsset::Unload()
    {
        // WAS `return BOOLSUCCESS;` — and this type owns no data at all, so a no-op is ALMOST right. What
        // it got wrong is the one line it had: leaving `m_ReadyForUse` true made the eviction unobservable
        // AND meant the file-existence check above — the entire point of Load() for this type — would
        // never be re-asked. One bool, and it is the whole of the asset.
        m_ReadyForUse = false;
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets