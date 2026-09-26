// DOES A UI COMPONENT SURVIVE BEING WRITTEN TO A `.desce` AND READ BACK?
//
// Nothing asked that until now. The UI components are serialized generically — ComponentRegistry's
// MakeReflected walks the reflected field table — so there was no per-component serializer for anyone
// to test, and the generic walker was only ever exercised on a synthetic three-field struct
// (Desert/Tests/Engine/ReflectionSerializer). The result was three defects living together in the one
// path a canvas background travels, all of them shipped, and one of them the reason the background
// looked like a dead setting: it could not be authored so that it survived a save.
//
// This suite is that missing round trip, on the REAL reflected types out of Reflection.gen.cpp, and it
// goes through JSON TEXT rather than staying in rfl::Generic. The text is not decoration: the handle
// corruption that the third defect was about happened in the conversion between the tree and a number,
// and a test that never leaves the tree cannot see it.
//
// The asset resolver is a stub. That is the point of the seam — MakeAssetResolver reaches the
// ResourceRegistry and through it the whole renderer, so no suite can build it; what a scene actually
// needs from it is a string in and a handle out, and the two stubs here are exactly that contract.

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/TextAssetHeader.hpp>

#include <gtest/gtest.h>

#include <Engine/ECS/Components.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Reflection/ReflectionSerializer.hpp>

#include <Common/Json/Json.hpp>

#include <cstdint>
#include <string>
#include <Common/Json/Document.hpp>

namespace
{
    // DeserializeReflected reads a Json::Node and collects wrong-typed values as Issues; every fixture this
    // file feeds it is well-typed, so an Issue is a failure here.
    void ReadReflectedValue( const Desert::Reflection::TypeInfo& type, void* obj, const Common::Json::Value& src,
                             const Desert::Reflection::AssetResolver* resolver = nullptr )
    {
        Common::Json::Issues issues;
        Desert::Reflection::DeserializeReflected( type, obj, Common::Json::Root( src ), issues, resolver );
        for ( const auto& issue : issues )
            ADD_FAILURE() << Common::Json::Describe( issue );
    }
    void ReadReflectedValue( const Desert::Reflection::TypeInfo& type, void* obj, const Common::Json::Object& src,
                             const Desert::Reflection::AssetResolver* resolver = nullptr )
    {
        ReadReflectedValue( type, obj, Common::Json::Value( src ), resolver );
    }
} // namespace

using Desert::Reflection::AssetResolver;
using Desert::Reflection::ReflectionRegistry;
using Desert::Reflection::SerializeReflected;
using Desert::Reflection::TypeInfo;

namespace ECS = Desert::ECS;

namespace
{
    // The handle the third defect was measured on: a texture id is a 64-bit FNV-1a of the asset's
    // project-relative path, so it lands above 2^53 essentially always. Through a double it came back
    // as 5355760296319879168 — 328 out.
    constexpr uint64_t kMeasuredHandle = 5355760296319878840ull;

    // The first integer a double cannot count past, and the first one it gets wrong. 2^53 itself is
    // exactly representable; 2^53 + 1 is not, and rounds to 2^53.
    constexpr uint64_t kDoubleExactLimit  = 1ull << 53; // 9 007 199 254 740 992
    constexpr uint64_t kFirstLostByDouble = ( 1ull << 53 ) + 1ull;

    // A handle above 2^63, which the path hash produces about half the time. It has to survive the
    // int64 the file carries being REINTERPRETED rather than converted.
    constexpr uint64_t kAboveInt64 = 0xF0E1D2C3B4A59687ull;

    const TypeInfo& Type( const std::string& name )
    {
        const TypeInfo* type = ReflectionRegistry::Get().Find( name );
        EXPECT_NE( type, nullptr ) << "the reflected type '" << name
                                   << "' is not registered, so no scene can carry it";
        return *type;
    }

    // What a `.desce` actually holds between the two halves of the trip: JSON text.
    Common::Json::Object ThroughJsonText( const Common::Json::Object& written )
    {
        const std::string text   = Common::Json::Write( written );
        const auto        parsed = Common::Json::Read<Common::Json::Object>( text );
        SCOPED_TRACE( "what the serializer wrote must be a JSON object: " + text );
        EXPECT_TRUE( parsed.IsSuccess() );
        return parsed ? parsed.GetValue() : Common::Json::Object{};
    }

