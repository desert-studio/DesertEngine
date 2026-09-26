// DOES AN AUTHORED COMPONENT WITH NO REFLECTED DATA BLOCK SURVIVE BEING WRITTEN AND READ BACK?
//
// Five of them did not survive anything at all until U13: Foliage, Locomotion, Morph,
// SocketAttachment and Projectile each had a full Details editor and no line in ComponentRegistry.cpp,
// so every value an artist set was discarded by the next load. `ComponentPersistence` next door is the
// gate that stops a SIXTH one existing — it asserts that the component is named by the table. This
// suite asserts the other half, which a name cannot: that the mapping is COMPLETE and EXACT, field by
// field, through the JSON text a `.desce` actually holds.
//
// The two halves are both needed and neither implies the other. A component can be registered and drop
// three of its fields silently (that is the "middle link drops a property" shape this project has now
// paid for seven times), and a mapping can be perfect while nothing calls it.
//
// NOTHING FROM THE RENDERER IS LINKED. AuthoredComponentIO.hpp is a pure header on purpose — in: a
// component, out: an object, and back — which is the only reason this trip can be asserted at all.
// ComponentRegistry.cpp itself cannot be: it links the AssetManager and through it the whole engine.

#include <gtest/gtest.h>

#include <Engine/Core/Serialize/AuthoredComponentIO.hpp>

#include <Common/Json/Document.hpp>

#include <rflcpp/rfl/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace ECS = Desert::ECS;

using Desert::Core::Serialize::ReadComponent;
using Desert::Core::Serialize::WriteComponent;

namespace
{
    // Where the reads below are rooted, so an Issue's path can be compared whole.
    Common::Json::Path BlockPath()
    {
        return Common::Json::Path().Key( "Entities" ).Record( "7" ).Key( "Block" );
    }

    // ReadComponent at a component's place in a scene; the Issues are what the load would report.
    template <class TComponent>
    Common::Json::Issues ReadAt( const rfl::Generic::Object& block, TComponent& read )
    {
        const rfl::Generic   value( block );
        Common::Json::Issues issues;
        ReadComponent( Common::Json::Root( value, BlockPath() ), read, issues );
        return issues;
    }

    // What a `.desce` holds between the two halves of the trip: JSON TEXT. Staying in `rfl::Generic`
    // would hide exactly the class of defect the UUID rule in AuthoredComponentIO.hpp is about — a
    // value that is fine in the tree and wrong on disk.
    rfl::Generic::Object ThroughJsonText( const rfl::Generic::Object& written )
    {
        const std::string text   = rfl::json::write( rfl::Generic( written ) );
        const auto        parsed = rfl::json::read<rfl::Generic>( text );
        EXPECT_TRUE( parsed.has_value() ) << "what the serializer wrote is not valid JSON: " << text;
        if ( !parsed.has_value() )
            return {};
        const auto object = parsed.value().to_object();
        EXPECT_TRUE( object.has_value() );
        return object.has_value() ? object.value() : rfl::Generic::Object{};
    }

    template <class TComponent>
    TComponent RoundTrip( const TComponent& written )
    {
        TComponent read;
        ReadAt( ThroughJsonText( WriteComponent( written ) ), read );
        return read;
    }

    std::string JsonTextOf( const rfl::Generic::Object& object )
    {
        return rfl::json::write( rfl::Generic( object ) );
    }

    // ── THE FIELD CENSUS ───────────────────────────────────────────────────────────────────────────
    //
    // Comparing the keys the writer emits against the fields the STRUCT declares. Without this, adding
    // a field to FoliageComponent and forgetting AuthoredComponentIO.hpp is silent: the component still
    // round-trips, the suite still passes, and one slider out of eleven stops being saved.

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/ECS/Components.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string WithoutLineComments( const std::string& source )
    {
        std::string out;
        out.reserve( source.size() );
        std::size_t at = 0;
        while ( at < source.size() )
        {
            const std::size_t line = source.find( '\n', at );
            const std::size_t end  = line == std::string::npos ? source.size() : line;
            std::string       text = source.substr( at, end - at );
            if ( const auto comment = text.find( "//" ); comment != std::string::npos )
                text = text.substr( 0, comment );
            out += text;
            out += '\n';
            at = end == source.size() ? end : end + 1;
        }
        return out;
    }

