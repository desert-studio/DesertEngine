#include <Engine/Assets/SparseVolumeAsset.hpp>
#include <Engine/Assets/ContentRegistry.hpp>

#include <Common/Utilities/VFS.hpp>

#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>

namespace Desert::Assets
{
    SparseVolumeAsset::SparseVolumeAsset( const Common::Filepath& filepath )
         : AssetBase( filepath, AssetTypeID::SparseVolume )
    {
        // The identity is the envelope GUID, adopted at creation (the asset manager keys its lookup here). A file
        // with no readable header keeps the path-derived handle and the load refuses it by name.
        const Common::Content::AssetGuid guid = ReadGuid( m_Metadata.Filepath );
        if ( guid.IsNull() )
            return;
        AdoptHandleFromFile( Common::UUID( static_cast<uint64_t>( Common::Content::HandleForGuid( guid ) ) ),
                             Common::AssetHandle::StableKeyForPath( m_Metadata.Filepath ) );
    }

    Common::Content::AssetGuid SparseVolumeAsset::ReadGuid( const std::filesystem::path& dsvol )
    {
        namespace CC                        = Common::Content;
        const CC::SubsystemVersion kKnown[] = { { kSparseVolumeSubsystemTag, kSparseVolumeContainerVersion } };
        const CC::AssetHeaderReadContext context{ kKnown };
        const std::filesystem::path      file = ContentRegistry::FileToOpen( dsvol );

        std::optional<Common::ResultStr<CC::EnvelopeHeader>> header;
        if ( const auto packed =
                  Common::Utils::VFS::Exists( file ) ? Common::Utils::VFS::ReadFile( file ) : std::nullopt;
             packed.has_value() )
        {
            std::istringstream in( *packed );
            header.emplace( CC::ReadEnvelopeHeader( in, context ) );
        }
        else if ( std::ifstream in( file, std::ios::binary ); in )
            header.emplace( CC::ReadEnvelopeHeader( in, context ) );

        if ( !header || !*header || header->GetValue().Asset.Kind != CC::ContentKind::SparseVolume )
            return {};
        return header->GetValue().Asset.Guid;
    }

    Common::ResultStr<SparseVolumeData> SparseVolumeAsset::ReadFile( const std::filesystem::path& dsvol )
    {
        const std::filesystem::path file = ContentRegistry::FileToOpen( dsvol );
        std::vector<unsigned char>  bytes;
        // Through the VFS first, so a packaged build reads the volume out of its .dpak like every other asset.
        if ( const auto packed =
                  Common::Utils::VFS::Exists( file ) ? Common::Utils::VFS::ReadFile( file ) : std::nullopt;
             packed.has_value() )
        {
            bytes.assign( packed->begin(), packed->end() );
        }
        else
        {
            std::ifstream in( file, std::ios::binary );
            if ( !in )
                return Common::MakeFormattedError<SparseVolumeData>( "sparse volume '{}' could not be opened",
                                                                     file.string() );
            bytes.assign( std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() );
        }
        auto decoded = DecodeSparseVolume( bytes );
        if ( !decoded )
            return Common::MakeFormattedError<SparseVolumeData>( "sparse volume '{}' is not usable: {}",
                                                                 file.string(), decoded.GetError() );
        return decoded;
    }

    Common::BoolResultStr SparseVolumeAsset::LoadFromFile()
    {
        auto read = ReadFile( m_Metadata.Filepath );
        if ( !read )
        {
            m_Ready = false;
            return Common::MakeError<bool>( read.GetError() );
        }
        m_Data  = read.ExtractValue();
        m_Ready = true;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr SparseVolumeAsset::Unload()
    {
        m_Data  = SparseVolumeData{};
        m_Ready = false;
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