    // THE ONE PLACE A TEXTURE REFERENCE BECOMES A STRING AND BACK, stubbed. The real resolver stores
    // the root-tagged stable key (`cooked:Textures/T.tex`), which is the form that survives being
    // carried to another machine; this stub stands in for the registry lookup with a fixed pair, so
    // the suite asserts the SHAPE of the round trip rather than re-testing TextureSlotRoundTrip.
    constexpr uint64_t kResolvedHandle = kMeasuredHandle;
    const std::string  kResolvedKey    = "cooked:Textures/T_Checker.tex";

    // The theme slot's own pair, and it is a DIFFERENT KEY on purpose: UIPanelData also declares a
    // `Video` slot (VideoAsset) and the canvas now declares a theme, so a resolver that answered every
    // type alike would let a sprite pass on a video's branch and a theme on a texture's. Two types, two
    // keys, and neither key is accepted on the other's branch.
    const std::string kThemeKey = "UI/Themes/Desert_Dark.detheme";
    const char*       kResolvedGuidText = "00112233445566778899aabbccddeeff";

    AssetResolver KeyResolver()
    {
        AssetResolver r;
        r.ToPath = []( uint64_t handle, const std::string& type ) -> std::string
        {
            if ( type == "TextureAsset" && handle == kResolvedHandle )
                return kResolvedKey;
            if ( type == "UIThemeAsset" && handle == kResolvedHandle )
                return kThemeKey;
            return std::string();
        };
        r.FromPath = []( const std::string& key, const std::string& type ) -> uint64_t
        {
            if ( type == "TextureAsset" && key == kResolvedKey )
                return kResolvedHandle;
            if ( type == "UIThemeAsset" && key == kThemeKey )
                return kResolvedHandle;
            return 0ull;
        };
        // SCNE 30: a texture slot is stored as {Guid, Path}. The GUID text is the handle's own hex, and
        // FromGuid answers the handle ResolveGuidRef derives from it, so the GUID is the route back in.
        r.ToGuid = []( uint64_t handle, const std::string& type ) -> std::string
        { return type == "TextureAsset" && handle == kResolvedHandle ? kResolvedGuidText : std::string(); };
        r.FromGuid = []( uint64_t guid, const std::string& type ) -> uint64_t
        {
            const auto parsed = Common::Content::AssetGuidFromText( kResolvedGuidText );
            return type == "TextureAsset" &&
                             guid == static_cast<uint64_t>( Common::Content::HandleForGuid( parsed.GetValue() ) )
                        ? kResolvedHandle
                        : 0ull;
        };
        return r;
    }
} // namespace

// --- (1) The canvas background, end to end -------------------------------------------------------
//
// The setting the whole task is about. A background sprite set in Details must come back as the same
// texture after the scene is written and reopened, and it must be written as a form that names a place
// in the PROJECT rather than on the machine that saved it.
TEST( UIComponentRoundTrip, ACanvasBackgroundSurvivesTheTripAndIsStoredByProjectRelativeKey )
{
    ECS::UICanvasData written;
    written.Sprite = Desert::Assets::AssetHandle( kResolvedHandle );

    const AssetResolver resolver = KeyResolver();
    const auto          object   = SerializeReflected( Type( "UICanvasData" ), &written, &resolver );

    const auto stored = object.get( "Sprite" );
    ASSERT_TRUE( stored.has_value() ) << "the canvas wrote no Sprite field at all";
    const Common::Json::Node ref = Common::Json::Root( stored.value() );
    SCOPED_TRACE( "the sprite must be written as a {Guid, Path} reference (SCNE 30) - a bare key or a raw id is "
                  "not an identity" );
    ASSERT_EQ( ref.GetKind(), Common::Json::Kind::Object );
    const auto field = [&]( const char* key ) -> std::string
    {
        const auto v = ref.Find( key );
        if ( !v.has_value() )
            return std::string();
        const auto text = v->AsString();
        return text ? text.GetValue() : std::string();
    };
    EXPECT_EQ( field( "Guid" ), kResolvedGuidText ) << "the identity half of the reference is not the GUID";
    const std::string path = field( "Path" );
    ASSERT_FALSE( path.empty() ) << "the locator half of the reference is empty";
    EXPECT_EQ( path, kResolvedKey );
    EXPECT_NE( path.find( ':' ), std::string::npos )
         << "the stored locator carries no root tag, so it is a bare path and the reader cannot tell which "
            "of the project's roots it is relative to";
    EXPECT_NE( path.front(), '/' )
         << "the stored locator is an absolute path, i.e. a directory that exists on one machine only";

    ECS::UICanvasData read;
    ReadReflectedValue( Type( "UICanvasData" ), &read, ThroughJsonText( object ), &resolver );

    EXPECT_EQ( static_cast<uint64_t>( read.Sprite ), kResolvedHandle )
         << "the canvas background did not come back: the setting is dead in the only way that matters, "
            "which is that it cannot be authored so that it survives a save";
}

