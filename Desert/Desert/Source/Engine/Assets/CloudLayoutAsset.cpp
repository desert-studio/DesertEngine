#include <Engine/Assets/CloudLayoutAsset.hpp>

#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

#include <fstream>
#include <optional>
#include <sstream>
#include <span>

namespace Desert::Assets
{
    CloudLayoutAsset::CloudLayoutAsset( const Common::Filepath& filepath )
         : AssetBase( filepath, AssetTypeID::CloudLayout )
    {
        // THE LAYOUT'S IDENTITY IS ITS ENVELOPE GUID (container 2), adopted HERE rather than in the load,
        // for the mesh's reason: the asset manager keys its handle lookup at creation. Only the envelope
        // header is read, through the VFS first like the load. A file with no readable header (absent: Save
        // is about to create it; or a bare version-1 file) keeps the path-derived handle - the load refuses
        // the latter by name, so no layout is ever READY under that handle.
        namespace CC                              = Common::Content;
        const CC::SubsystemVersion       kKnown[] = { { kCloudLayoutSubsystemTag, kCloudLayoutContainerVersion } };
        const CC::AssetHeaderReadContext context{ kKnown };

        std::optional<Common::ResultStr<CC::EnvelopeHeader>> header;
        if ( const auto packed = Common::Utils::VFS::Exists( m_Metadata.Filepath )
                                      ? Common::Utils::VFS::ReadFile( m_Metadata.Filepath )
                                      : std::nullopt;
             packed.has_value() )
        {
            std::istringstream in( *packed );
            header.emplace( CC::ReadEnvelopeHeader( in, context ) );
        }
        else if ( std::ifstream in( m_Metadata.Filepath, std::ios::binary ); in )
            header.emplace( CC::ReadEnvelopeHeader( in, context ) );

        if ( !header || !*header || header->GetValue().Asset.Kind != CC::ContentKind::CloudLayout ||
             header->GetValue().Asset.Guid.IsNull() )
            return;
        m_Guid = header->GetValue().Asset.Guid;
        AdoptHandleFromFile( Common::UUID( static_cast<uint64_t>( CC::HandleForGuid( m_Guid ) ) ),
                             Common::AssetHandle::StableKeyForPath( m_Metadata.Filepath ) );
    }

    Common::BoolResultStr CloudLayoutAsset::LoadFromFile()
    {
        const std::string path = m_Metadata.Filepath.string();

        // Through the VFS rather than straight off the disk, so a packaged build reads the painting out of
        // its .dpak exactly like every other asset.
        const auto contents = Common::Utils::VFS::Exists( m_Metadata.Filepath )
                                   ? Common::Utils::VFS::ReadFile( m_Metadata.Filepath )
                                   : std::nullopt;

        std::vector<unsigned char> bytes;
        if ( contents.has_value() )
        {
            bytes.assign( contents->begin(), contents->end() );
        }
        else
        {
            std::ifstream file( m_Metadata.Filepath, std::ios::binary );
            if ( !file )
                return Common::MakeFormattedError<bool>( "Cloud layout '{}' could not be opened", path );

            bytes.assign( std::istreambuf_iterator<char>( file ), std::istreambuf_iterator<char>() );
        }

        auto decoded = DecodeCloudLayout( bytes );
        if ( !decoded )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "Cloud layout '{}' is not usable: {}", path,
                                                     decoded.GetError() );
        }

        m_Layout = std::make_shared<const CloudLayoutData>( decoded.ExtractValue() );
        m_Ready  = true;

        const CloudLayoutData& layout = *m_Layout;
        LOG_INFO( "[Clouds] Layout '{}' loaded: {}x{}, pattern {}, mask {}, channel means "
                  "{:.3f}/{:.3f}/{:.3f}/{:.3f}, content {:08x}.",
                  path, layout.Resolution, layout.Resolution, layout.HasPattern() ? "yes" : "no",
                  layout.HasMask() ? "yes" : "no", layout.PatternMean[0], layout.PatternMean[1],
                  layout.PatternMean[2], layout.PatternMean[3], layout.ContentHash );

        return BOOLSUCCESS;
    }

    Common::BoolResultStr CloudLayoutAsset::Unload()
    {
        // A FRESH EMPTY REVISION rather than the old one cleared in place: a bake still holding the old
        // revision keeps reading valid pixels, and this asset no longer describes a buffer it freed (a zero
        // ContentHash, zero means, no tables).
        m_Layout = std::make_shared<const CloudLayoutData>();
        m_Ready  = false;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr CloudLayoutAsset::ReplaceLayout( CloudLayoutData layout )
    {
        if ( auto valid = ValidateCloudLayoutData( layout ); !valid )
            return Common::MakeFormattedError<bool>( "cloud layout '{}' was not replaced: {}",
                                                     m_Metadata.Filepath.string(), valid.GetError() );

        m_Layout = std::make_shared<const CloudLayoutData>( std::move( layout ) );
        m_Ready  = true;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr CloudLayoutAsset::Save( const Common::Filepath& filepath, const CloudLayoutData& layout )
    {
        auto encoded = EncodeCloudLayout( layout );
        if ( !encoded )
            return Common::MakeFormattedError<bool>( "refusing to write '{}': {}", filepath.string(),
                                                     encoded.GetError() );

        std::error_code ec;
        if ( filepath.has_parent_path() )
            std::filesystem::create_directories( filepath.parent_path(), ec );

        // Through the write primitive, not a local std::ofstream (Д35): the local stream's flush is its
        // destructor, which runs after this function has already returned BOOLSUCCESS, so a full disk
        // produced a green save and a truncated `.dclayout`.
        const std::vector<unsigned char>& bytes = encoded.GetValue();
        if ( const auto written = Common::Utils::FileSystem::WriteBytesToFileAtomic(
                  filepath, std::as_bytes( std::span( bytes ) ) );
             !written )
            return Common::MakeFormattedError<bool>( "'{}' ({} bytes) could not be written: {}", filepath.string(),
                                                     bytes.size(), written.GetError() );

        LOG_INFO( "[Clouds] Layout written: '{}', {}x{}, {} bytes.", filepath.string(), layout.Resolution,
                  layout.Resolution, bytes.size() );
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
