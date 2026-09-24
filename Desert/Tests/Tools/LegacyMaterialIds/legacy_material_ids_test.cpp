// THE REGISTER OF OLD MATERIAL NUMBERS (AF7c/AF7d).
//
// MATL 2 removed the u64 MaterialId from every `.demat`; scenes (until SCNE 27) and meshes still name
// materials by it. Tools/SceneMigrator keeps the one translation, Editor/Resources/LegacyMaterialIds.json,
// and a v2 file can no longer re-derive it. So the register has to be COMPLETE (every shipped material has
// its row), UNAMBIGUOUS (one number, one GUID; one GUID, one number) and the MATL 1 -> 2 step that reads it
// has to produce exactly the form the engine's parser accepts.

#include <gtest/gtest.h>

#include <LegacyMaterialIds.hpp>

#include <Engine/Assets/MaterialFormat.hpp>

#include <Common/Content/TextAssetHeader.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

using namespace Desert::Migration;
namespace Content = Common::Content;

namespace
{
    std::filesystem::path RepoRoot()
    {
        std::filesystem::path p = std::filesystem::current_path();
        for ( int up = 0; up < 8; ++up, p = p.parent_path() )
            if ( std::filesystem::exists( p / "Editor/Resources/LegacyMaterialIds.json" ) )
                return p;
        return {};
    }

    std::string ReadAll( const std::filesystem::path& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    Content::AssetGuid G( const char* text )
    {
        const auto parsed = Content::AssetGuidFromText( text );
        return parsed.GetValue();
    }

    // M_CheckerFloor_Inst.demat as it was before MATL 2 (git 81dd892f), and its parent's number.
    const std::string     kInstanceV1 = R"({
    "Header": {
        "Kind": "Material",
        "Guid": "3cac456286293463b516718906b23e28",
        "Versions": {
            "MATL": 1
        },
        "Dependencies": []
    },
    "Params": [],
    "Textures": [],
    "MaterialId": 16812725336093661457,
    "ParentMaterialId": 6418972230554417713
})";
    constexpr uint64_t    kParentId   = 6418972230554417713ull;
    constexpr const char* kParent     = "45d579b03cc0d0a8df2e4cb025d6bea5";
} // namespace

TEST( LegacyMaterialIds, TheShippedRegisterIsCompleteAndUnambiguous )
{
    const auto root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "repository root not found";
    const auto assets = root / "Editor/Resources/Assets";
    EXPECT_EQ( LegacyMaterialIdRegisterPath( assets ), root / "Editor/Resources/LegacyMaterialIds.json" );

    const auto text = ReadAll( LegacyMaterialIdRegisterPath( assets ) );
    const auto map  = ParseLegacyMaterialIdRegister( "LegacyMaterialIds.json", text );
    ASSERT_TRUE( map ) << map.GetError();

    // One GUID, one number: two numbers for one file would mean two v1 files stated one GUID.
    std::map<std::string, uint64_t> numberOf;
    for ( const auto& [id, guid] : map.GetValue() )
    {
        EXPECT_NE( id, 0u );
        EXPECT_FALSE( guid.IsNull() ) << "number " << id << " maps to the null GUID";
        const auto [it, fresh] = numberOf.emplace( Content::AssetGuidToText( guid ), id );
        EXPECT_TRUE( fresh ) << "GUID " << it->first << " is named by both " << it->second << " and " << id;
    }

    // Complete: every shipped material has its row, and no row names a GUID no file carries.
    std::set<std::string> shipped;
    for ( const auto& entry : std::filesystem::recursive_directory_iterator( assets / "Materials" ) )
    {
        if ( !entry.is_regular_file() || entry.path().extension() != ".demat" )
            continue;
        const auto parsed = Desert::Assets::ParseMaterialJson( entry.path().string(), ReadAll( entry.path() ) );
        ASSERT_TRUE( parsed ) << parsed.GetError();
        const auto guid = Content::AssetGuidToText( parsed.GetValue().Guid() );
        shipped.insert( guid );
        EXPECT_TRUE( numberOf.count( guid ) ) << entry.path().filename().string() << " (" << guid
                                              << ") has no row: its old number can never be translated again";
    }
    for ( const auto& [guid, id] : numberOf )
        EXPECT_TRUE( shipped.count( guid ) ) << "row " << id << " names " << guid << ", which no .demat carries";
    EXPECT_EQ( shipped.size(), numberOf.size() );
    EXPECT_GE( shipped.size(), 100u ) << "the sweep found too few materials to mean anything";

    // The register is its own canonical text: re-writing it changes no byte.
    const auto rewritten = WriteLegacyMaterialIdRegister( map.GetValue() );
    ASSERT_TRUE( rewritten ) << rewritten.GetError();
    EXPECT_EQ( rewritten.GetValue(), text );

    // With every material already v2, loading the corpus adds nothing to the register.
    const auto loaded = LoadLegacyMaterialIds( assets );
    ASSERT_TRUE( loaded ) << loaded.GetError();
    EXPECT_EQ( loaded.GetValue(), map.GetValue() );
}