    std::string Trimmed( const std::string& text )
    {
        const auto first = text.find_first_not_of( " \t\r\n" );
        if ( first == std::string::npos )
            return {};
        const auto last = text.find_last_not_of( " \t\r\n" );
        return text.substr( first, last - first + 1 );
    }

    // The data-member names of `struct <name>` in @p source. Methods are skipped (a member function's
    // declaration carries a `(` before its `;`), and so is anything that is not a declaration.
    std::set<std::string> DeclaredFieldsOf( const std::string& source, const std::string& name )
    {
        const std::size_t at = source.find( "struct " + name );
        if ( at == std::string::npos )
            return {};
        const std::size_t open = source.find( '{', at );
        if ( open == std::string::npos )
            return {};

        int         depth = 0;
        std::size_t close = open;
        for ( ; close < source.size(); ++close )
        {
            if ( source[close] == '{' )
                ++depth;
            else if ( source[close] == '}' && --depth == 0 )
                break;
        }

        const std::string     body = source.substr( open + 1, close - open - 1 );
        std::set<std::string> fields;
        std::size_t           from = 0;
        while ( from < body.size() )
        {
            const std::size_t semicolon = body.find( ';', from );
            if ( semicolon == std::string::npos )
                break;
            std::string statement = Trimmed( body.substr( from, semicolon - from ) );
            from                  = semicolon + 1;

            // The initialiser goes FIRST and the method test comes after it. The other order reads
            // `Common::UUID Owner = Common::UUID::Null()` as a member function because of the parens in
            // its default, and silently drops the field from the left-hand side of the census — which
            // is the census failing in the direction that looks like a pass.
            if ( const auto assign = statement.find( '=' ); assign != std::string::npos )
                statement = Trimmed( statement.substr( 0, assign ) );
            if ( statement.empty() || statement.find( '(' ) != std::string::npos )
                continue;

            const auto gap = statement.find_last_of( " \t*&>" );
            if ( gap == std::string::npos )
                continue;
            const std::string field = Trimmed( statement.substr( gap + 1 ) );
            if ( !field.empty() &&
                 ( std::isalpha( static_cast<unsigned char>( field.front() ) ) || field.front() == '_' ) )
                fields.insert( field );
        }
        return fields;
    }

    std::set<std::string> KeysOf( const rfl::Generic::Object& object )
    {
        std::set<std::string> keys;
        for ( const auto& [key, value] : object )
        {
            (void)value;
            keys.insert( key );
        }
        return keys;
    }

    // A struct field that is deliberately NOT written, with the reason. Exactly one exists; a second
    // one is a conversation, because "not saved" is the defect this whole task is about.
    const std::map<std::string, std::string> kNotWritten = {
         { "AppliedWeights",
           "MorphComponent's cache of the last weights the runtime blended into the geometry. Writing a "
           "frame of cache into the file would make a saved scene differ from itself depending on when "
           "it was saved." },
         { "Heights",
           "LandscapeTileComponent's loaded tile: what HeightFile decodes to. The samples live in the DLHT "
           "file beside the scene, and writing them into the block too would be two copies of one terrain." },
    };

