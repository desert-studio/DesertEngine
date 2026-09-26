#pragma once

// THE FILE FORM OF THE COMPONENTS THAT ARE AUTHORED BY HAND AND HAVE NO REFLECTED DATA BLOCK.
//
// THE DEFECT. Foliage, Locomotion, Morph, Socket and Projectile each have a full Details editor and
// none of them had a line in ComponentRegistry.cpp. So the sliders worked, the values were read by
// their systems, and the next `Open Scene` threw the lot away — with no error, no warning and no
// mark. It is the same defect VisibilityComponent had (U11) and the same one LockComponent had
// before it: a table the user cannot see quietly discarding the user's own edit. It reaches further
// than the scene file, because EVERY subtree copy in this editor goes through the same registry —
// delete-undo, Ctrl+C/Ctrl+V, prefab instancing and the Play/Stop snapshot all lose the same values.
//
// WHY THESE FIVE ARE NOT `MakeReflected`. A reflected component serializes itself for free, and that
// is the right answer whenever the component HAS a `Data` block. These five do not, deliberately:
// each is edited by a bespoke widget (a bone picker fed by the target's skeleton, a clip picker fed
// by the animation library, one slider per blendshape of the mesh) and PROPERTY metadata for a widget
// nobody builds from it would be dead metadata plus a second spelling of every display name.
// `Common::UUID` is also not a reflectable field type, and MorphComponent carries a cache that must
// NOT be written. So the mapping is written out here, once, in a header with no renderer in it.
//
// WHY A HEADER AND NOT THE ANONYMOUS NAMESPACE OF ComponentRegistry.cpp. Everything in that file is
// unreachable from a test: it links the AssetManager and through it the whole renderer. These
// functions are pure — in: a component; out: a JSON object, and back — so
// Desert/Tests/Engine/AuthoredComponentRoundTrip asserts the real trip on the real types without a
// GPU. A serializer nobody can round-trip is how a middle link comes to drop a field.
//
// TWO RULES THAT ARE LOAD-BEARING, AND BOTH ARE TESTED:
//
//   1. AN ABSENT KEY LEAVES THE FIELD ALONE. It does not reset it to the struct default. The two look
//      identical today only because the load path hands us a freshly added component; they stop
//      looking identical the moment anything deserializes onto a component that already exists (undo
//      restoring in place, a ForeignKeys merge). "Absent = keep" is also what lets a field be ADDED
//      to one of these blocks with no scene version bump, which is the rule ForeignKeys.hpp states.
//   2. A UUID IS WRITTEN AS A DECIMAL STRING. A component payload travels as `Json::Value`, whose
//      integer arm is `int64_t`; roughly half of `Common::UUID::Generate()`'s draws are above
//      INT64_MAX and would be stored as a NEGATIVE number in the `.desce`. The bits happen to survive
//      the round trip, so no test of ours would ever have caught it — and a file full of negative
//      entity ids is one hand-edit or one other reader away from being wrong. `Common::UUID` already
//      has a decimal-string spelling (`ToString`), so that is the form we store, and `Json::Node::AsUuid`
//      reads it back STRICTLY: digits only, inside 64 bits — "12abc" and "-1" are refused, not read as 12
//      and UINT64_MAX the way `std::stoull` read them.
//   3. A WRONG-TYPED VALUE IS NEVER SILENT (the wrong-type rule, Common/Json/Document.hpp). Every
//      reader here takes a Json::Node rooted at the component's place in the scene and an Issues list:
//      a value of the wrong type — or a number the field cannot hold, like 1e300 for a float, which a
//      cast would have made infinity — becomes an Issue naming the full path
//      ("Entities[id=4127].Foliage.Density"), and the field keeps the value it had.

#include <Common/Core/Logger.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>
#include <Common/Json/Document.hpp>

#include <Engine/ECS/Components.hpp>

