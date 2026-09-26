#include "AssetRefSerialization.hpp"

#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/AssetHandle.hpp>

#include <format>

namespace Desert::Assets
{
    Common::ResultStr<AssetGuidRef> WriteAssetGuidRef( const Common::Content::AssetGuid& guid,
                                                       const std::filesystem::path&      runtimePath,
                                                       const AssetRefSite&               site )
    {
        if ( guid.IsNull() )
            return Common::MakeError<AssetGuidRef>(
                 std::format( "{} on {}: {} '{}' states no header GUID to name it by (missing, or no header)",
                              site.Field, site.Context, site.Kind, runtimePath.generic_string() ) );
        return Common::MakeSuccess( AssetGuidRef{ Common::Content::AssetGuidToText( guid ),
                                                  Common::AssetHandle::StableKeyForPath( runtimePath ) } );
    }

    Common::ResultStr<std::string> ResolveAssetGuidRef( const AssetGuidRef& ref, const AssetGuidResolver& resolver,
                                                        const AssetRefSite& site )
    {
        const auto guid = Common::Content::AssetGuidFromText( ref.Guid );
        if ( !guid || guid.GetValue().IsNull() )
            return Common::MakeError<std::string>( std::format( "{} on {}: {} reference '{}' ('{}') states no GUID",
                                                                site.Field, site.Context, site.Kind, ref.Guid,
                                                                ref.Path ) );
        auto resolved = resolver( guid.GetValue() );
        if ( !resolved.has_value() )
            return Common::MakeError<std::string>(
                 std::format( "{} on {}: {} {} ('{}') is not a {} this project knows", site.Field, site.Context,
                              site.Kind, ref.Guid, ref.Path, site.Kind ) );
        return Common::MakeSuccess( std::move( *resolved ) );
    }
} // namespace Desert::Assets