    // Asserts that every declared field of @p structName is a key of @p written, except the ones
    // kNotWritten excuses — and that each excuse still names a real field.
    void ExpectEveryFieldIsWritten( const std::string& structName, const rfl::Generic::Object& written )
    {
        const std::string root = RepoRoot();
        ASSERT_FALSE( root.empty() ) << "repository root not found from the test's working directory";
        const std::string source = WithoutLineComments(
             [&]
             {
                 std::ifstream      in( root + "Desert/Desert/Source/Engine/ECS/Components.hpp" );
                 std::ostringstream ss;
                 ss << in.rdbuf();
                 return ss.str();
             }() );
        ASSERT_FALSE( source.empty() ) << "could not read Components.hpp";

        const std::set<std::string> declared = DeclaredFieldsOf( source, structName );
        ASSERT_FALSE( declared.empty() ) << "no fields parsed out of struct " << structName
                                         << " — the parse has stopped matching the header, and an empty "
                                            "left-hand side agrees with any writer at all";

        const std::set<std::string> keys = KeysOf( written );

        std::string missing;
        for ( const auto& field : declared )
        {
            if ( keys.count( field ) != 0 || kNotWritten.count( field ) != 0 )
                continue;
            missing += "\n  " + structName + "::" + field;
        }
        EXPECT_TRUE( missing.empty() )
             << "These fields are declared on the component and are not written by "
                "AuthoredComponentIO.hpp, so whatever is set in them is lost on the next load while the "
                "component as a whole looks persisted — which is the more expensive half of the defect, "
                "because the registration reads as a fix. Map them, or add them to kNotWritten in this "
                "file WITH THE REASON:"
             << missing;

        // A key with no field behind it is the mirror defect: it reads as persistence of something that
        // no longer exists, and the next reader believes it.
        std::string orphaned;
        for ( const auto& key : keys )
            if ( declared.count( key ) == 0 )
                orphaned += "\n  " + structName + " writes '" + key + "', which is not a field of it";
        EXPECT_TRUE( orphaned.empty() ) << orphaned;
    }
} // namespace

// ── FOLIAGE ────────────────────────────────────────────────────────────────────────────────────────
TEST( AuthoredComponentRoundTrip, EveryFoliageFieldComesBack )
{
    ECS::FoliageComponent written;
    written.Density       = 23.0f;
    written.ScaleMin      = 0.11f;
    written.ScaleMax      = 4.75f;
    written.ZOffsetMin    = -12.5f;
    written.ZOffsetMax    = 37.25f;
    written.MaxPitchDeg   = 18.5f;
    written.SlopeMinDeg   = 7.0f;
    written.SlopeMaxDeg   = 62.5f;
    written.AlignToNormal = false; // both bools default TRUE, so false is the value a marker would eat
    written.RandomYaw     = false;

    const ECS::FoliageComponent read = RoundTrip( written );

    EXPECT_FLOAT_EQ( read.Density, written.Density );
    EXPECT_FLOAT_EQ( read.ScaleMin, written.ScaleMin );
    EXPECT_FLOAT_EQ( read.ScaleMax, written.ScaleMax );
    EXPECT_FLOAT_EQ( read.ZOffsetMin, written.ZOffsetMin );
    EXPECT_FLOAT_EQ( read.ZOffsetMax, written.ZOffsetMax );
    EXPECT_FLOAT_EQ( read.MaxPitchDeg, written.MaxPitchDeg );
    EXPECT_FLOAT_EQ( read.SlopeMinDeg, written.SlopeMinDeg );
    EXPECT_FLOAT_EQ( read.SlopeMaxDeg, written.SlopeMaxDeg );
    EXPECT_FALSE( read.AlignToNormal );
    EXPECT_FALSE( read.RandomYaw );
}

TEST( AuthoredComponentRoundTrip, TheFoliageBlockNamesEveryFoliageField )
{
    ExpectEveryFieldIsWritten( "FoliageComponent", WriteComponent( ECS::FoliageComponent{} ) );
}

// ── LOCOMOTION ─────────────────────────────────────────────────────────────────────────────────────
TEST( AuthoredComponentRoundTrip, EveryLocomotionFieldComesBack )
{
    ECS::LocomotionComponent written;
    written.IdleClip  = "Anim_Idle_Relaxed";
    written.WalkClip  = "Anim_Walk_Fwd";
    written.RunClip   = "Anim_Sprint";
    written.JumpClip  = "Anim_Jump_Start";
    written.WalkSpeed = 33.75f;
    written.RunSpeed  = 412.5f;

    const ECS::LocomotionComponent read = RoundTrip( written );

    EXPECT_EQ( read.IdleClip, written.IdleClip );
    EXPECT_EQ( read.WalkClip, written.WalkClip );
    EXPECT_EQ( read.RunClip, written.RunClip );
    EXPECT_EQ( read.JumpClip, written.JumpClip );
    EXPECT_FLOAT_EQ( read.WalkSpeed, written.WalkSpeed );
    EXPECT_FLOAT_EQ( read.RunSpeed, written.RunSpeed );
}

