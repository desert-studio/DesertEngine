#include "IesProfileImporter.hpp"

#include <Engine/Assets/Serialization/IesProfile.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>
#include <Engine/Graphic/IesProfile.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <system_error>

namespace Desert::Editor
{
    std::filesystem::path IesProfileAssetPathFor( const std::filesystem::path& ies )
    {
        std::filesystem::path dies = ies;
        dies.replace_extension( Assets::Serialization::kIesProfileExtension );
        return dies;
    }

    CookVerdict ImportIesProfile( const std::filesystem::path& ies, const ImportPass pass, const bool force )
    {
        const std::filesystem::path dies = IesProfileAssetPathFor( ies );

        std::error_code ec;
        const bool      exists = std::filesystem::exists( dies, ec );
        if ( exists && !force )
        {
            const auto assetTime  = std::filesystem::last_write_time( dies, ec );
            const bool assetKnown = !ec;
            const auto sourceTime = std::filesystem::last_write_time( ies, ec );
            if ( assetKnown && !ec && assetTime >= sourceTime )
                return CookVerdict::UpToDate;
        }
        if ( pass == ImportPass::Cook )
        {
            LOG_ERROR( "[Import] IES profile '{}' has no current '{}'; re-import the .ies (drop it on the Content "
                       "Browser or Rebuild Cooked Assets).",
                       ies.string(), dies.filename().string() );
            return CookVerdict::Failed;
        }

        auto text = Common::Utils::FileSystem::ReadFileContent( ies );
        if ( !text )
        {
            LOG_ERROR( "[Import] IES profile '{}' could not be read: {}", ies.string(), text.GetError() );
            return CookVerdict::Failed;
        }
        const auto photometry = Graphic::ParseIesProfile( text.GetValue() );
        if ( !photometry )
        {
            LOG_ERROR( "[Import] IES profile '{}' was not imported: {}", ies.string(), photometry.GetError() );
            return CookVerdict::Failed;
        }

        Assets::Serialization::IesProfileData data =
             Assets::Serialization::IesProfileFromPhotometry( photometry.GetValue() );
        // The re-import's identity: the GUID of the `.dies` already there (StampTextHeader mints one only when
        // the header states none, i.e. on the first import).
        if ( exists )
            if ( const Assets::TextAssetIdentity identity = Assets::ReadTextAssetIdentity( dies );
                 !identity.Guid.IsNull() )
                data.Header =
                     Common::Content::MakeTextHeader( Common::Content::ContentKind::IesProfile, identity.Guid,
                                                      Assets::Serialization::IesProfileTextSubsystems() );

        if ( const auto saved = Assets::Serialization::SaveIesProfileFile( dies, data ); !saved )
        {
            LOG_ERROR( "[Import] IES profile '{}' was not written: {}", dies.string(), saved.GetError() );
            return CookVerdict::Failed;
        }
        LOG_INFO( "[Import] IES profile '{}' -> '{}' ({} vertical x {} horizontal angles).", ies.string(),
                  dies.filename().string(), data.VerticalAngles.size(), data.HorizontalAngles.size() );
        return CookVerdict::Cooked;
    }
} // namespace Desert::Editor
