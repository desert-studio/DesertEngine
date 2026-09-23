#include <Engine/Assets/TextureAsset.hpp>

#include <Engine/Assets/Serialization/TextureBinary.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

namespace Desert::Assets
{
    namespace
    {
        // THE METADATA READ, AND WHY IT IS A PREFIX. `TextureService::Get` and `GetSourcePath` both call
        // Load() and both say in their own comments that it reads the metadata and not the pixels. That
        // was free while a `.tex` was 133 bytes of JSON; the payload made it a promise somebody has to
        // keep, and this function is where it is kept. One read of `kTextureBinaryPrefixBytes` covers
        // every texture the cook produces; a longer source key costs one more read at the exact size the
        // header itself reports, and the suite drives that branch.
        Common::ResultStr<Serialization::TextureBinaryHeaderInfo>
        ReadCookedHeader( const Common::Filepath& filepath )
        {
            auto prefix = Common::Utils::FileSystem::ReadFileContentPrefix(
                 filepath, Serialization::kTextureBinaryPrefixBytes );
            if ( !prefix.IsSuccess() )
                return Common::MakeError<Serialization::TextureBinaryHeaderInfo>( prefix.GetError() );

            const uint64_t needed = Serialization::TextureBinaryMetadataBytes( prefix.GetValue() );
            if ( needed > prefix.GetValue().size() )
            {
                auto wider = Common::Utils::FileSystem::ReadFileContentPrefix(
                     filepath, static_cast<std::size_t>( needed ) );
                if ( !wider.IsSuccess() )
                    return Common::MakeError<Serialization::TextureBinaryHeaderInfo>( wider.GetError() );
                return Serialization::DecodeTextureHeader( wider.GetValue(), filepath.string() );
            }

            return Serialization::DecodeTextureHeader( prefix.GetValue(), filepath.string() );
        }
    } // namespace

    TextureAsset::TextureAsset( AssetPriority priority, const Common::Filepath& filepath )
         : AssetBase( priority, filepath, AssetTypeID::Texture2D )
    {
    }

    Common::BoolResultStr TextureAsset::LoadFromFile()
    {
        const auto header = ReadCookedHeader( m_Metadata.Filepath );
        if ( !header.IsSuccess() )
            return Common::MakeError( header.GetError() );

        // The file stores the source's PLACE in the project (`assets:Textures/T.png`), not a path — that is
        // what lets a committed .tex be read on a machine whose checkout sits anywhere. Expanded here,
        // once, through the same root table that wrote it. A stored value with no known tag passes through
        // unchanged; that covers sources genuinely outside the project.
        //
        // NOTHING IN THE TEXTURE PATH OPENS IT ANY MORE. Until B17 this was the path the pixels came from,
        // decoded from PNG on every load; the pixels are in the `.tex` now. It survives because three
        // other consumers address the SOURCE and not the texture: the animated-image service decodes a
        // GIF's frames from it, the video service its stream, and the editor labels a texture by the name
        // an artist gave the file.
        m_SourcePath = Common::AssetHandle::PathForStableKey( header.GetValue().SourcePath ).string();

        // The cooked `.tex` carries an id of its own, which additionally survives a rename of the file, so
        // it wins over the path-derived handle AssetBase installed. GetHandle() reads this same field, so
        // TextureService and the editor cannot disagree about which id a texture has.
        //
        // IT IS BOUND TO THE SOURCE IMAGE'S KEY, NOT THIS FILE'S. TextureImporter mints this number as
        // `FromCookedPath(<the source image>)`, so the source is the file the number is a statement
        // about; binding it to the `.tex` would put a true-looking wrong answer in the index — the one
        // kind of entry worse than a missing one. The `.tex`'s own path-derived handle is separately in
        // the index already, from the constructor, and it still names the `.tex`.
        const std::string sourceKey = Common::AssetHandle::StableKeyForPath( m_SourcePath );
        AdoptHandleFromFile( header.GetValue().Handle, sourceKey );

        // THE RELATION NOBODY OWNED. The field above is not a free identity: TextureImporter mints it as
        // `AssetHandle::FromCookedPath(<the source image>)`, so the number in the file and the number the
        // next cook will derive are two statements of one quantity — and until this check existed nothing
        // compared them. A handle miss has no filename in it, which is why the message carries both
        // numbers and the command that fixes them.
        //
        // The stored value is still what wins. Substituting the derived id here would be a migration at
        // load that never writes itself back (DC §4.3) — and it would repair the symptom on the one
        // machine that does not need repairing, leaving the file wrong.
        //
        // Only when the source is project-relative. A `.tex` whose SourcePath is an absolute path outside
        // every content root has no derivable identity to compare against — TextureImporter has already
        // warned at cook time that such a file resolves on one machine only.
        if ( Common::AssetHandle::IsProjectRelativeKey( sourceKey ) )
        {
            const auto derived = Common::AssetHandle::FromKey( sourceKey );
            if ( static_cast<uint64_t>( derived ) != static_cast<uint64_t>( header.GetValue().Handle ) )
            {
                LOG_ERROR( "[Textures] '{0}' stores Handle={1}, but its own source '{2}' derives {3}. The "
                           "stored number is what every reference resolves against, so it is kept — but the "
                           "next cook of this texture will mint {3} and every `.demat` naming {1} will then "
                           "resolve to nothing, with no filename anywhere in the log. Re-cook it (Assets > "
                           "Rebuild Cooked Assets) and re-point the materials that name {1} at {3}.",
                           m_Metadata.Filepath.string(), static_cast<uint64_t>( header.GetValue().Handle ),
                           sourceKey, static_cast<uint64_t>( derived ) );
            }
        }

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
