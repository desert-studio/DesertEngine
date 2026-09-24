#pragma once

#include <Common/Content/TextAssetHeader.hpp>

#include <cstdint>
#include <optional>
#include <span>

// The engine's side of the text asset header (Common/Content/TextAssetHeader.hpp): the subsystem tags a
// scene, a prefab and a material state their schema generations under, and the one stamping rule every
// text writer follows.
namespace Desert::Assets
{
    // A .desce / .deprefab: the scene schema generation and the world-unit generation (Core::kSceneVersion,
    // Core::kUnitVersion). The same two numbers for both, because a prefab's payload IS the scene's records.
    inline constexpr uint32_t kSceneSchemaTag = Common::Content::FourCC( "SCNE" );
    inline constexpr uint32_t kUnitSchemaTag  = Common::Content::FourCC( "UNIT" );
    // A .demat: the material schema, numbered for the first time by AF6f (v1). v2 (AF7c): the payload's
    // MaterialId is gone - the header GUID is the one identity - and an instance names its parent by that
    // GUID (`Parent`), stated again as the header's Dependency.
    inline constexpr uint32_t kMaterialSchemaTag     = Common::Content::FourCC( "MATL" );
    inline constexpr uint32_t kMaterialSchemaVersion = 2;
    // A .decloudtype: the cloud type file layout, stated in the header since v4 (AF7v; v1-v3 had a
    // top-level FormatVersion and no header). Here rather than beside CloudTypeData so the migrator can
    // state it without the cloud maths.
    inline constexpr uint32_t kCloudTypeSchemaTag     = Common::Content::FourCC( "CLTY" );
    inline constexpr uint32_t kCloudTypeSchemaVersion = 4;

    // The version the header states for `tag`; 0 when there is no header or it does not state that tag -
    // the same "absent is version 0, never current" the gates have always applied.
    [[nodiscard]] inline int
    StatedVersion( const std::optional<Common::Content::TextAssetHeaderSerialized>& header, uint32_t tag )
    {
        if ( !header )
            return 0;
        return static_cast<int>( Common::Content::TextHeaderVersion( *header, tag ).value_or( 0 ) );
    }

    // THE STAMPING RULE. The GUID is the file's identity: kept from the header the asset was loaded with, and
    // minted (AssetGuid::Generate) only when there is none - a new asset. It is never derived from anything,
    // so a rename or a move keeps it. Kind and versions are this build's; Dependencies are left empty for the
    // format's own writer to fill (a material instance names its parent: StampMaterialHeader).
    [[nodiscard]] inline Common::Content::TextAssetHeaderSerialized
    StampTextHeader( const std::optional<Common::Content::TextAssetHeaderSerialized>& loaded,
                     Common::Content::ContentKind                                     kind,
                     std::span<const Common::Content::SubsystemVersion>               versions )
    {
        Common::Content::AssetGuid guid;
        if ( loaded )
            if ( const auto parsed = Common::Content::AssetGuidFromText( loaded->Guid ); parsed )
                guid = parsed.GetValue();
        if ( guid.IsNull() )
            guid = Common::Content::AssetGuid::Generate();
        return Common::Content::MakeTextHeader( kind, guid, versions );
    }
} // namespace Desert::Assets