TEST( AuthoredComponentRoundTrip, TheLocomotionBlockNamesEveryLocomotionField )
{
    ExpectEveryFieldIsWritten( "LocomotionComponent", WriteComponent( ECS::LocomotionComponent{} ) );
}

// ── MORPH ──────────────────────────────────────────────────────────────────────────────────────────
TEST( AuthoredComponentRoundTrip, EveryMorphWeightComesBackIndexAligned )
{
    ECS::MorphComponent written;
    written.Weights        = { 0.0f, 0.25f, 1.0f, 0.75f };
    written.TargetNames    = { "Smile", "BrowRaise", "JawOpen", "EyeBlink_L" };
    written.AppliedWeights = { 9.0f, 9.0f, 9.0f, 9.0f };

    const ECS::MorphComponent read = RoundTrip( written );

    ASSERT_EQ( read.Weights.size(), written.Weights.size() );
    for ( std::size_t i = 0; i < written.Weights.size(); ++i )
        EXPECT_FLOAT_EQ( read.Weights[i], written.Weights[i] ) << "weight " << i;
    EXPECT_EQ( read.TargetNames, written.TargetNames );

    // The cache is not part of the file, so a fresh component must come back with it EMPTY rather than
    // with a frame of somebody else's blend in it.
    EXPECT_TRUE( read.AppliedWeights.empty() )
         << "AppliedWeights was written to the scene; it is a per-frame cache and a saved scene would "
            "differ from itself depending on when it was saved";
}

TEST( AuthoredComponentRoundTrip, AMorphBlockWhoseTwoListsDisagreeIsRepairedNotTrusted )
{
    // Hand-written, or written by a build whose mesh had more targets. The Details sliders index the
    // names with the weights' size, so a longer name list is a read past the end waiting to happen.
    rfl::Generic::Object block;
    block["Weights"] = rfl::Generic( rfl::Generic::Array{ rfl::Generic( 0.5 ), rfl::Generic( 0.25 ) } );
    block["TargetNames"] =
         rfl::Generic( rfl::Generic::Array{ rfl::Generic( std::string( "A" ) ), rfl::Generic( std::string( "B" ) ),
                                            rfl::Generic( std::string( "C" ) ) } );

    ECS::MorphComponent read;
    ReadAt( ThroughJsonText( block ), read );

    EXPECT_EQ( read.Weights.size(), 2u );
    EXPECT_EQ( read.TargetNames.size(), read.Weights.size() )
         << "the two lists came back out of step, and every widget that reads them assumes they are not";
}

TEST( AuthoredComponentRoundTrip, TheMorphBlockNamesEveryMorphField )
{
    ExpectEveryFieldIsWritten( "MorphComponent", WriteComponent( ECS::MorphComponent{} ) );
}

// ── SOCKET ATTACHMENT ──────────────────────────────────────────────────────────────────────────────
TEST( AuthoredComponentRoundTrip, EverySocketFieldComesBack )
{
    ECS::SocketAttachmentComponent written;
    written.Target            = Common::UUID( 1234567890123456789ull );
    written.BoneName          = "mixamorig:RightHand";
    written.OffsetTranslation = glm::vec3( 1.5f, -2.25f, 3.125f );
    written.OffsetRotation    = glm::vec3( 0.5f, 1.25f, -0.75f );
    written.OffsetScale       = glm::vec3( 2.0f, 0.5f, 1.75f );

    const ECS::SocketAttachmentComponent read = RoundTrip( written );

    EXPECT_EQ( static_cast<uint64_t>( read.Target ), static_cast<uint64_t>( written.Target ) );
    EXPECT_EQ( read.BoneName, written.BoneName );
    EXPECT_EQ( read.OffsetTranslation, written.OffsetTranslation );
    EXPECT_EQ( read.OffsetRotation, written.OffsetRotation );
    EXPECT_EQ( read.OffsetScale, written.OffsetScale );
}

