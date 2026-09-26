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
#include <Common/Json/Json.hpp>

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

TEST( LegacyMaterialIds, RaisingAnInstanceThroughMatl2To4GivesTheFormTheEngineReads )
{
    LegacyMaterialIdMap map{ { kParentId, G( kParent ) } };
    MaterialV2Report    report;
    const auto          raised = RaiseMaterialTextToV2( "inst.demat", kInstanceV1, map, report );
    ASSERT_TRUE( raised ) << raised.GetError();
    EXPECT_TRUE( report.DroppedId );
    EXPECT_TRUE( report.Parented );

    const std::string& v2 = raised.GetValue();
    EXPECT_EQ( v2.find( "MaterialId" ), std::string::npos ) << v2;
    const auto frozen = Common::Json::Read<MaterialDataV2>( v2 );
    ASSERT_TRUE( frozen ) << v2;
    ASSERT_TRUE( frozen.GetValue().Header.has_value() );
    EXPECT_EQ( Content::TextHeaderVersion( *frozen.GetValue().Header, Desert::Assets::kMaterialSchemaTag ), 2u );
    ASSERT_EQ( frozen.GetValue().Header->Dependencies.size(), 1u );
    EXPECT_EQ( frozen.GetValue().Header->Dependencies.front(), kParent );

    // The rest of the chain: MATL 2 -> 4 (no slots, no shader here) and the engine's own reader.
    const auto v3 = RaiseMaterialV2ToV4( "inst.demat", frozen.GetValue(), LegacyAssetRefMap{} );
    ASSERT_TRUE( v3 ) << v3.GetError();
    const auto text = Desert::Assets::WriteMaterialJson( v3.GetValue() );
    ASSERT_TRUE( text ) << text.GetError();
    const auto parsed = Desert::Assets::ParseMaterialJson( "inst.demat", text.GetValue() );
    ASSERT_TRUE( parsed ) << parsed.GetError() << "\n" << text.GetValue();
    const auto& m = parsed.GetValue();
    EXPECT_EQ( Content::AssetGuidToText( m.Guid() ), "3cac456286293463b516718906b23e28" );
    ASSERT_TRUE( m.IsInstance() );
    EXPECT_EQ( *m.Parent, kParent );
    ASSERT_EQ( m.Header->Dependencies.size(), 1u );
    EXPECT_EQ( m.Header->Dependencies.front(), kParent );
    EXPECT_EQ( Content::TextHeaderVersion( *m.Header, Desert::Assets::kMaterialSchemaTag ),
               Desert::Assets::kMaterialSchemaVersion );
    EXPECT_EQ( static_cast<uint64_t>( *m.InstanceParentId() ),
               static_cast<uint64_t>( Content::HandleForGuid( G( kParent ) ) ) );
}

// MATL 2 -> 4, the pure half: every slot lands in the list its name says, by the GUID the table gives for
// its number, with the locator beside it; the shader name becomes the named file's GUID; the header then states
// every GUID as a Dependency, the shader's first.
TEST( LegacyMaterialIds, RaisingToMatl4RoutesEverySlotByNameAndStatesItsGuid )
{
    constexpr const char*   kShd   = "44444444444444444444444444444444";
    constexpr const char*   kTex   = "11111111111111111111111111111111";
    constexpr const char*   kType  = "22222222222222222222222222222222";
    constexpr const char*   kPaint = "33333333333333333333333333333333";
    const LegacyAssetRefMap refs{ { 101, { LegacyAssetKind::Texture, kTex, "assets:Textures/T.detex" } },
                                  { 202, { LegacyAssetKind::CloudType, kType, "assets:Clouds/C.decloudtype" } },
                                  { 303, { LegacyAssetKind::CloudLayout, kPaint, "assets:Clouds/L.dclayout" } },
                                  { 404, { LegacyAssetKind::Shader, kShd, "engine:Shaders/Programs/M.shader" } } };
    MaterialDataV2          v2;
    v2.ShaderName = "M";
    v2.Header =
         Content::MakeTextHeader( Content::ContentKind::Material, G( kParent ), MaterialTextSubsystemsV2() );
    v2.Textures = { { "u_AlbedoTexture", 101 }, { "u_NormalTexture", 0 }, { "CloudType1", 202 },
                    { "LayoutPattern", 303 },   { "LayoutMask", 303 },    { "Medium", 404 } };

    const auto raised = RaiseMaterialV2ToV4( "m.demat", v2, refs );
    ASSERT_TRUE( raised ) << raised.GetError();
    const auto text = Desert::Assets::WriteMaterialJson( raised.GetValue() );
    ASSERT_TRUE( text ) << text.GetError();
    const auto parsed = Desert::Assets::ParseMaterialJson( "m.demat", text.GetValue() );
    ASSERT_TRUE( parsed ) << parsed.GetError() << "\n" << text.GetValue();
    const auto& m = parsed.GetValue();

    ASSERT_EQ( m.Textures.size(), 2u );
    EXPECT_EQ( m.Textures[0].Guid, kTex );
    EXPECT_EQ( m.Textures[0].Path, "assets:Textures/T.detex" );
    EXPECT_TRUE( m.Textures[1].Guid.empty() ) << "a 0 is an authored empty slot";
    const auto& shader = m.Shader;
    if ( !shader.has_value() )
    {
        ADD_FAILURE() << "the raised material has no shader";
        return;
    }
    EXPECT_EQ( shader->Guid, kShd );
    EXPECT_EQ( shader->Path, "engine:Shaders/Programs/M.shader" );
    ASSERT_EQ( m.CloudAssets.size(), 4u );
    EXPECT_EQ( m.CloudAssets[0].Guid, kType );
    EXPECT_EQ( m.CloudAssets[1].Guid, kPaint );
    EXPECT_EQ( m.CloudAssets[2].Guid, kPaint );
    EXPECT_EQ( m.CloudAssets[3].Name, "Medium" );
    EXPECT_EQ( m.CloudAssets[3].Guid, kShd );
    EXPECT_EQ( m.CloudAssets[3].Path, "engine:Shaders/Programs/M.shader" );
    const auto& header = m.Header;
    if ( !header.has_value() )
    {
        ADD_FAILURE() << "the raised material has no header";
        return;
    }
    EXPECT_EQ( header->Dependencies, ( std::vector<std::string>{ kShd, kTex, kType, kPaint } ) );
    EXPECT_EQ( text.GetValue().find( "TextureHandle" ), std::string::npos );
}