// --- (2) The rest of the canvas, so the fix is not one field wide --------------------------------
TEST( UIComponentRoundTrip, EveryAuthoredCanvasFieldComesBack )
{
    ECS::UICanvasData written;
    written.ScaleMode        = ECS::UICanvasScaleMode::Letterbox;
    written.RenderMode       = ECS::UICanvasRenderMode::WorldSpace;
    written.WorldScale       = 1234.5f;
    written.ReferenceWidth   = 1920.0f;
    written.ReferenceHeight  = 1080.0f;
    written.MatchWidthHeight = 0.25f;
    written.Sprite           = Desert::Assets::AssetHandle( kResolvedHandle );
    written.Visible          = false;
    written.SafeArea         = glm::vec4( 4.0f, 8.0f, 12.0f, 16.0f );
    // Ю13's three. The theme HANDLE in particular is the field whose absence from this list would be the
    // sixth instance of "authored and silently not saved" (У13): a canvas whose theme did not survive a
    // save looks exactly like a theme system that works, until the scene is reopened.
    written.Theme        = Desert::Assets::AssetHandle( kResolvedHandle );
    written.FontScale    = 1.75f;
    written.HighContrast = true;

    const AssetResolver resolver = KeyResolver();
    const auto          object   = SerializeReflected( Type( "UICanvasData" ), &written, &resolver );

    ECS::UICanvasData read;
    ReadReflectedValue( Type( "UICanvasData" ), &read, ThroughJsonText( object ), &resolver );

    EXPECT_EQ( read.ScaleMode, written.ScaleMode );
    EXPECT_EQ( read.RenderMode, written.RenderMode );
    EXPECT_FLOAT_EQ( read.WorldScale, written.WorldScale );
    EXPECT_FLOAT_EQ( read.ReferenceWidth, written.ReferenceWidth );
    EXPECT_FLOAT_EQ( read.ReferenceHeight, written.ReferenceHeight );
    EXPECT_FLOAT_EQ( read.MatchWidthHeight, written.MatchWidthHeight );
    EXPECT_EQ( static_cast<uint64_t>( read.Sprite ), kResolvedHandle );
    EXPECT_EQ( read.Visible, written.Visible );
    EXPECT_EQ( read.SafeArea, written.SafeArea );
    EXPECT_EQ( static_cast<uint64_t>( read.Theme ), kResolvedHandle );
    EXPECT_FLOAT_EQ( read.FontScale, written.FontScale );
    EXPECT_EQ( read.HighContrast, written.HighContrast );
}

// THE ELEMENT'S HALF OF THE SAME QUESTION. Two fields, and both of them decide what the element is drawn
// with: a Source that came back as Theme on an element authored Local repaints it from the palette, and a
// Style that came back empty falls the element back to Default with no message, because "" is a style name
// the theme genuinely does not declare and the walk reports it as a typo rather than as a lost field.
TEST( UIComponentRoundTrip, TheElementStyleSurvivesTheTrip )
{
    ECS::UIStyleData written;
    written.Source = ECS::UIStyleSource::Local;
    written.Style  = "Primary";

    const AssetResolver resolver = KeyResolver();
    const auto          object   = SerializeReflected( Type( "UIStyleData" ), &written, &resolver );

    ECS::UIStyleData read;
    ReadReflectedValue( Type( "UIStyleData" ), &read, ThroughJsonText( object ), &resolver );

    EXPECT_EQ( read.Source, written.Source );
    EXPECT_EQ( read.Style, written.Style );
}