TEST( LegacyMaterialIds, OneNumberNamingTwoGuidsIsRefused )
{
    LegacyMaterialIdMap map;
    ASSERT_TRUE( AddLegacyMaterialId( map, 7, G( kParent ), "a.demat" ) );
    EXPECT_TRUE( AddLegacyMaterialId( map, 7, G( kParent ), "a.demat again" ) )
         << "the same row twice is not a collision";
    const auto clash = AddLegacyMaterialId( map, 7, G( "3cac456286293463b516718906b23e28" ), "b.demat" );
    ASSERT_FALSE( clash );
    EXPECT_NE( clash.GetError().find( "b.demat" ), std::string::npos ) << clash.GetError();
    EXPECT_EQ( map.size(), 1u );
    EXPECT_EQ( map.at( 7 ), G( kParent ) ) << "a refused row must not replace the first";
}

TEST( LegacyMaterialIds, RaisingAnInstanceToMatl2GivesTheFormTheEngineReads )
{
    LegacyMaterialIdMap map{ { kParentId, G( kParent ) } };
    MaterialV2Report    report;
    const auto          raised = RaiseMaterialTextToV2( "inst.demat", kInstanceV1, map, report );
    ASSERT_TRUE( raised ) << raised.GetError();
    EXPECT_TRUE( report.DroppedId );
    EXPECT_TRUE( report.Parented );

    const std::string& v2 = raised.GetValue();
    EXPECT_EQ( v2.find( "MaterialId" ), std::string::npos ) << v2;
    const auto parsed = Desert::Assets::ParseMaterialJson( "inst.demat", v2 );
    ASSERT_TRUE( parsed ) << parsed.GetError() << "\n" << v2;
    const auto& m = parsed.GetValue();
    EXPECT_EQ( Content::AssetGuidToText( m.Guid() ), "3cac456286293463b516718906b23e28" );
    ASSERT_TRUE( m.IsInstance() );
    EXPECT_EQ( *m.Parent, kParent );
    ASSERT_EQ( m.Header->Dependencies.size(), 1u );
    EXPECT_EQ( m.Header->Dependencies.front(), kParent );
    EXPECT_EQ( Content::TextHeaderVersion( *m.Header, Desert::Assets::kMaterialSchemaTag ), 2u );
    EXPECT_EQ( static_cast<uint64_t>( *m.InstanceParentId() ),
               static_cast<uint64_t>( Content::HandleForGuid( G( kParent ) ) ) );
}

TEST( LegacyMaterialIds, RaisingRefusesAnUnknownParentAndAFileAlreadyRaised )
{
    MaterialV2Report report;
    EXPECT_FALSE( RaiseMaterialTextToV2( "inst.demat", kInstanceV1, {}, report ) )
         << "a parent number the register does not know must be refused, not dropped";

    LegacyMaterialIdMap map{ { kParentId, G( kParent ) } };
    const auto          raised = RaiseMaterialTextToV2( "inst.demat", kInstanceV1, map, report );
    ASSERT_TRUE( raised );
    EXPECT_FALSE( RaiseMaterialTextToV2( "inst.demat", raised.GetValue(), map, report ) )
         << "a MATL 2 text is not a MATL 1 text";
}

TEST( LegacyMaterialIds, ReadingTheStatedNumbersOfAV1Text )
{
    const auto stated = ReadStatedMaterialIds( "inst.demat", kInstanceV1 );
    ASSERT_TRUE( stated ) << stated.GetError();
    EXPECT_EQ( stated.GetValue().Version, 1u );
    EXPECT_EQ( stated.GetValue().Guid, G( "3cac456286293463b516718906b23e28" ) );
    ASSERT_TRUE( stated.GetValue().MaterialId.has_value() );
    EXPECT_EQ( *stated.GetValue().MaterialId, 16812725336093661457ull );
    ASSERT_TRUE( stated.GetValue().ParentMaterialId.has_value() );
    EXPECT_EQ( *stated.GetValue().ParentMaterialId, kParentId );
}