// THE RULE THAT COST A DESIGN DECISION. `Common::UUID::Generate()` draws over the whole 64-bit range,
// so about half of all entity ids are above INT64_MAX — and a component payload travels as
// `rfl::Generic`, whose integer arm is `int64_t`. Stored as a number, those ids land in the `.desce`
// as NEGATIVE integers. The bits survive our own round trip, which is precisely why no round-trip test
// would ever have found it; what would have found it is the first person to read the file.
TEST( AuthoredComponentRoundTrip, ASocketTargetAboveInt64MaxIsStoredAsDecimalTextAndComesBackExact )
{
    ECS::SocketAttachmentComponent written;
    written.Target = Common::UUID( 0xF0E1D2C3B4A59687ull ); // 17357386176853808775, above INT64_MAX

    const rfl::Generic::Object object = WriteComponent( written );

    const auto stored = object.get( "Target" );
    ASSERT_TRUE( stored.has_value() ) << "the socket wrote no Target at all";
    ASSERT_TRUE( stored.value().to_string().has_value() )
         << "the entity reference was written as a number; over half of this engine's ids do not fit "
            "the int64 that a scene's generic tree carries, and the file gets a negative id";

    const std::string text = JsonTextOf( object );
    EXPECT_EQ( text.find( '-' ), std::string::npos )
         << "a negative number reached the file — the id overflowed on the way out: " << text;
    EXPECT_NE( text.find( "17357386176853808775" ), std::string::npos )
         << "the id is not in the file in its own decimal spelling: " << text;

    const ECS::SocketAttachmentComponent read = RoundTrip( written );
    EXPECT_EQ( static_cast<uint64_t>( read.Target ), 0xF0E1D2C3B4A59687ull );
}

TEST( AuthoredComponentRoundTrip, TheSocketBlockNamesEverySocketField )
{
    ExpectEveryFieldIsWritten( "SocketAttachmentComponent", WriteComponent( ECS::SocketAttachmentComponent{} ) );
}

// ── PROJECTILE ─────────────────────────────────────────────────────────────────────────────────────
TEST( AuthoredComponentRoundTrip, EveryProjectileFieldComesBack )
{
    ECS::ProjectileComponent written;
    written.Velocity      = glm::vec3( 100.0f, 25.5f, -3000.0f );
    written.GravityScale  = 0.35f;
    written.LifeRemaining = 12.5f;
    written.Damage        = 47.5f;
    written.Owner         = Common::UUID( 9876543210987654321ull );

    const ECS::ProjectileComponent read = RoundTrip( written );

    EXPECT_EQ( read.Velocity, written.Velocity );
    EXPECT_FLOAT_EQ( read.GravityScale, written.GravityScale );
    EXPECT_FLOAT_EQ( read.LifeRemaining, written.LifeRemaining );
    EXPECT_FLOAT_EQ( read.Damage, written.Damage );
    EXPECT_EQ( static_cast<uint64_t>( read.Owner ), static_cast<uint64_t>( written.Owner ) );
}

TEST( AuthoredComponentRoundTrip, TheProjectileBlockNamesEveryProjectileField )
{
    ExpectEveryFieldIsWritten( "ProjectileComponent", WriteComponent( ECS::ProjectileComponent{} ) );
}

