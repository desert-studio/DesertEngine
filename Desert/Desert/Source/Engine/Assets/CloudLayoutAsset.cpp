#include <Engine/Assets/CloudLayoutAsset.hpp>

#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

#include <fstream>
#include <span>

namespace Desert::Assets
{
    // NOTHING IN THE BODY, AND THE EMPTINESS IS THE POINT. An earlier draft of this file derived the
    // handle here — `m_Metadata.Handle = AssetHandle::FromCookedPath(...)` — copying what the three cloud
    // assets beside it did at the time. That is no longer where identity comes from: `AssetBase`'s own
    // constructor derives it from the path for every type at once, so a fourth statement of the same rule
    // would be a fourth place it can drift from. A layout gets its identity BY CONSTRUCTION, and the two
    // assignments left in the engine are the two that genuinely carry an identity of their own from a file.
    CloudLayoutAsset::CloudLayoutAsset( AssetPriority priority, const Common::Filepath& filepath )
         : AssetBase( priority, filepath, AssetTypeID::CloudLayout )
    {
    }

    Common::BoolResultStr CloudLayoutAsset::Load()
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

        m_Layout = decoded.ExtractValue();
        m_Ready  = true;

        LOG_INFO( "[Clouds] Layout '{}' loaded: {}x{}, pattern {}, mask {}, channel means "
                  "{:.3f}/{:.3f}/{:.3f}/{:.3f}, content {:08x}.",
                  path, m_Layout.Resolution, m_Layout.Resolution, m_Layout.HasPattern() ? "yes" : "no",
                  m_Layout.HasMask() ? "yes" : "no", m_Layout.PatternMean[0], m_Layout.PatternMean[1],
                  m_Layout.PatternMean[2], m_Layout.PatternMean[3], m_Layout.ContentHash );

        return BOOLSUCCESS;
    }

    Common::BoolResultStr CloudLayoutAsset::Unload()
    {
        m_Layout.Pattern.clear();
        m_Layout.Pattern.shrink_to_fit();
        m_Layout.Mask.clear();
        m_Layout.Mask.shrink_to_fit();
        m_Layout.Resolution  = 0u;
        m_Layout.ContentHash = 0u;
        // The one thing this body — the most thorough of the thirteen and the model for the rest — still
        // left describing a buffer it had freed. A zero ContentHash beside a non-zero mean is an
        // inconsistent state for exactly the reason the two lines above exist.
        m_Layout.PatternMean[0] = 0.0F;
        m_Layout.PatternMean[1] = 0.0F;
        m_Layout.PatternMean[2] = 0.0F;
        m_Layout.PatternMean[3] = 0.0F;
        m_Ready                 = false;
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
