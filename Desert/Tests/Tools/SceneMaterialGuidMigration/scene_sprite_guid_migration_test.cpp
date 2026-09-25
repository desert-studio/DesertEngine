// SCNE 30 (T6f): the UI sprite slots - UICanvas.Sprite, UIPanel.Sprite, UIImage.Sprite, UIButton.Sprite /
// HoverSprite / PressedSprite - and Settings.SplashSprite stop being a bare stable key and become
// `{"Guid": <the .detex header GUID>, "Path": "assets:<path under the assets root>"}`, the form SCNE 29 gave
// the skybox (MigrateSpriteGuidsV29ToV30). A key whose file is missing, lies outside the assets root, or states
// no texture GUID REFUSES the file, naming the slot.
//
// The second half is the writer: every reflected TextureAsset field serializes to that same object through the
// resolver's one ToGuid, resolves back through ResolveGuidRef, and a bare string is no longer read.

#include <SceneMigration.hpp>
#include <Engine/Core/SceneSettings.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Reflection/ReflectionSerializer.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/TextAssetHeader.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace Migration = Desert::Migration;
namespace fs        = std::filesystem;

using Common::Content::AssetGuid;

namespace
{
    const AssetGuid kSpriteGuid{ 0x0123456789abcdefULL, 0x1122334455667788ULL };
    const char*     kSpriteGuidText = "0123456789abcdef1122334455667788";
    const char*     kSpriteKey      = "assets:Textures/UI/Sprite.detex";

    uint64_t SpriteHandle()
    {
        return static_cast<uint64_t>( Common::Content::HandleForGuid( kSpriteGuid ) );
    }

    void WriteDetex( const fs::path& file, const AssetGuid& guid, Common::Content::ContentKind kind )
    {
        fs::create_directories( file.parent_path() );
        Common::Content::AssetEnvelope envelope;
        envelope.Asset.Kind = kind;
        envelope.Asset.Guid = guid;
        envelope.Sections.push_back( { Common::Content::EnvelopeSection::ImportInfo,
                                       Common::Content::EnvelopeCodec::Stored,
                                       std::vector<std::byte>( 16, std::byte{ 7 } ) } );
        const auto written = Common::Content::WriteAssetEnvelopeFile( file, envelope );
        ASSERT_TRUE( written ) << written.GetError();
    }

    // <root>/Resources/Assets/Textures/UI/Sprite.detex stating kSpriteGuid, and a material-kind file beside it.
    struct Project
    {
        fs::path Root;
        fs::path AssetsRoot;

        explicit Project( const char* name )
        {
            Root       = fs::temp_directory_path() / ( std::string( "t6f_" ) + name );
            AssetsRoot = Root / "Resources" / "Assets";
            fs::remove_all( Root );
            WriteDetex( AssetsRoot / "Textures" / "UI" / "Sprite.detex", kSpriteGuid,
                        Common::Content::ContentKind::Texture );
            WriteDetex( AssetsRoot / "Textures" / "UI" / "NotATexture.detex", AssetGuid{ 5, 5 },
                        Common::Content::ContentKind::Material );
        }
        ~Project()
        {
            std::error_code ec;
            fs::remove_all( Root, ec );
        }
        Project( const Project& )            = delete;
        Project& operator=( const Project& ) = delete;
    };

    std::string Quoted( const std::string& s )
    {
        return rfl::json::write( rfl::Generic( s ) );
    }

    // The version the fixtures state: the one step before the sprite-GUID step.
    constexpr int kBeforeSpriteGuids = Migration::kSceneVersionSpriteGuids - 1;

    // Every sprite slot SCNE 30 raises holds `value`: four components, a prefab override and the settings.
    std::string V29Scene( const std::string& value )
    {
        const std::string v = Quoted( value );
        return std::string( R"({"Header":{"Kind":"Scene","Guid":"00000000000000000000000000000001",)" ) +
               R"("Versions":{"SCNE":)" + std::to_string( kBeforeSpriteGuids ) +
               R"(,"UNIT":1},"Dependencies":[]},"SceneName":"S",)" + R"("Settings":{"SplashSprite":)" + v +
               R"(,"SplashDuration":1.0},"Entities":[)" + R"({"id":1,"Tag":"Canvas","UICanvas":{"Sprite":)" + v +
               R"(}},)" + R"({"id":2,"Tag":"Panel","UIPanel":{"Sprite":)" + v + R"(,"Opacity":1.0}},)" +
               R"({"id":3,"Tag":"Image","UIImage":{"Sprite":)" + v + R"(}},)" +
               R"({"id":4,"Tag":"Button","UIButton":{"Sprite":)" + v + R"(,"HoverSprite":)" + v +
               R"(,"PressedSprite":)" + v + R"(}},)" +
               R"({"id":5,"Tag":"Inst","PrefabPath":"p.deprefab","PrefabOverrides":[{"Path":[],)" +
               R"("UIPanel":{"Sprite":)" + v + R"(}}]}]})";
    }