// ── THE RULE THE LOAD PATH HIDES ───────────────────────────────────────────────────────────────────
//
// An absent key must LEAVE THE FIELD ALONE, not reset it to the struct default. Today the two are
// indistinguishable because the loader always hands the reader a component it has just added — so the
// defaults would hold either way, and a reader who assumed "absent = default" would be right by
// accident. They stop being indistinguishable the moment anything deserializes onto a component that
// already exists, which undo-in-place and a ForeignKeys merge both do. "Absent = keep" is also the
// property that lets a field be ADDED to one of these blocks with no scene version bump.
TEST( AuthoredComponentRoundTrip, AnAbsentKeyLeavesTheFieldAsItIs )
{
    ECS::FoliageComponent foliage;
    foliage.Density       = 42.0f;
    foliage.AlignToNormal = false;
    ReadAt( rfl::Generic::Object{}, foliage );
    EXPECT_FLOAT_EQ( foliage.Density, 42.0f ) << "an empty block reset a float to its struct default";
    EXPECT_FALSE( foliage.AlignToNormal )
         << "an empty block turned a false flag back to true — the exact way a marker registration "
            "counterfeits persistence";

    ECS::LocomotionComponent locomotion;
    locomotion.RunClip  = "Anim_Sprint";
    locomotion.RunSpeed = 999.0f;
    ReadAt( rfl::Generic::Object{}, locomotion );
    EXPECT_EQ( locomotion.RunClip, "Anim_Sprint" );
    EXPECT_FLOAT_EQ( locomotion.RunSpeed, 999.0f );

    ECS::SocketAttachmentComponent socket;
    socket.Target   = Common::UUID( 777ull );
    socket.BoneName = "mixamorig:Head";
    ReadAt( rfl::Generic::Object{}, socket );
    EXPECT_EQ( static_cast<uint64_t>( socket.Target ), 777ull );
    EXPECT_EQ( socket.BoneName, "mixamorig:Head" );

    ECS::MorphComponent morph;
    morph.Weights     = { 0.5f };
    morph.TargetNames = { "Smile" };
    ReadAt( rfl::Generic::Object{}, morph );
    ASSERT_EQ( morph.Weights.size(), 1u );
    EXPECT_FLOAT_EQ( morph.Weights[0], 0.5f );
    EXPECT_EQ( morph.TargetNames.size(), 1u );
}

// A key that is present and of the wrong type is a different case from an absent one: nothing this
// project writes can produce it, so the value is refused and the field keeps what it had rather than
// silently becoming zero.
TEST( AuthoredComponentRoundTrip, AKeyOfTheWrongTypeIsRefusedRatherThanSilentlyZeroing )
{
    rfl::Generic::Object block;
    block["Density"]       = rfl::Generic( std::string( "lots" ) );
    block["AlignToNormal"] = rfl::Generic( 3.5 );
    block["ScaleMax"]      = rfl::Generic( 8.5 ); // a good key beside the bad ones still lands

    ECS::FoliageComponent foliage;
    foliage.Density = 42.0f;
    ReadAt( ThroughJsonText( block ), foliage );

    EXPECT_FLOAT_EQ( foliage.Density, 42.0f );
    EXPECT_TRUE( foliage.AlignToNormal );
    EXPECT_FLOAT_EQ( foliage.ScaleMax, 8.5f );
}

// ── LANDSCAPE ──────────────────────────────────────────────────────────────────────────────────────
// Values off every default, a negative tile coordinate (a landscape extends either side of its root) and an
// id above INT64_MAX, which is the half of the id space rule 2 exists for.
TEST( AuthoredComponentRoundTrip, EveryLandscapeFieldComesBack )
{
    ECS::LandscapeComponent written;
    written.QuadsPerTile = 127u;
    written.SpacingCm    = 50.0f;
    written.ZScale       = 256.0f;

    const ECS::LandscapeComponent read = RoundTrip( written );
    EXPECT_EQ( read.QuadsPerTile, written.QuadsPerTile );
    EXPECT_FLOAT_EQ( read.SpacingCm, written.SpacingCm );
    EXPECT_FLOAT_EQ( read.ZScale, written.ZScale );
}

namespace
{
    ECS::LandscapeComponent TwoLayerLandscape()
    {
        ECS::LandscapeComponent c;
        c.Layers.push_back( { "Grass", 0.25f, false, glm::vec3( 0.1f, 0.8f, 0.2f ) } );
        c.Layers.push_back( { "Puddles", 1.0f, true, glm::vec3( 0.0f, 0.3f, 0.9f ) } );
        return c;
    }
} // namespace