// Ю17. EVERY field of the new container, because a list whose Item Height comes back at zero is a list
// that renders one row of one design pixel — and the only way to see that is to author it, save, and
// reopen. It is also the component with an INT field, which no other UI round trip here covers.
TEST( UIComponentRoundTrip, EveryListViewFieldComesBack )
{
    ECS::UIListViewData written;
    written.ScrollY        = 1234.5f;
    written.ItemHeight     = 73.0f;
    written.Spacing        = 6.0f;
    written.Overscan       = 4;
    written.Background     = glm::vec3( 0.21f, 0.22f, 0.23f );
    written.ShowScrollbar  = false;
    written.ScrollbarColor = glm::vec3( 0.71f, 0.72f, 0.73f );

    const AssetResolver resolver = KeyResolver();
    const auto          object   = SerializeReflected( Type( "UIListViewData" ), &written, &resolver );

    ECS::UIListViewData read;
    ReadReflectedValue( Type( "UIListViewData" ), &read, ThroughJsonText( object ), &resolver );

    EXPECT_FLOAT_EQ( read.ScrollY, written.ScrollY );
    EXPECT_FLOAT_EQ( read.ItemHeight, written.ItemHeight );
    EXPECT_FLOAT_EQ( read.Spacing, written.Spacing );
    EXPECT_EQ( read.Overscan, written.Overscan );
    EXPECT_EQ( read.Background, written.Background );
    EXPECT_EQ( read.ShowScrollbar, written.ShowScrollbar );
    EXPECT_EQ( read.ScrollbarColor, written.ScrollbarColor );
}

// --- (3) The other two UI slots that carry an asset ----------------------------------------------
//
// UIImage's Sprite and UIPanel's Sprite go down the same branch. Asserting them here is not repetition:
// the defect was in the SHARED path, so a fix that only reached the canvas would be a fix in exactly
// one of the three places a UI asset reference lives.
TEST( UIComponentRoundTrip, TheImageAndPanelSpriteSlotsTakeTheSameRoute )
{
    const AssetResolver resolver = KeyResolver();

    {
        ECS::UIImageData written;
        written.Sprite    = Desert::Assets::AssetHandle( kResolvedHandle );
        const auto object = SerializeReflected( Type( "UIImageData" ), &written, &resolver );

        ECS::UIImageData read;
        ReadReflectedValue( Type( "UIImageData" ), &read, ThroughJsonText( object ), &resolver );
        EXPECT_EQ( static_cast<uint64_t>( read.Sprite ), kResolvedHandle );
    }
    {
        ECS::UIPanelData written;
        written.Sprite    = Desert::Assets::AssetHandle( kResolvedHandle );
        const auto object = SerializeReflected( Type( "UIPanelData" ), &written, &resolver );

        ECS::UIPanelData read;
        ReadReflectedValue( Type( "UIPanelData" ), &read, ThroughJsonText( object ), &resolver );
        EXPECT_EQ( static_cast<uint64_t>( read.Sprite ), kResolvedHandle );
    }
}

// --- (4) The handle itself, through the text, at the sizes that break -----------------------------
//
// With no resolver a handle is written as a raw integer, which is the form a `.desce` carries for any
// slot whose reference could not be turned into a path. Through a double that number is destroyed
// above 2^53, and a UI handle is above 2^53 essentially always. Asserted on the value it was MEASURED
// wrong on, and on both sides of the boundary, because a fix that rounds correctly at the limit and
// wrongly one past it is the failure this catches.
TEST( UIComponentRoundTrip, ARawHandleSurvivesTheJsonTextExactlyAtEverySize )
{
    for ( const uint64_t handle :
          { kMeasuredHandle, kDoubleExactLimit, kFirstLostByDouble, kAboveInt64, 1ull, 0ull } )
    {
        ECS::UICanvasData written;
        written.Sprite = Desert::Assets::AssetHandle( handle );

        // No resolver: the AssetHandle field takes the raw-integer route on both sides.
        const auto object = SerializeReflected( Type( "UICanvasData" ), &written, nullptr );

        ECS::UICanvasData read;
        read.Sprite = Desert::Assets::AssetHandle( 0xDEADBEEFull ); // so "unchanged" cannot pass as "read"
        ReadReflectedValue( Type( "UICanvasData" ), &read, ThroughJsonText( object ), nullptr );

        EXPECT_EQ( static_cast<uint64_t>( read.Sprite ), handle )
             << "handle " << handle
             << " did not survive the JSON text; a double holds 53 bits and a "
                "texture id is 64";
    }
}

