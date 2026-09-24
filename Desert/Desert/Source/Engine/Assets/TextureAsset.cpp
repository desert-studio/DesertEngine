#include <Engine/Assets/TextureAsset.hpp>

#include <Engine/Assets/TextureSourceAsset.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

namespace Desert::Assets
{
    TextureAsset::TextureAsset( AssetPriority priority, const Common::Filepath& filepath )
         : AssetBase( priority, filepath, AssetTypeID::Texture2D )
    {
        // THE TEXTURE'S IDENTITY IS ITS HEADER GUID, adopted HERE and not in the load (as the mesh does,
        // MeshAsset.hpp): the asset manager keys its handle lookup at creation, so a handle adopted by Load
        // would arrive after the path-derived one had been handed out, and a create with
        // loadAfterCreate=false would stay findable only by the wrong number.
        // Absence is an answer (a create may name a file about to be written): such an asset keeps the
        // path-derived handle, and its Load refuses by name rather than change identity under the lookup.
        if ( !Common::Utils::FileSystem::Exists( m_Metadata.Filepath ) )
            return;
        const auto key = ReadTextureAssetKey( m_Metadata.Filepath );
        if ( !key.IsSuccess() )
        {
            // Not adopted: Load reads the same key and returns this error to the caller.
            LOG_ERROR( "[TextureAsset] '{}' states no readable identity: {}", m_Metadata.Filepath.string(),
                       key.GetError() );
            return;
        }
        if ( key.GetValue().Guid.IsNull() )
            return;
        m_Guid = key.GetValue().Guid;
        // The handle is indexed under THIS file, not the image it was imported from: SourceFile is provenance
        // only and is gone from the tree after import, so indexing the handle under it would make every
        // reference written through the index (TextureSlotToPath) name a file nothing can open.
        AdoptHandleFromFile( Common::UUID( static_cast<uint64_t>( Common::Content::HandleForGuid( m_Guid ) ) ),
                             Common::AssetHandle::StableKeyForPath( m_Metadata.Filepath ) );
    }

    Common::BoolResultStr TextureAsset::LoadFromFile()
    {
        // THE METADATA READ IS A PREFIX OF THE ASSET: header + TOC + key section, never the source bytes it
        // carries. The identity was adopted by the constructor; Load only checks that the file still states
        // it -- a file replaced by another asset since creation would otherwise be served under the old key.
        const auto key = ReadTextureAssetKey( m_Metadata.Filepath );
        if ( !key.IsSuccess() )
            return Common::MakeError<bool>( key.GetError() );
        const Common::Content::AssetGuid& stated = key.GetValue().Guid;
        if ( stated != m_Guid )
            return Common::MakeFormattedError<bool>(
                 "texture '{}' states GUID {:016x}{:016x} but was created as {:016x}{:016x} (handle {:016x}; "
                 "zero = the file was absent or unreadable at creation). An asset's identity is fixed when it "
                 "is created, so it must be created again",
                 m_Metadata.Filepath.string(), stated.Hi, stated.Lo, m_Guid.Hi, m_Guid.Lo,
                 static_cast<uint64_t>( m_Metadata.Handle ) );
        m_SourcePath    = Common::AssetHandle::PathForStableKey( key.GetValue().SourceFile ).string();
        m_IsReadyForUse = true;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr TextureAsset::Unload()
    {
        // WAS `return BOOLSUCCESS;` — a body that released nothing AND left IsReadyForUse() true, which is
        // the worse half: EnsureLoaded short-circuits on that flag, so an "unloaded" texture would never
        // have been re-read.
        //
        // THE HANDLE IS DELIBERATELY NOT RESET. The constructor adopted HandleForGuid of the header GUID,
        // and that is the key TextureService and every reference resolve against. Unload is not
        // the inverse of Load here and must not be: an evicted asset keeps its identity, or the reload
        // that follows would be a different asset.
        m_SourcePath.clear();
        m_SourcePath.shrink_to_fit();
        m_IsReadyForUse = false;
        return BOOLSUCCESS;
    }

} // namespace Desert::Assets