// Every field of every layer, off its default, in order: the order is the panel's and the paint rules'.
TEST( AuthoredComponentRoundTrip, EveryLandscapeLayerFieldComesBackInOrder )
{
    const ECS::LandscapeComponent written = TwoLayerLandscape();
    const ECS::LandscapeComponent read    = RoundTrip( written );
    ASSERT_EQ( read.Layers.size(), 2u );
    for ( size_t i = 0; i < 2; ++i )
    {
        EXPECT_EQ( read.Layers[i].Name, written.Layers[i].Name );
        EXPECT_FLOAT_EQ( read.Layers[i].Hardness, written.Layers[i].Hardness );
        EXPECT_EQ( read.Layers[i].NoWeightBlend, written.Layers[i].NoWeightBlend );
        EXPECT_EQ( read.Layers[i].Color, written.Layers[i].Color );
    }
}

// A scene written before layers existed has no "Layers" key and loads with none.
TEST( AuthoredComponentRoundTrip, AnOldLandscapeBlockReadsNoLayers )
{
    rfl::Generic::Object block;
    block["QuadsPerTile"] = rfl::Generic( static_cast<int64_t>( 63 ) );
    ECS::LandscapeComponent read;
    ReadAt( ThroughJsonText( block ), read );
    EXPECT_EQ( read.QuadsPerTile, 63u );
    EXPECT_TRUE( read.Layers.empty() );
}

// Undo restores the block written BEFORE a layer was added: that block must empty the list, so the empty
// list is written, not omitted (an absent key keeps the current value).
TEST( AuthoredComponentRoundTrip, AnEmptyLayerListReplacesTheCurrentOne )
{
    ECS::LandscapeComponent current = TwoLayerLandscape();
    ReadAt( ThroughJsonText( WriteComponent( ECS::LandscapeComponent{} ) ), current );
    EXPECT_TRUE( current.Layers.empty() );
}

// A name is the key a tile's weight plane is found by: an empty, over-long or repeated one, or a Hardness
// outside 0..1, refuses the WHOLE list, and the component keeps the layers it had.
TEST( AuthoredComponentRoundTrip, ABadLayerListIsRefusedWhole )
{
    const auto withLayers = []( std::vector<ECS::LandscapeLayerInfo> layers )
    {
        ECS::LandscapeComponent c;
        c.Layers = std::move( layers );
        return WriteComponent( c );
    };
    const std::string longName( Desert::World::Landscape::kLandscapeMaxWeightLayerName + 1, 'x' );
    const std::vector<rfl::Generic::Object> bad = {
         withLayers( { { "Rock", 0.5f, false, glm::vec3( 1.0f ) }, { "Rock", 0.5f, false, glm::vec3( 1.0f ) } } ),
         withLayers( { { "", 0.5f, false, glm::vec3( 1.0f ) } } ),
         withLayers( { { longName, 0.5f, false, glm::vec3( 1.0f ) } } ),
         withLayers( { { "Rock", 1.5f, false, glm::vec3( 1.0f ) } } ),
    };
    for ( const auto& block : bad )
    {
        ECS::LandscapeComponent current = TwoLayerLandscape();
        ReadAt( ThroughJsonText( block ), current );
        ASSERT_EQ( current.Layers.size(), 2u );
        EXPECT_EQ( current.Layers[0].Name, "Grass" );
        EXPECT_EQ( current.Layers[1].Name, "Puddles" );
    }
    // The longest legal name is accepted: the limit is inclusive, as the tile blob's is.
    ECS::LandscapeComponent current;
    ReadAt( ThroughJsonText(
                 withLayers( { { std::string( Desert::World::Landscape::kLandscapeMaxWeightLayerName, 'x' ), 0.5f,
                                 false, glm::vec3( 1.0f ) } } ) ),
            current );
    EXPECT_EQ( current.Layers.size(), 1u );
}

TEST( AuthoredComponentRoundTrip, TheLandscapeBlockNamesEveryLandscapeField )
{
    ExpectEveryFieldIsWritten( "LandscapeComponent", WriteComponent( ECS::LandscapeComponent{} ) );
}