// --- (5) The documented behaviour of a missing key ------------------------------------------------
//
// A record that does not mention a field leaves it at what it already held. Asserted rather than
// assumed, because the two halves of a migration depend on it in opposite directions: an old file
// that has no `Sprite` must not zero a default, and a new field added tomorrow must not read as 0
// from every scene written before it.
TEST( UIComponentRoundTrip, AFieldTheRecordDoesNotMentionKeepsWhatItHad )
{
    Common::Json::Object partial;
    partial["Visible"] = Common::Json::Value( false );

    ECS::UICanvasData read;
    read.Sprite         = Desert::Assets::AssetHandle( kMeasuredHandle );
    read.ReferenceWidth = 640.0f;

    ReadReflectedValue( Type( "UICanvasData" ), &read, ThroughJsonText( partial ), nullptr );

    EXPECT_FALSE( read.Visible ) << "the one field the record DID state was not read";
    EXPECT_EQ( static_cast<uint64_t>( read.Sprite ), kMeasuredHandle )
         << "an absent key cleared a field instead of leaving it alone";
    EXPECT_FLOAT_EQ( read.ReferenceWidth, 640.0f );
}

// --- (6) An unresolvable reference is refused, not silently zeroed --------------------------------
//
// The read side used to answer 0 and say nothing, which is indistinguishable from "this canvas has no
// background" — so a scene whose texture had moved opened looking correct and saved the loss back out.
// The stub answers 0 for an unknown key exactly as the real resolver does for a missing file; what
// this asserts is that the serializer ASKS rather than skipping the field, so the refusal reaches the
// resolver where the log lives.
TEST( UIComponentRoundTrip, AReferenceThatResolvesToNothingReachesTheResolverRatherThanBeingSkipped )
{
    int asked = 0;

    int located = 0;

    // A texture that HAS an identity but is gone from the registry: ToGuid answers its GUID (the handle
    // as the low half, so every handle has one), FromGuid knows nothing, FromPath finds no file.
    AssetResolver counting;
    counting.ToGuid = []( uint64_t handle, const std::string& type ) -> std::string
    {
        return type == "TextureAsset" && handle != 0
                    ? Common::Content::AssetGuidToText( Common::Content::AssetGuid{ 0ull, handle } )
                    : std::string();
    };
    counting.FromGuid = [&asked]( uint64_t guid, const std::string& type ) -> uint64_t
    {
        if ( type != "TextureAsset" )
            return 0ull;
        ++asked;
        EXPECT_EQ( guid, static_cast<uint64_t>( Common::Content::HandleForGuid(
                              Common::Content::AssetGuid{ 0ull, kMeasuredHandle } ) ) );
        return 0ull;
    };
    counting.ToPath = []( uint64_t, const std::string& type ) -> std::string
    { return type == "TextureAsset" ? "cooked:Textures/Gone.tex" : std::string(); };
    counting.FromPath = [&located]( const std::string& key, const std::string& type ) -> uint64_t
    {
        if ( type != "TextureAsset" )
            return 0ull;
        ++located;
        EXPECT_EQ( key, "cooked:Textures/Gone.tex" );
        return 0ull;
    };

    ECS::UICanvasData written;
    written.Sprite    = Desert::Assets::AssetHandle( kMeasuredHandle );
    const auto object = SerializeReflected( Type( "UICanvasData" ), &written, &counting );

    ECS::UICanvasData read;
    read.Sprite = Desert::Assets::AssetHandle( 7ull );
    ReadReflectedValue( Type( "UICanvasData" ), &read, ThroughJsonText( object ), &counting );

    EXPECT_EQ( asked, 1 ) << "the stored reference never reached the resolver, so nothing could report "
                             "that it did not resolve";
    EXPECT_EQ( located, 1 ) << "an unknown GUID was not followed to its locator, so a moved texture could "
                               "never be told apart from a deleted one";
    EXPECT_EQ( static_cast<uint64_t>( read.Sprite ), 0ull );
}