    Migration::SceneSerialized Parse( const std::string& json )
    {
        auto parsed = rfl::json::read<Migration::SceneSerialized>( json );
        EXPECT_TRUE( parsed.has_value() ) << ( parsed.has_value() ? "" : parsed.error().what() );
        return parsed.has_value() ? parsed.value() : Migration::SceneSerialized{};
    }

    std::size_t Count( const std::string& text, const std::string& what )
    {
        std::size_t n = 0;
        for ( auto at = text.find( what ); at != std::string::npos; at = text.find( what, at + 1 ) )
            ++n;
        return n;
    }

    std::string SpriteRef()
    {
        return std::string( R"({"Guid":")" ) + kSpriteGuidText + R"(","Path":")" + kSpriteKey + R"("})";
    }
} // namespace

TEST( SceneSpriteGuidMigration, EverySpriteSlotAndTheSplashBecomeTheHeaderGuidAndTheKey )
{
    const Project project( "key" );
    auto          scene  = Parse( V29Scene( kSpriteKey ) );
    const auto    report = Migration::MigrateScene( scene, project.AssetsRoot, "", {} );

    ASSERT_TRUE( report.Refused.empty() ) << report.Refused;
    EXPECT_TRUE( report.SpriteGuidsRaised );
    EXPECT_FALSE( report.TextureGuidsRaised ) << "the file was already past the texture-GUID step";
    EXPECT_EQ( report.SpriteGuids.Rewritten, 8 );
    const std::string text = rfl::json::write( scene );
    EXPECT_EQ( Count( text, SpriteRef() ), 8u ) << text;
    EXPECT_EQ( Count( text, std::string( R"("SplashSprite":)" ) + SpriteRef() ), 1u ) << text;
    EXPECT_EQ( Count( text, std::string( R"("HoverSprite":)" ) + SpriteRef() ), 1u ) << text;
    EXPECT_EQ( Count( text, R"("Opacity":1.0)" ), 1u ) << "the other fields of the component are kept: " << text;
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ),
               Desert::Core::kSceneVersion );
}

TEST( SceneSpriteGuidMigration, AnEmptySpriteBecomesAnEmptyReference )
{
    const Project project( "empty" );
    auto          scene  = Parse( V29Scene( "" ) );
    const auto    report = Migration::MigrateScene( scene, project.AssetsRoot, "", {} );

    ASSERT_TRUE( report.Refused.empty() ) << report.Refused;
    EXPECT_EQ( report.SpriteGuids.Rewritten, 0 );
    EXPECT_EQ( Count( rfl::json::write( scene ), R"({"Guid":"","Path":""})" ), 8u );
}

TEST( SceneSpriteGuidMigration, ANonexistentFileRefusesNamingEverySlotAndLeavesTheSceneUnstamped )
{
    const Project project( "missing" );
    auto          scene  = Parse( V29Scene( "assets:Textures/UI/Gone.detex" ) );
    const auto    report = Migration::MigrateScene( scene, project.AssetsRoot, "", {} );

    ASSERT_FALSE( report.Refused.empty() );
    for ( const char* site :
          { "Settings > SplashSprite", "Canvas > UICanvas.Sprite", "Panel > UIPanel.Sprite",
            "Image > UIImage.Sprite", "Button > UIButton.Sprite", "Button > UIButton.HoverSprite",
            "Button > UIButton.PressedSprite", "Inst > PrefabOverrides[0] > UIPanel.Sprite" } )
        EXPECT_NE( report.Refused.find( site ), std::string::npos ) << site << " in " << report.Refused;
    EXPECT_NE( report.Refused.find( "Gone.detex" ), std::string::npos ) << report.Refused;
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ), kBeforeSpriteGuids )
         << "a refused file was restamped";
}

TEST( SceneSpriteGuidMigration, AFileThatIsNotATextureRefuses )
{
    const Project project( "kind" );
    auto          scene  = Parse( V29Scene( "assets:Textures/UI/NotATexture.detex" ) );
    const auto    report = Migration::MigrateScene( scene, project.AssetsRoot, "", {} );

    ASSERT_FALSE( report.Refused.empty() );
    EXPECT_NE( report.Refused.find( "is not a texture" ), std::string::npos ) << report.Refused;
}

