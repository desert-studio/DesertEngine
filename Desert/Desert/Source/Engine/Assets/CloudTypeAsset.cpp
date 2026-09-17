#include <Engine/Assets/CloudTypeAsset.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/CloudNoiseVolumeAsset.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

#include <algorithm>
#include <iterator>

namespace Desert::Assets
{
    CloudTypeAsset::CloudTypeAsset( AssetPriority priority, const Common::Filepath& filepath )
         : AssetBase( priority, filepath, AssetTypeID::CloudType )
    {
        // The path-derived handle this type used to compute for itself now comes from AssetBase, which
        // derives it the same way for every asset type. See the comment on that constructor.
        m_DisplayName = m_Metadata.Filepath.stem().string();
    }

    Common::BoolResultStr CloudTypeAsset::LoadFromFile()
    {
        const std::string path = m_Metadata.Filepath.string();

        // Through the VFS first, so a packaged build reads the type out of its .dpak exactly like every
        // other asset, then off the disk for a loose file the pak does not carry.
        std::string text;
        if ( const auto packed = Common::Utils::VFS::Exists( m_Metadata.Filepath )
                                      ? Common::Utils::VFS::ReadFile( m_Metadata.Filepath )
                                      : std::nullopt;
             packed.has_value() )
        {
            text = packed.value();
        }
        else
        {
            if ( auto read = Common::Utils::FileSystem::ReadFileContent( m_Metadata.Filepath ); read )
                text = read.ExtractValue();
            // A failed read leaves `text` empty on purpose: the branch below is the one refusal that
            // names both shapes ("empty or could not be opened").
        }

        if ( text.empty() )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "Cloud type '{}' is empty or could not be opened", path );
        }

        auto parsed = ParseCloudType( text );
        if ( !parsed )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "Cloud type '{}' is not usable: {}", path,
                                                     parsed.GetError() );
        }

        m_Data        = parsed.ExtractValue();
        m_DisplayName = m_Data.DisplayName.value_or( m_Metadata.Filepath.stem().string() );

        ++m_Revision;
        m_Ready = true;

        // THE PROFILE IS SUMMARISED RATHER THAN PRINTED IN FULL: sixteen floats per type times nine types
        // is a screenful of log nobody reads, and what a reader needs from this line is whether the curve
        // that loaded is the curve they authored. Base, widest and top say that — they separate a deck
        // (equal ends), a taper (falling), and a tower (widest in the middle) from one another at a
        // glance, which is the whole set of shapes the format can now carry.
        const auto& profile = m_Data.Shape.Profile;
        const auto  widest  = std::max_element( profile.HalfWidth.begin(), profile.HalfWidth.end() );
        const float peakAt  = static_cast<float>( std::distance( profile.HalfWidth.begin(), widest ) ) /
                             static_cast<float>( Graphic::kCloudProfileSamples - 1 );

        LOG_INFO( "[Clouds] Cloud type '{}' loaded: {}, base {:.2f} km, top {:.2f} km, edge {:.2f}, "
                  "anvil {:.2f} at {:.2f} km, detail {:.2f} ({}), density x{:.2f}, extinction x{:.2f}, "
                  "profile {:.3f} at the base to {:.3f} at the top, widest {:.3f} at {:.0f}% up the band.",
                  path, m_DisplayName, m_Data.Shape.BaseAltitudeKm, m_Data.Shape.TopAltitudeKm,
                  m_Data.Shape.EdgeTopFraction, m_Data.Shape.AnvilStrength, m_Data.Shape.AnvilAltitudeKm,
                  m_Data.Shape.DetailFactor, m_Data.Shape.DetailCharacter < 0.5f ? "wispy" : "billowy",
                  m_Data.Shape.DensityFactor, m_Data.Shape.ExtinctionFactor, profile.HalfWidth.front(),
                  profile.HalfWidth.back(), *widest, peakAt * 100.0f );

        return BOOLSUCCESS;
    }

    void CloudTypeAsset::ResolveDependencies( AssetManager& manager )
    {
        m_NoiseVolume = AssetHandle::Null();

        const std::string relative = m_Data.NoiseVolume.value_or( std::string{} );
        if ( relative.empty() )
            return; // the documented "use the built-in default volume"

        // RELATIVE TO THE ASSETS ROOT, joined here and nowhere else. The file stores
        // "Clouds/CloudNoise_FineWisp.dcnv" so that the library is the same library on another machine;
        // the AssetManager indexes volumes under their full project-rooted path, so exactly one join has
        // to happen and this is it.
        const Common::Filepath full = ( Common::Constants::Path::ASSETS_PATH / relative ).lexically_normal();

        if ( const auto volume = manager.FindByPath<CloudNoiseVolumeAsset>( full ) )
        {
            m_NoiseVolume = volume->GetMetadata().Handle;
            return;
        }

        // NOT a silent fall-through to the default: the type names a volume, the volume is not there, and
        // the sky that comes out will be the default one wearing this type's name. §1.4.
        LOG_ERROR( "[Clouds] Cloud type '{}' names noise volume '{}' ({}), which is not loaded. The layer "
                   "will use the built-in default volume and its edge will not be the authored one.",
                   m_Metadata.Filepath.string(), relative, full.string() );
    }

    Common::BoolResultStr CloudTypeAsset::Unload()
    {
        // WAS THE FLAG ALONE, so `GetData()`, `GetShape()`, `GetDisplayName()` and `GetNoiseVolume()` all
        // went on answering with post-load values on an asset that reported itself not ready. A cloud type
        // is a dozen numbers and three optional strings, so the memory at stake is small — but a getter
        // that contradicts the readiness flag is exactly the state §1.4 is about, and eviction is the
        // caller that makes it reachable.
        //
        // The DISPLAY NAME goes back to the stem, which is what the constructor seeded and what a picker
        // shows for a type nobody has read yet — not to empty, which would make the slot look broken.
        m_Data        = CloudTypeData{};
        m_NoiseVolume = AssetHandle{};
        m_DisplayName = m_Metadata.Filepath.stem().string();
        m_Ready       = false;
        // The revision is monotonic ON PURPOSE and is not reset: it is what CloudTypeService compares to
        // decide whether its cached profile table is stale, and rewinding it would make a reload look like
        // no change at all.
        return BOOLSUCCESS;
    }

    Common::BoolResultStr CloudTypeAsset::Save( const Common::Filepath& filepath, const CloudTypeData& data )
    {
        if ( auto valid = ValidateCloudTypeShape( data.Shape ); !valid )
            return Common::MakeFormattedError<bool>( "refusing to write '{}': {}", filepath.string(),
                                                     valid.GetError() );

        std::error_code ec;
        if ( filepath.has_parent_path() )
            std::filesystem::create_directories( filepath.parent_path(), ec );

        CloudTypeData written = data;
        written.FormatVersion = kCloudTypeFormatVersion;

        const std::string text = WriteCloudType( written );

        // Through the write primitive, not a local std::ofstream (Д35). A `.decloudtype` is a few
        // hundred bytes — smaller than one filebuf — so it is precisely the payload that never reaches
        // the OS until the flush, and the flush of a local stream is its destructor, running after this
        // function has already returned BOOLSUCCESS. The primitive closes before it decides, and writes
        // through a temporary so a failed save cannot cost the artist the type they already had.
        if ( const auto written = Common::Utils::FileSystem::WriteContentToFileAtomic( filepath, text ); !written )
            return Common::MakeFormattedError<bool>( "'{}' ({} bytes) could not be written: {}", filepath.string(),
                                                     text.size(), written.GetError() );

        LOG_INFO( "[Clouds] Cloud type written: '{}', base {:.2f} km, top {:.2f} km, {} bytes.", filepath.string(),
                  written.Shape.BaseAltitudeKm, written.Shape.TopAltitudeKm, text.size() );
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
