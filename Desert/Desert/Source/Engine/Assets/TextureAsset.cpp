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
    }

    Common::BoolResultStr TextureAsset::LoadFromFile()
    {
        // THE METADATA READ IS A PREFIX OF THE ASSET: header + TOC + key section, never the source bytes it
        // carries. The handle is the one frozen into the header at import (AF3) -- the number every `.demat`
        // names -- and the provenance key is ImportInfo's.
        const auto key = ReadTextureAssetKey( m_Metadata.Filepath );
        if ( !key.IsSuccess() )
            return Common::MakeError<bool>( key.GetError() );
        const std::string& sourceKey = key.GetValue().SourceFile;
        m_SourcePath                 = Common::AssetHandle::PathForStableKey( sourceKey ).string();
        AdoptHandleFromFile( key.GetValue().Handle, sourceKey );
        m_IsReadyForUse = true;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr TextureAsset::Unload()
    {
        // WAS `return BOOLSUCCESS;` — a body that released nothing AND left IsReadyForUse() true, which is
        // the worse half: EnsureLoaded short-circuits on that flag, so an "unloaded" texture would never
        // have been re-read.
        //
        // THE HANDLE IS DELIBERATELY NOT RESET. Load() replaced m_Metadata.Handle with the id inside the
        // `.tex`, and that id is the key TextureService and every `.demat` resolve against. Unload is not
        // the inverse of Load here and must not be: an evicted asset keeps its identity, or the reload
        // that follows would be a different asset.
        m_SourcePath.clear();
        m_SourcePath.shrink_to_fit();
        m_IsReadyForUse = false;
        return BOOLSUCCESS;
    }

} // namespace Desert::Assets
