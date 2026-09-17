#include <Engine/Assets/TextureAsset.hpp>

#include <Engine/Assets/Serialization/Texture.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>

#include <rflcpp/rfl.hpp>
#include <rflcpp/rfl/json.hpp>

namespace Desert::Assets
{
    TextureAsset::TextureAsset( AssetPriority priority, const Common::Filepath& filepath )
         : AssetBase( priority, filepath, AssetTypeID::Texture2D )
    {
    }

    Common::BoolResultStr TextureAsset::LoadFromFile()
    {
        const auto raw = Common::Utils::FileSystem::ReadFileContent( m_Metadata.Filepath );
        if ( !raw )
            return Common::MakeError( raw.GetError() );

        const auto dataReflected = rfl::json::read<Serialization::TextureAssetData>( raw.GetValue() );
        if ( !dataReflected.has_value() )
        {
            return Common::MakeError( dataReflected.error().what() );
        }

        // The file stores the source's PLACE in the project (`assets:Textures/T.png`), not a path — that is
        // what lets a committed .tex load pixels on a machine whose checkout sits anywhere. Expanded here,
        // once, through the same root table that wrote it, so every consumer of GetSourcePath() (the GPU
        // upload in TextureFactory, the GIF decode, the editor's filename labels) keeps seeing a path this
        // machine can open. A stored value with no known tag passes through unchanged; that covers .tex
        // files from before this form existed and sources genuinely outside the project.
        m_SourcePath = Common::AssetHandle::PathForStableKey( dataReflected->SourcePath ).string();

        // The cooked `.tex` carries an id of its own, which additionally survives a rename of the file, so
        // it wins over the path-derived handle AssetBase installed. GetHandle() reads this same field, so
        // TextureService and the editor cannot disagree about which id a texture has.
        m_Metadata.Handle = dataReflected->Handle;

        // THE RELATION NOBODY OWNED. The field above is not a free identity: TextureImporter mints it as
        // `AssetHandle::FromCookedPath(<the source image>)`, so the number in the file and the number the
        // next cook will derive are two statements of one quantity — and until now nothing compared them.
        //
        // What that cost. Four `.tex` files in this repository were written before the derivation became
        // project-relative and still carried the old absolute-path hash. They loaded fine here, because
        // nothing re-cooks a `.tex` whose mesh is already cooked; on a machine that cooks from scratch the
        // importer mints the derived id instead and every `.demat` naming the old number resolves to
        // nothing. Silently — a handle miss has no filename in it, which is the whole reason §1.4 asks for
        // this message. (The two materials that carried such numbers were migrated to the derived ones by
        // the change that added this check; AssetReferenceCensus pins them.)
        //
        // The stored value is still what wins. Substituting the derived id here would be a migration at
        // load that never writes itself back (DC §4.3) — and it would repair the symptom on the one machine
        // that does not need repairing, leaving the file wrong. The message names the file, both numbers
        // and the command that fixes it.
        //
        // Only when the source is project-relative. A `.tex` whose SourcePath is an absolute path outside
        // every content root has no derivable identity to compare against — TextureImporter has already
        // warned at cook time that such a file resolves on one machine only, and repeating it per load
        // would be noise rather than news.
        const std::string sourceKey = Common::AssetHandle::StableKeyForPath( m_SourcePath );
        if ( Common::AssetHandle::IsProjectRelativeKey( sourceKey ) )
        {
            const auto derived = Common::AssetHandle::FromKey( sourceKey );
            if ( static_cast<uint64_t>( derived ) != static_cast<uint64_t>( dataReflected->Handle ) )
            {
                LOG_ERROR( "[Textures] '{0}' stores Handle={1}, but its own source '{2}' derives {3}. The "
                           "stored number is what every reference resolves against, so it is kept — but the "
                           "next cook of this texture will mint {3} and every `.demat` naming {1} will then "
                           "resolve to nothing, with no filename anywhere in the log. Re-cook it (Assets > "
                           "Rebuild Cooked Assets) and re-point the materials that name {1} at {3}.",
                           m_Metadata.Filepath.string(), static_cast<uint64_t>( dataReflected->Handle ), sourceKey,
                           static_cast<uint64_t>( derived ) );
            }
        }

        m_IsReadyForUse = true;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr TextureAsset::Unload()
    {
        // WAS `return BOOLSUCCESS;` — a body that released nothing AND left IsReadyForUse() true, which is
        // the worse half: EnsureLoaded short-circuits on that flag, so an "unloaded" texture would never
        // have been re-read. The eviction that has now been written would have marked it gone and then
        // handed out a shell that still claimed to be loaded, for ever.
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