TEST( LegacyMaterialIds, RaisingToMatl4RefusesAnUnknownNumberAndAWrongKindByName )
{
    const LegacyAssetRefMap refs{
         { 303,
           { LegacyAssetKind::CloudLayout, "33333333333333333333333333333333", "assets:Clouds/L.dclayout" } } };
    MaterialDataV2 v2;
    v2.Header =
         Content::MakeTextHeader( Content::ContentKind::Material, G( kParent ), MaterialTextSubsystemsV2() );
    v2.Textures        = { { "u_AlbedoTexture", 999 } };
    const auto unknown = RaiseMaterialV2ToV4( "lost.demat", v2, refs );
    ASSERT_FALSE( unknown );
    EXPECT_NE( unknown.GetError().find( "lost.demat" ), std::string::npos ) << unknown.GetError();
    EXPECT_NE( unknown.GetError().find( "u_AlbedoTexture" ), std::string::npos ) << unknown.GetError();
    EXPECT_NE( unknown.GetError().find( "999" ), std::string::npos ) << unknown.GetError();

    v2.Textures          = { { "CloudType1", 303 } };
    const auto wrongKind = RaiseMaterialV2ToV4( "swapped.demat", v2, refs );
    ASSERT_FALSE( wrongKind );
    EXPECT_NE( wrongKind.GetError().find( "cloud layout" ), std::string::npos ) << wrongKind.GetError();
}

// The shader name: no file with that stem, two files with it, and a file with no header GUID are each refused
// naming the material and the shader - never a material left pointing at nothing.
TEST( LegacyMaterialIds, RaisingToMatl4RefusesAShaderNameNoOneHeaderedFileCarries )
{
    constexpr const char* kShd = "44444444444444444444444444444444";
    LegacyAssetRefMap     refs{ { 404, { LegacyAssetKind::Shader, kShd, "engine:Shaders/Programs/A/M.shader" } },
                                { 405, { LegacyAssetKind::Shader, "", "engine:Shaders/Programs/Bare.shader" } } };
    MaterialDataV2        v2;
    v2.Header =
         Content::MakeTextHeader( Content::ContentKind::Material, G( kParent ), MaterialTextSubsystemsV2() );

    v2.ShaderName      = "Gone";
    const auto unknown = RaiseMaterialV2ToV4( "gone.demat", v2, refs );
    ASSERT_FALSE( unknown );
    EXPECT_NE( unknown.GetError().find( "gone.demat" ), std::string::npos ) << unknown.GetError();
    EXPECT_NE( unknown.GetError().find( "'Gone'" ), std::string::npos ) << unknown.GetError();

    v2.ShaderName   = "Bare";
    const auto bare = RaiseMaterialV2ToV4( "bare.demat", v2, refs );
    ASSERT_FALSE( bare );
    EXPECT_NE( bare.GetError().find( "raise the shader first" ), std::string::npos ) << bare.GetError();

    refs.emplace( 406, LegacyAssetRef{ LegacyAssetKind::Shader, kShd, "engine:Shaders/Programs/B/M.shader" } );
    v2.ShaderName  = "M";
    const auto two = RaiseMaterialV2ToV4( "two.demat", v2, refs );
    ASSERT_FALSE( two );
    EXPECT_NE( two.GetError().find( "A/M.shader" ), std::string::npos ) << two.GetError();
    EXPECT_NE( two.GetError().find( "B/M.shader" ), std::string::npos ) << two.GetError();
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

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