TEST( SceneSpriteGuidMigration, ASecondRunOfTheStepChangesNothing )
{
    const Project project( "twice" );
    auto          scene = Parse( V29Scene( kSpriteKey ) );
    ASSERT_TRUE( Migration::MigrateScene( scene, project.AssetsRoot, "", {} ).Refused.empty() );
    const std::string first = rfl::json::write( scene );

    auto       entities = scene.Entities;
    auto       settings = scene.Settings;
    const auto again    = Migration::MigrateSpriteGuidsV29ToV30( settings, entities, project.AssetsRoot );
    EXPECT_EQ( again.Rewritten, 0 );
    EXPECT_TRUE( again.UnknownNames.empty() );
    scene.Entities = entities;
    scene.Settings = settings;
    EXPECT_EQ( rfl::json::write( scene ), first );

    const auto rerun = Migration::MigrateScene( scene, project.AssetsRoot, "", {} );
    EXPECT_FALSE( rerun.SpriteGuidsRaised );
    EXPECT_EQ( rfl::json::write( scene ), first );
}

// ---- The writer ------------------------------------------------------------------------------------------

namespace
{
    // The resolver MakeAssetResolver builds, reduced to one texture: its GUID, its key, its handle.
    Desert::Reflection::AssetResolver SpriteResolver( int* fromGuidAsked = nullptr )
    {
        Desert::Reflection::AssetResolver r;
        r.ToPath = []( uint64_t handle, const std::string& type ) -> std::string
        { return type == "TextureAsset" && handle == SpriteHandle() ? kSpriteKey : ""; };
        r.ToGuid = []( uint64_t handle, const std::string& type ) -> std::string
        { return type == "TextureAsset" && handle == SpriteHandle() ? kSpriteGuidText : ""; };
        r.FromPath = []( const std::string& path, const std::string& type ) -> uint64_t
        { return type == "TextureAsset" && path == kSpriteKey ? SpriteHandle() : 0; };
        r.FromGuid = [fromGuidAsked]( uint64_t guid, const std::string& type ) -> uint64_t
        {
            if ( fromGuidAsked )
                ++*fromGuidAsked;
            return type == "TextureAsset" && guid == SpriteHandle() ? guid : 0;
        };
        return r;
    }

    const Desert::Reflection::TypeInfo& TypeNamed( const char* name )
    {
        const auto* type = Desert::Reflection::ReflectionRegistry::Get().Find( name );
        EXPECT_NE( type, nullptr ) << name;
        return *type;
    }
} // namespace

TEST( SpriteReferenceWriter, AnAssignedButtonSpriteIsWrittenAsItsGuidAndReadBackThroughIt )
{
    Desert::ECS::UIButtonData button;
    button.HoverSprite  = Desert::Assets::AssetHandle( SpriteHandle() );
    const auto resolver = SpriteResolver();

    const auto&                type = TypeNamed( "UIButtonData" );
    const rfl::Generic::Object out  = Desert::Reflection::SerializeReflected( type, &button, &resolver );
    const std::string          text = rfl::json::write( out );
    EXPECT_NE( text.find( std::string( R"("HoverSprite":)" ) + SpriteRef() ), std::string::npos ) << text;
    EXPECT_NE( text.find( R"("Sprite":{"Guid":"","Path":""})" ), std::string::npos ) << text;

    Desert::ECS::UIButtonData back;
    int                       asked = 0;
    const auto                again = SpriteResolver( &asked );
    Desert::Reflection::DeserializeReflected( type, &back, out, &again );
    EXPECT_EQ( static_cast<uint64_t>( back.HoverSprite ), SpriteHandle() );
    EXPECT_EQ( static_cast<uint64_t>( back.Sprite ), 0u );
    EXPECT_EQ( asked, 1 ) << "the GUID is the identity and is asked first";
}

TEST( SpriteReferenceWriter, TheSplashSpriteIsWrittenAsItsGuid )
{
    Desert::Core::SceneSettings settings;
    settings.SplashSprite      = Desert::Assets::AssetHandle( SpriteHandle() );
    const auto        resolver = SpriteResolver();
    const std::string text     = rfl::json::write(
         Desert::Reflection::SerializeReflected( TypeNamed( "SceneSettings" ), &settings, &resolver ) );
    EXPECT_NE( text.find( std::string( R"("SplashSprite":)" ) + SpriteRef() ), std::string::npos ) << text;
}

TEST( SpriteReferenceWriter, ABareStringIsNoLongerReadAsASprite )
{
    rfl::Generic::Object in;
    in["Sprite"] = std::string( kSpriteKey );
    Desert::ECS::UIPanelData panel;
    const auto               resolver = SpriteResolver();
    Desert::Reflection::DeserializeReflected( TypeNamed( "UIPanelData" ), &panel, in, &resolver );
    EXPECT_EQ( static_cast<uint64_t>( panel.Sprite ), 0u );
}
