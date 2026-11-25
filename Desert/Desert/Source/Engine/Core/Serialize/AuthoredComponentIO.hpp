#pragma once

// THE FILE FORM OF THE COMPONENTS THAT ARE AUTHORED BY HAND AND HAVE NO REFLECTED DATA BLOCK.
//
// THE DEFECT. Foliage, Locomotion, Morph, Socket and Projectile each had a full Details editor and
// none of them had a line in ComponentRegistry.cpp. So the sliders worked, the values were read by
// their systems, and the next `Open Scene` threw the lot away — with no error, no warning and no
// mark. It is the same defect VisibilityComponent had (U11) and the same one LockComponent had
// before it: a table the user cannot see quietly discarding the user's own edit. It reaches further
// than the scene file, because EVERY subtree copy in this editor goes through the same registry —
// delete-undo, Ctrl+C/Ctrl+V, prefab instancing and the Play/Stop snapshot all lose the same values.
//
// (Projectile has since become a reflected ProjectileData block, LEG-PROJECTILE.)
//
// WHY THESE FOUR ARE NOT `MakeReflected`. A reflected component serializes itself for free, and that
// is the right answer whenever the component HAS a `Data` block. These four do not, deliberately:
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
//      ("Entities[id=4127].Locomotion.WalkSpeed"), and the field keeps the value it had.

#include <Common/Core/Logger.hpp>
#include <Common/Json/Document.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <Engine/ECS/MorphComponent.hpp>
#include <Engine/ECS/SocketAttachmentComponent.hpp>

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

    // ── MORPH TARGETS ──────────────────────────────────────────────────────────────────────────────
    // `Revision` is NOT written and must not be: it counts the changes of Weights in THIS session so the
    // renderer re-blends only on a change. Writing it would put a session's edit count into every `.desce`
    // and make a saved scene differ from itself depending on how often it was edited.
    //
    // Weights and TargetNames are index-aligned by the component's own contract, and this is where
    // that stops being a comment: a file whose two lists disagree is repaired on read and reported,
    // because the Details sliders index one list with the other's size.
    inline Common::Json::Object WriteComponent( const ECS::MorphComponent& c )
    {
        return Common::Json::ObjectBuilder()
             .Set( "Weights", c.GetWeights() )
             .Set( "TargetNames", c.TargetNames )
             .Build();
    }

    inline void ReadComponent( const Common::Json::Node& from, ECS::MorphComponent& c,
                               Common::Json::Issues& issues )
    {
        // Through the setter: it moves the Revision when the read changed a weight, so a component the
        // renderer already blended is re-blended.
        std::vector<float> weights = c.GetWeights();
        AuthoredIO::ReadList( from, "Weights", weights, issues );
        c.SetWeights( std::move( weights ) );
        AuthoredIO::ReadList( from, "TargetNames", c.TargetNames, issues );

        if ( c.TargetNames.size() != c.GetWeights().size() )
        {
            LOG_WARN( "[Scene] {0} carries {1} weights against {2} target names; the names were aligned to the "
                      "weights.",
                      from.Where().ToString(), c.GetWeights().size(), c.TargetNames.size() );
            c.TargetNames.resize( c.GetWeights().size() );
        }
    }

    // ── SOCKET ATTACHMENT ──────────────────────────────────────────────────────────────────────────
    // `Target` is a reference to another ENTITY of the same scene, written as its UUID (rule 2 above).
    // It is deliberately NOT remapped on duplicate or on prefab instancing — no entity reference in
    // this engine is — so a copied weapon follows the same
    // character the original followed. That is right for a weapon and wrong for a prefab that
    // contains both ends; the general remap is a separate piece of work and is named in U13's report.
    // A refused id keeps the old reference: a weapon that stays in the hand is a visible wrong, a weapon
    // on the floor is a bug report about animation.
    inline Common::Json::Object WriteComponent( const ECS::SocketAttachmentComponent& c )
    {
        return Common::Json::ObjectBuilder()
             .Set( "Target", c.Target )
             .Set( "SocketName", c.SocketName )
             .Set( "OffsetTranslation", c.OffsetTranslation )
             .Set( "OffsetRotation", c.OffsetRotation )
             .Set( "OffsetScale", c.OffsetScale )
             .Build();
    }

    inline void ReadComponent( const Common::Json::Node& from, ECS::SocketAttachmentComponent& c,
                               Common::Json::Issues& issues )
    {
        from.ReadInto( "Target", c.Target, issues );
        from.ReadInto( "SocketName", c.SocketName, issues );
        from.ReadInto( "OffsetTranslation", c.OffsetTranslation, issues );
        from.ReadInto( "OffsetRotation", c.OffsetRotation, issues );
        from.ReadInto( "OffsetScale", c.OffsetScale, issues );
    }
} // namespace Desert::Core::Serialize