// --- (7) The render transform (Ю8) survives the trip ---------------------------------------------
//
// Three fields joined UILayoutData, and У13 had just closed a class of five components that could be
// authored and never reached the file. A transform that does not survive a save is the same defect
// wearing a matrix, and it is invisible in the editor — the element looks right until it is reopened.
//
// The values are deliberately not defaults: Rotation nonzero, Scale non-uniform AND not 1, Pivot away
// from the centre. A round trip that dropped a field and left the default behind would be caught by
// each of the three separately.
TEST( UIComponentRoundTrip, TheRenderTransformSurvivesTheTrip )
{
    ECS::UILayoutData written;
    written.Rotation = -37.5f;
    written.Scale    = { 1.75f, 0.25f };
    written.Pivot    = { 0.125f, 0.875f };

    const auto object = SerializeReflected( Type( "UILayoutData" ), &written, nullptr );

    ECS::UILayoutData read;
    ReadReflectedValue( Type( "UILayoutData" ), &read, ThroughJsonText( object ), nullptr );

    EXPECT_FLOAT_EQ( read.Rotation, -37.5f );
    EXPECT_FLOAT_EQ( read.Scale.x, 1.75f );
    EXPECT_FLOAT_EQ( read.Scale.y, 0.25f );
    EXPECT_FLOAT_EQ( read.Pivot.x, 0.125f );
    EXPECT_FLOAT_EQ( read.Pivot.y, 0.875f );
}

// The other direction, and the one every scene in the repository depends on: NONE of them states these
// keys, because they were written before the fields existed. An absent key has to leave the field
// alone, or every existing .desce would come back with a zeroed Scale — an element scaled to nothing.
TEST( UIComponentRoundTrip, ALayoutRecordFromBeforeTheTransformExistedLeavesItNeutral )
{
    // Exactly the keys UI_ElementProbe.desce carries for a UILayout, and not one more.
    Common::Json::Object old;
    old["AnchorMin"] = Common::Json::Value(
         Common::Json::Value::Array{ Common::Json::Value( 0.0 ), Common::Json::Value( 0.0 ) } );
    old["AnchorMax"] = Common::Json::Value(
         Common::Json::Value::Array{ Common::Json::Value( 1.0 ), Common::Json::Value( 1.0 ) } );
    old["OffsetMin"] = Common::Json::Value(
         Common::Json::Value::Array{ Common::Json::Value( 0.0 ), Common::Json::Value( 0.0 ) } );
    old["OffsetMax"] = Common::Json::Value(
         Common::Json::Value::Array{ Common::Json::Value( 0.0 ), Common::Json::Value( 0.0 ) } );
    old["CustomMinimumSize"] = Common::Json::Value(
         Common::Json::Value::Array{ Common::Json::Value( 0.0 ), Common::Json::Value( 0.0 ) } );
    old["ClipContents"] = Common::Json::Value( false );

    ECS::UILayoutData read; // its defaults ARE the neutral transform
    ReadReflectedValue( Type( "UILayoutData" ), &read, ThroughJsonText( old ), nullptr );

    EXPECT_FLOAT_EQ( read.Rotation, 0.0f );
    EXPECT_FLOAT_EQ( read.Scale.x, 1.0f ) << "an old scene came back with its element scaled away";
    EXPECT_FLOAT_EQ( read.Scale.y, 1.0f ) << "an old scene came back with its element scaled away";
    EXPECT_FLOAT_EQ( read.Pivot.x, 0.5f );
    EXPECT_FLOAT_EQ( read.Pivot.y, 0.5f );
    // ...and the keys it DID state still arrived, so this is not passing because the read did nothing.
    EXPECT_FLOAT_EQ( read.AnchorMax.x, 1.0f );
}

int main( int argc, char** argv )
{
    ReflectionRegistry::Get().ResolveStructLinks();
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