#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Core::Serialize
{
    namespace AuthoredIO
    {
        // A list member read element by element through the scalar arms, so a bad element is an Issue with
        // its own index ("...Morph.Weights[2]"). One bad element keeps the WHOLE list as it was: a list read
        // in part would misalign Weights with TargetNames, which the Details sliders index together.
        template <class T>
        void ReadList( const Common::Json::Node& from, std::string_view key, std::vector<T>& out,
                       Common::Json::Issues& issues )
        {
            const auto member = from.Find( key );
            if ( !member || !member->ExpectKind( Common::Json::Kind::Array, issues ) )
                return;
            const std::size_t before = issues.size();
            std::vector<T>    read;
            member->ForEachElement(
                 [&]( std::size_t, const Common::Json::Node& element )
                 {
                     T value{};
                     element.ReadValue( value, issues );
                     read.push_back( std::move( value ) );
                 } );
            if ( issues.size() == before )
                out = std::move( read );
        }
    } // namespace AuthoredIO

    // ── FOLIAGE TYPE ───────────────────────────────────────────────────────────────────────────────
    // The scatter parameters the paint brush reads (Editor/.../Tools/FoliagePaintTool.cpp). Losing
    // these is the most visible of the five: the next paint dab after a reload scatters at a density
    // and a scale nobody asked for, and the instances already on the terrain do not match it.
    inline Common::Json::Object WriteComponent( const ECS::FoliageComponent& c )
    {
        return Common::Json::ObjectBuilder()
             .Set( "Density", c.Density )
             .Set( "ScaleMin", c.ScaleMin )
             .Set( "ScaleMax", c.ScaleMax )
             .Set( "ZOffsetMin", c.ZOffsetMin )
             .Set( "ZOffsetMax", c.ZOffsetMax )
             .Set( "MaxPitchDeg", c.MaxPitchDeg )
             .Set( "SlopeMinDeg", c.SlopeMinDeg )
             .Set( "SlopeMaxDeg", c.SlopeMaxDeg )
             .Set( "AlignToNormal", c.AlignToNormal )
             .Set( "RandomYaw", c.RandomYaw )
             .Build();
    }

    inline void ReadComponent( const Common::Json::Node& from, ECS::FoliageComponent& c,
                               Common::Json::Issues& issues )
    {
        from.ReadInto( "Density", c.Density, issues );
        from.ReadInto( "ScaleMin", c.ScaleMin, issues );
        from.ReadInto( "ScaleMax", c.ScaleMax, issues );
        from.ReadInto( "ZOffsetMin", c.ZOffsetMin, issues );
        from.ReadInto( "ZOffsetMax", c.ZOffsetMax, issues );
        from.ReadInto( "MaxPitchDeg", c.MaxPitchDeg, issues );
        from.ReadInto( "SlopeMinDeg", c.SlopeMinDeg, issues );
        from.ReadInto( "SlopeMaxDeg", c.SlopeMaxDeg, issues );
        from.ReadInto( "AlignToNormal", c.AlignToNormal, issues );
        from.ReadInto( "RandomYaw", c.RandomYaw, issues );
    }

    // ── LOCOMOTION ─────────────────────────────────────────────────────────────────────────────────
    // The state -> clip mapping LocomotionSystem reads. The clip NAMES are the whole point: a lost
    // name is a character that silently stops walking, because SystemRules falls back to the default
    // spellings ("Idle"/"Walk"/"Run") which the character's own library may not contain.
    inline Common::Json::Object WriteComponent( const ECS::LocomotionComponent& c )
    {
        return Common::Json::ObjectBuilder()
             .Set( "IdleClip", c.IdleClip )
             .Set( "WalkClip", c.WalkClip )
             .Set( "RunClip", c.RunClip )
             .Set( "JumpClip", c.JumpClip )
             .Set( "WalkSpeed", c.WalkSpeed )
             .Set( "RunSpeed", c.RunSpeed )
             .Build();
    }

    inline void ReadComponent( const Common::Json::Node& from, ECS::LocomotionComponent& c,
                               Common::Json::Issues& issues )
    {
        from.ReadInto( "IdleClip", c.IdleClip, issues );
        from.ReadInto( "WalkClip", c.WalkClip, issues );
        from.ReadInto( "RunClip", c.RunClip, issues );
        from.ReadInto( "JumpClip", c.JumpClip, issues );
        from.ReadInto( "WalkSpeed", c.WalkSpeed, issues );
        from.ReadInto( "RunSpeed", c.RunSpeed, issues );
    }

    // ── MORPH TARGETS ──────────────────────────────────────────────────────────────────────────────
    // `AppliedWeights` is NOT written and must not be: it is the last set the runtime blended into the
    // geometry, kept so a per-frame apply can skip work. Writing it would put a frame of cache into
    // every `.desce` and make a saved scene differ from itself depending on when it was saved.
    //
    // Weights and TargetNames are index-aligned by the component's own contract, and this is where
    // that stops being a comment: a file whose two lists disagree is repaired on read and reported,
    // because the Details sliders index one list with the other's size.
    inline Common::Json::Object WriteComponent( const ECS::MorphComponent& c )
    {
        return Common::Json::ObjectBuilder()
             .Set( "Weights", c.Weights )
             .Set( "TargetNames", c.TargetNames )
             .Build();
    }

    inline void ReadComponent( const Common::Json::Node& from, ECS::MorphComponent& c,
                               Common::Json::Issues& issues )
    {
        AuthoredIO::ReadList( from, "Weights", c.Weights, issues );
        AuthoredIO::ReadList( from, "TargetNames", c.TargetNames, issues );

        if ( c.TargetNames.size() != c.Weights.size() )
        {
            LOG_WARN( "[Scene] {0} carries {1} weights against {2} target names; the names were aligned to the "
                      "weights.",
                      from.Where().ToString(), c.Weights.size(), c.TargetNames.size() );
            c.TargetNames.resize( c.Weights.size() );
        }
    }

    // ── SOCKET ATTACHMENT ──────────────────────────────────────────────────────────────────────────
    // `Target` is a reference to another ENTITY of the same scene, written as its UUID (rule 2 above).
    // It is deliberately NOT remapped on duplicate or on prefab instancing — no entity reference in
    // this engine is, ProjectileComponent::Owner included — so a copied weapon follows the same
    // character the original followed. That is right for a weapon and wrong for a prefab that
    // contains both ends; the general remap is a separate piece of work and is named in U13's report.
    // A refused id keeps the old reference: a weapon that stays in the hand is a visible wrong, a weapon
    // on the floor is a bug report about animation.
    inline Common::Json::Object WriteComponent( const ECS::SocketAttachmentComponent& c )
    {
        return Common::Json::ObjectBuilder()
             .Set( "Target", c.Target )
             .Set( "BoneName", c.BoneName )
             .Set( "OffsetTranslation", c.OffsetTranslation )
             .Set( "OffsetRotation", c.OffsetRotation )
             .Set( "OffsetScale", c.OffsetScale )
             .Build();
    }

    inline void ReadComponent( const Common::Json::Node& from, ECS::SocketAttachmentComponent& c,
                               Common::Json::Issues& issues )
    {
        from.ReadInto( "Target", c.Target, issues );
        from.ReadInto( "BoneName", c.BoneName, issues );
        from.ReadInto( "OffsetTranslation", c.OffsetTranslation, issues );
        from.ReadInto( "OffsetRotation", c.OffsetRotation, issues );
        from.ReadInto( "OffsetScale", c.OffsetScale, issues );
    }

    // ── PROJECTILE ─────────────────────────────────────────────────────────────────────────────────
    // Written down as transient until U13 measured the claim. The reason on the exemption was "spawned
    // by a script during Play, so there is no authored projectile to save" — but the Details panel
    // offers `Add Component > Projectile` and then four editable fields, and the normal way to author
    // a bullet is exactly that: an entity carrying velocity/gravity/lifetime/damage, saved as a PREFAB
    // for the firing script to spawn. Prefabs go through this same registry, so that bullet lost its
    // damage; so did every Ctrl+C of it, and so did Play/Stop, whose snapshot is the same code path.
    // `Owner` is stamped by the script at spawn and is null in an authored one — it is written anyway
    // because a block that carries all of a component is one fewer thing to reason about.
    inline Common::Json::Object WriteComponent( const ECS::ProjectileComponent& c )
    {
        return Common::Json::ObjectBuilder()
             .Set( "Velocity", c.Velocity )
             .Set( "GravityScale", c.GravityScale )
             .Set( "LifeRemaining", c.LifeRemaining )
             .Set( "Damage", c.Damage )
             .Set( "Owner", c.Owner )
             .Build();
    }

    inline void ReadComponent( const Common::Json::Node& from, ECS::ProjectileComponent& c,
                               Common::Json::Issues& issues )
    {
        from.ReadInto( "Velocity", c.Velocity, issues );
        from.ReadInto( "GravityScale", c.GravityScale, issues );
        from.ReadInto( "LifeRemaining", c.LifeRemaining, issues );
        from.ReadInto( "Damage", c.Damage, issues );
        from.ReadInto( "Owner", c.Owner, issues );
    }
    // ── LANDSCAPE ──────────────────────────────────────────────────────────────────────────────────
    // Hand-mapped for the reason the file header gives: the tile's `Landscape` is a Common::UUID, which is
    // not a reflectable field type, and the tile carries loaded heights that must NOT be written. The root
    // goes with it so the two halves of one feature are read in one place.
    inline Common::Json::Object WriteComponent( const ECS::LandscapeComponent& c )
    {
        // Written even when empty: an undo restores the whole block, and an absent key keeps the current
        // value (rule 1), so an omitted empty list would leave a just-added layer in place after its undo.
        Common::Json::Value::Array layers;
        layers.reserve( c.Layers.size() );
        for ( const auto& layer : c.Layers )
            layers.emplace_back( Common::Json::ObjectBuilder()
                                      .Set( "Name", layer.Name )
                                      .Set( "Hardness", layer.Hardness )
                                      .Set( "NoWeightBlend", layer.NoWeightBlend )
                                      .Set( "Color", layer.Color )
                                      .Build() );
        return Common::Json::ObjectBuilder()
             .Set( "QuadsPerTile", c.QuadsPerTile )
             .Set( "SpacingCm", c.SpacingCm )
             .Set( "ZScale", c.ZScale )
             .Set( "Layers", Common::Json::Value( std::move( layers ) ) )
             .Build();
    }

    /// The layer list of one Landscape block, or the reason it cannot be one. A name is the key every tile's
    /// weight plane is looked up by, so an empty, over-long or repeated name would make a plane unreachable
    /// or ambiguous; Hardness is a 0..1 share (LandscapeLayerRule). A wrong-typed field of one layer is an
    /// Issue and keeps that field's default; the rules above then judge the layer as read.
    inline Common::ResultStr<std::vector<ECS::LandscapeLayerInfo>>
    ReadLandscapeLayers( const Common::Json::Node& value, Common::Json::Issues& issues )
    {
        using Layers = std::vector<ECS::LandscapeLayerInfo>;
        if ( value.GetKind() != Common::Json::Kind::Array )
            return Common::MakeError<Layers>( "'Layers' is not an array" );
        Layers                     read;
        std::optional<std::string> refusal;
        value.ForEachElement(
             [&]( std::size_t index, const Common::Json::Node& element )
             {
                 if ( refusal )
                     return;
                 if ( element.GetKind() != Common::Json::Kind::Object )
                 {
                     refusal = std::format( "layer {} is not an object", index );
                     return;
                 }
                 ECS::LandscapeLayerInfo layer;
                 element.ReadInto( "Name", layer.Name, issues );
                 element.ReadInto( "Hardness", layer.Hardness, issues );
                 element.ReadInto( "NoWeightBlend", layer.NoWeightBlend, issues );
                 element.ReadInto( "Color", layer.Color, issues );
                 if ( layer.Name.empty() )
                     refusal = std::format( "layer {} has no name", index );
                 else if ( layer.Name.size() > World::Landscape::kLandscapeMaxWeightLayerName )
                     refusal = std::format( "layer '{}' is {} bytes long, the limit is {}", layer.Name,
                                            layer.Name.size(), World::Landscape::kLandscapeMaxWeightLayerName );
                 else if ( std::isnan( layer.Hardness ) || layer.Hardness < 0.0f || layer.Hardness > 1.0f )
                     refusal =
                          std::format( "layer '{}' has Hardness {}, outside 0..1", layer.Name, layer.Hardness );
                 else
                     for ( const auto& earlier : read )
                         if ( earlier.Name == layer.Name )
                             refusal = std::format( "layer '{}' is named twice", layer.Name );
                 if ( !refusal )
                     read.push_back( std::move( layer ) );
             } );
        if ( refusal )
            return Common::MakeError<Layers>( *refusal );
        return Common::MakeSuccess( std::move( read ) );
    }

    // A refused layer list keeps the current one and is an Issue at "...Landscape.Layers": the same report
    // line as a wrong-typed field, because it is the same kind of wrong — a file this build did not write.
    inline void ReadComponent( const Common::Json::Node& from, ECS::LandscapeComponent& c,
                               Common::Json::Issues& issues )
    {
        from.ReadInto( "QuadsPerTile", c.QuadsPerTile, issues );
        from.ReadInto( "SpacingCm", c.SpacingCm, issues );
        from.ReadInto( "ZScale", c.ZScale, issues );
        // Absent in every scene written before layers existed: the list stays as it is (empty on load).
        if ( const auto value = from.Find( "Layers" ) )
        {
            auto layers = ReadLandscapeLayers( *value, issues );
            if ( layers )
                c.Layers = layers.ExtractValue();
            else
                issues.push_back( { value->Where().ToString(), "a valid layer list", layers.GetError() } );
        }
    }

    // `Heights` is not written: it is what `HeightFile` decodes to, and the registry's LandscapeTile
    // serializer is what loads it (ComponentRegistry.cpp). Writing both would be two copies of one terrain.
    inline Common::Json::Object WriteComponent( const ECS::LandscapeTileComponent& c )
    {
        return Common::Json::ObjectBuilder()
             .Set( "Landscape", c.Landscape )
             .Set( "TileX", c.TileX )
             .Set( "TileZ", c.TileZ )
             .Set( "HeightFile", c.HeightFile )
             .Build();
    }

    inline void ReadComponent( const Common::Json::Node& from, ECS::LandscapeTileComponent& c,
                               Common::Json::Issues& issues )
    {
        from.ReadInto( "Landscape", c.Landscape, issues );
        from.ReadInto( "TileX", c.TileX, issues );
        from.ReadInto( "TileZ", c.TileZ, issues );
        from.ReadInto( "HeightFile", c.HeightFile, issues );
    }
} // namespace Desert::Core::Serialize