TEST( AuthoredComponentRoundTrip, EveryLandscapeTileFieldComesBack )
{
    ECS::LandscapeTileComponent written;
    written.Landscape  = Common::UUID( 0xF00DFACE12345678ull );
    written.TileX      = -3;
    written.TileZ      = 7;
    written.HeightFile = "Resources/Assets/Scenes/Hills_Landscape/42.dlht";

    const ECS::LandscapeTileComponent read = RoundTrip( written );
    EXPECT_EQ( static_cast<uint64_t>( read.Landscape ), static_cast<uint64_t>( written.Landscape ) );
    EXPECT_EQ( read.TileX, written.TileX );
    EXPECT_EQ( read.TileZ, written.TileZ );
    EXPECT_EQ( read.HeightFile, written.HeightFile );
    EXPECT_FALSE( read.Heights.has_value() ) << "the block carries a file name, never the samples";
}

TEST( AuthoredComponentRoundTrip, TheLandscapeTileBlockNamesEveryLandscapeTileField )
{
    ExpectEveryFieldIsWritten( "LandscapeTileComponent", WriteComponent( ECS::LandscapeTileComponent{} ) );
}

// A tile coordinate that does not fit is refused, not wrapped: 2^32 narrowed into an int32 is 0, which is
// a real tile, and a fractional coordinate truncated is a real tile too.
TEST( AuthoredComponentRoundTrip, ALandscapeTileCoordinateThatDoesNotFitIsRefused )
{
    rfl::Generic::Object block;
    block["TileX"] = rfl::Generic( static_cast<int64_t>( 4294967296ll ) );
    block["TileZ"] = rfl::Generic( 2.5 );

    ECS::LandscapeTileComponent tile;
    tile.TileX = 5;
    tile.TileZ = 6;
    ReadAt( ThroughJsonText( block ), tile );
    EXPECT_EQ( tile.TileX, 5 );
    EXPECT_EQ( tile.TileZ, 6 );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

// THE SILENT READS THE OLD READERS MADE (JS1c S3). Each of these values was accepted by
// AuthoredIO::ReadUUID/ReadFloat without a word: `std::stoull` reads "12abc" as 12 and "-1" as UINT64_MAX (a
// socket re-targeted at an entity nobody named), and a double cast to float turns 1e300 into infinity. The
// wrong-type rule refuses each one with the field's full path, and the field keeps its value.
TEST( AuthoredComponentRoundTrip, AnIdWithTrailingJunkIsRefusedNotReadAsItsDigits )
{
    ECS::SocketAttachmentComponent socket;
    socket.Target = Common::UUID( 42 );
    rfl::Generic::Object block;
    block["Target"]   = rfl::Generic( std::string( "12abc" ) );
    const auto issues = ReadAt( block, socket );
    EXPECT_EQ( static_cast<uint64_t>( socket.Target ), 42u );
    ASSERT_EQ( issues.size(), 1u );
    EXPECT_EQ( issues[0].Path, BlockPath().Key( "Target" ).ToString() );
}

TEST( AuthoredComponentRoundTrip, ANegativeIdIsRefusedNotWrappedToTheLargestId )
{
    ECS::ProjectileComponent projectile;
    projectile.Owner = Common::UUID( 42 );
    rfl::Generic::Object block;
    block["Owner"]    = rfl::Generic( std::string( "-1" ) );
    const auto issues = ReadAt( block, projectile );
    EXPECT_EQ( static_cast<uint64_t>( projectile.Owner ), 42u );
    ASSERT_EQ( issues.size(), 1u );
    EXPECT_EQ( issues[0].Path, BlockPath().Key( "Owner" ).ToString() );
}

TEST( AuthoredComponentRoundTrip, ANumberAFloatCannotHoldIsRefusedNotReadAsInfinity )
{
    ECS::FoliageComponent foliage;
    const float           density = foliage.Density;
    rfl::Generic::Object  block;
    block["Density"]  = rfl::Generic( 1e300 );
    const auto issues = ReadAt( ThroughJsonText( block ), foliage );
    EXPECT_EQ( foliage.Density, density );
    ASSERT_EQ( issues.size(), 1u );
    EXPECT_EQ( issues[0].Path, BlockPath().Key( "Density" ).ToString() );
    EXPECT_EQ( issues[0].Expected, "number" );
}
