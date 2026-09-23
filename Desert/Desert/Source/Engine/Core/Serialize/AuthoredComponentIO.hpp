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
//   2. A UUID IS WRITTEN AS A DECIMAL STRING. A component payload travels as `rfl::Generic`, whose
//      integer arm is `int64_t`; roughly half of `Common::UUID::Generate()`'s draws are above
//      INT64_MAX and would be stored as a NEGATIVE number in the `.desce`. The bits happen to survive
//      the round trip, so no test of ours would ever have caught it — and a file full of negative
//      entity ids is one hand-edit or one other reader away from being wrong. `Common::UUID` already
//      has a decimal-string spelling (its `std::string` constructor), so that is the form we store.

#include <Common/Core/Logger.hpp>
#include <Common/Core/UUID.hpp>

#include <Engine/ECS/Components.hpp>

#include <rflcpp/rfl/Generic.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace Desert::Core::Serialize
{
    // ── READING ONE VALUE ──────────────────────────────────────────────────────────────────────────
    //
    // Absent -> the target keeps whatever it already holds (rule 1 above), silently: a block written
    // by an older build legitimately lacks a key a newer build added, and warning about it would put
    // a line in the log for every entity in the scene. Present but of the WRONG TYPE is a different
    // thing entirely — nothing this project writes can produce it — so that one is reported by name.

    namespace AuthoredIO
    {
        inline void ReadFloat( const rfl::Generic::Object& from, const char* key, float& out )
        {
            const auto value = from.get( key );
            if ( !value.has_value() )
                return;
            // A JSON number that happens to be whole parses as the integer arm, so both are accepted:
            // `1` and `1.0` are the same authored value and only one of them is what a writer emits.
            if ( const auto asDouble = value.value().to_double(); asDouble.has_value() )
            {
                out = static_cast<float>( asDouble.value() );
                return;
            }
            if ( const auto asInt = value.value().to_int64(); asInt.has_value() )
            {
                out = static_cast<float>( asInt.value() );
                return;
            }
            LOG_WARN( "[Scene] '{0}' is not a number; the field kept its current value.", key );
        }

        inline void ReadBool( const rfl::Generic::Object& from, const char* key, bool& out )
        {
            const auto value = from.get( key );
            if ( !value.has_value() )
                return;
            const auto flag = value.value().to_bool();
            if ( !flag.has_value() )
            {
                LOG_WARN( "[Scene] '{0}' is not a boolean; the field kept its current value.", key );
                return;
            }
            out = flag.value();
        }

        // An integer field of type T. Whole JSON numbers only: `2.5` for a tile coordinate is not a value
        // this project writes, and truncating it would put the tile somewhere nobody asked. A number that
        // does not fit T is refused the same way — narrowing 2^32 into a uint32 is 0, a real place.
        template <class T>
        inline void ReadInteger( const rfl::Generic::Object& from, const char* key, T& out )
        {
            const auto value = from.get( key );
            if ( !value.has_value() )
                return;
            const auto whole = value.value().to_int64();
            if ( !whole.has_value() )
            {
                LOG_WARN( "[Scene] '{0}' is not a whole number; the field kept its current value.", key );
                return;
            }
            if ( whole.value() < static_cast<int64_t>( std::numeric_limits<T>::min() ) ||
                 whole.value() > static_cast<int64_t>( std::numeric_limits<T>::max() ) )
            {
                LOG_WARN( "[Scene] '{0}' = {1} does not fit the field; it kept its current value.", key,
                          whole.value() );
                return;
            }
            out = static_cast<T>( whole.value() );
        }

        inline void ReadString( const rfl::Generic::Object& from, const char* key, std::string& out )
        {
            const auto value = from.get( key );
            if ( !value.has_value() )
                return;
            const auto text = value.value().to_string();
            if ( !text.has_value() )
            {
                LOG_WARN( "[Scene] '{0}' is not a string; the field kept its current value.", key );
                return;
            }
            out = text.value();
        }

        inline void ReadVec3( const rfl::Generic::Object& from, const char* key, glm::vec3& out )
        {
            const auto value = from.get( key );
            if ( !value.has_value() )
                return;
            const auto array = value.value().to_array();
            if ( !array.has_value() || array.value().size() != 3 )
            {
                LOG_WARN( "[Scene] '{0}' is not a 3-element array; the field kept its current value.", key );
                return;
            }
            const rfl::Generic::Array elements = array.value();
            glm::vec3                 read     = out;
            for ( std::size_t i = 0; i < 3; ++i )
            {
                if ( const auto asDouble = elements[i].to_double(); asDouble.has_value() )
                    read[static_cast<glm::length_t>( i )] = static_cast<float>( asDouble.value() );
                else if ( const auto asInt = elements[i].to_int64(); asInt.has_value() )
                    read[static_cast<glm::length_t>( i )] = static_cast<float>( asInt.value() );
                else
                {
                    LOG_WARN( "[Scene] '{0}' holds a non-number; the field kept its current value.", key );
                    return;
                }
            }
            out = read;
        }

        inline rfl::Generic WriteVec3( const glm::vec3& value )
        {
            return rfl::Generic( rfl::Generic::Array{ rfl::Generic( static_cast<double>( value.x ) ),
                                                      rfl::Generic( static_cast<double>( value.y ) ),
                                                      rfl::Generic( static_cast<double>( value.z ) ) } );
        }

        // Rule 2: the decimal string, not the number. `std::stoull` is what Common::UUID's own string
        // constructor uses, and it is exact over the whole 64-bit range.
        inline rfl::Generic WriteUUID( const Common::UUID& value )
        {
            return rfl::Generic( std::to_string( static_cast<uint64_t>( value ) ) );
        }

        inline void ReadUUID( const rfl::Generic::Object& from, const char* key, Common::UUID& out )
        {
            const auto value = from.get( key );
            if ( !value.has_value() )
                return;
            const auto text = value.value().to_string();
            if ( !text.has_value() )
            {
                LOG_WARN( "[Scene] '{0}' is not a decimal id string; the reference kept its current "
                          "value.",
                          key );
                return;
            }
            try
            {
                out = Common::UUID( std::stoull( text.value() ) );
            }
            catch ( const std::exception& )
            {
                // Refusing loudly and keeping the old value beats silently detaching the socket: a
                // weapon that stays in the hand is a visible wrong, a weapon on the floor is a bug
                // report about animation.
                LOG_WARN( "[Scene] '{0}' = '{1}' is not a 64-bit decimal id; the reference kept its "
                          "current value.",
                          key, text.value() );
            }
        }

        inline rfl::Generic WriteFloats( const std::vector<float>& values )
        {
            rfl::Generic::Array array;
            array.reserve( values.size() );
            for ( float v : values )
                array.push_back( rfl::Generic( static_cast<double>( v ) ) );
            return rfl::Generic( array );
        }

        inline void ReadFloats( const rfl::Generic::Object& from, const char* key, std::vector<float>& out )
        {
            const auto value = from.get( key );
            if ( !value.has_value() )
                return;
            const auto array = value.value().to_array();
            if ( !array.has_value() )
            {
                LOG_WARN( "[Scene] '{0}' is not an array; the list kept its current contents.", key );
                return;
            }
            const rfl::Generic::Array elements = array.value();
            std::vector<float>        read;
            read.reserve( elements.size() );
            for ( const auto& element : elements )
            {
                if ( const auto asDouble = element.to_double(); asDouble.has_value() )
                    read.push_back( static_cast<float>( asDouble.value() ) );
                else if ( const auto asInt = element.to_int64(); asInt.has_value() )
                    read.push_back( static_cast<float>( asInt.value() ) );
                else
                {
                    LOG_WARN( "[Scene] '{0}' holds a non-number; the list kept its current contents.", key );
                    return;
                }
            }
            out = std::move( read );
        }

        inline rfl::Generic WriteStrings( const std::vector<std::string>& values )
        {
            rfl::Generic::Array array;
            array.reserve( values.size() );
            for ( const auto& v : values )
                array.push_back( rfl::Generic( v ) );
            return rfl::Generic( array );
        }

        inline void ReadStrings( const rfl::Generic::Object& from, const char* key, std::vector<std::string>& out )
        {
            const auto value = from.get( key );
            if ( !value.has_value() )
                return;
            const auto array = value.value().to_array();
            if ( !array.has_value() )
            {
                LOG_WARN( "[Scene] '{0}' is not an array; the list kept its current contents.", key );
                return;
            }
            const rfl::Generic::Array elements = array.value();
            std::vector<std::string>  read;
            read.reserve( elements.size() );
            for ( const auto& element : elements )
            {
                const auto text = element.to_string();
                if ( !text.has_value() )
                {
                    LOG_WARN( "[Scene] '{0}' holds a non-string; the list kept its current contents.", key );
                    return;
                }
                read.push_back( text.value() );
            }
            out = std::move( read );
        }
    } // namespace AuthoredIO

    // ── FOLIAGE TYPE ───────────────────────────────────────────────────────────────────────────────
    // The scatter parameters the paint brush reads (Editor/.../Tools/FoliagePaintTool.cpp). Losing
    // these is the most visible of the five: the next paint dab after a reload scatters at a density
    // and a scale nobody asked for, and the instances already on the terrain do not match it.
    inline rfl::Generic::Object WriteComponent( const ECS::FoliageComponent& c )
    {
        rfl::Generic::Object o;
        o["Density"]       = rfl::Generic( static_cast<double>( c.Density ) );
        o["ScaleMin"]      = rfl::Generic( static_cast<double>( c.ScaleMin ) );
        o["ScaleMax"]      = rfl::Generic( static_cast<double>( c.ScaleMax ) );
        o["ZOffsetMin"]    = rfl::Generic( static_cast<double>( c.ZOffsetMin ) );
        o["ZOffsetMax"]    = rfl::Generic( static_cast<double>( c.ZOffsetMax ) );
        o["MaxPitchDeg"]   = rfl::Generic( static_cast<double>( c.MaxPitchDeg ) );
        o["SlopeMinDeg"]   = rfl::Generic( static_cast<double>( c.SlopeMinDeg ) );
        o["SlopeMaxDeg"]   = rfl::Generic( static_cast<double>( c.SlopeMaxDeg ) );
        o["AlignToNormal"] = rfl::Generic( c.AlignToNormal );
        o["RandomYaw"]     = rfl::Generic( c.RandomYaw );
        return o;
    }

    inline void ReadComponent( const rfl::Generic::Object& from, ECS::FoliageComponent& c )
    {
        AuthoredIO::ReadFloat( from, "Density", c.Density );
        AuthoredIO::ReadFloat( from, "ScaleMin", c.ScaleMin );
        AuthoredIO::ReadFloat( from, "ScaleMax", c.ScaleMax );
        AuthoredIO::ReadFloat( from, "ZOffsetMin", c.ZOffsetMin );
        AuthoredIO::ReadFloat( from, "ZOffsetMax", c.ZOffsetMax );
        AuthoredIO::ReadFloat( from, "MaxPitchDeg", c.MaxPitchDeg );
        AuthoredIO::ReadFloat( from, "SlopeMinDeg", c.SlopeMinDeg );
        AuthoredIO::ReadFloat( from, "SlopeMaxDeg", c.SlopeMaxDeg );
        AuthoredIO::ReadBool( from, "AlignToNormal", c.AlignToNormal );
        AuthoredIO::ReadBool( from, "RandomYaw", c.RandomYaw );
    }

    // ── LOCOMOTION ─────────────────────────────────────────────────────────────────────────────────
    // The state -> clip mapping LocomotionSystem reads. The clip NAMES are the whole point: a lost
    // name is a character that silently stops walking, because SystemRules falls back to the default
    // spellings ("Idle"/"Walk"/"Run") which the character's own library may not contain.
    inline rfl::Generic::Object WriteComponent( const ECS::LocomotionComponent& c )
    {
        rfl::Generic::Object o;
        o["IdleClip"]  = rfl::Generic( c.IdleClip );
        o["WalkClip"]  = rfl::Generic( c.WalkClip );
        o["RunClip"]   = rfl::Generic( c.RunClip );
        o["JumpClip"]  = rfl::Generic( c.JumpClip );
        o["WalkSpeed"] = rfl::Generic( static_cast<double>( c.WalkSpeed ) );
        o["RunSpeed"]  = rfl::Generic( static_cast<double>( c.RunSpeed ) );
        return o;
    }

    inline void ReadComponent( const rfl::Generic::Object& from, ECS::LocomotionComponent& c )
    {
        AuthoredIO::ReadString( from, "IdleClip", c.IdleClip );
        AuthoredIO::ReadString( from, "WalkClip", c.WalkClip );
        AuthoredIO::ReadString( from, "RunClip", c.RunClip );
        AuthoredIO::ReadString( from, "JumpClip", c.JumpClip );
        AuthoredIO::ReadFloat( from, "WalkSpeed", c.WalkSpeed );
        AuthoredIO::ReadFloat( from, "RunSpeed", c.RunSpeed );
    }

    // ── MORPH TARGETS ──────────────────────────────────────────────────────────────────────────────
    // `AppliedWeights` is NOT written and must not be: it is the last set the runtime blended into the
    // geometry, kept so a per-frame apply can skip work. Writing it would put a frame of cache into
    // every `.desce` and make a saved scene differ from itself depending on when it was saved.
    //
    // Weights and TargetNames are index-aligned by the component's own contract, and this is where
    // that stops being a comment: a file whose two lists disagree is repaired on read and reported,
    // because the Details sliders index one list with the other's size.
    inline rfl::Generic::Object WriteComponent( const ECS::MorphComponent& c )
    {
        rfl::Generic::Object o;
        o["Weights"]     = AuthoredIO::WriteFloats( c.Weights );
        o["TargetNames"] = AuthoredIO::WriteStrings( c.TargetNames );
        return o;
    }

    inline void ReadComponent( const rfl::Generic::Object& from, ECS::MorphComponent& c )
    {
        AuthoredIO::ReadFloats( from, "Weights", c.Weights );
        AuthoredIO::ReadStrings( from, "TargetNames", c.TargetNames );

        if ( c.TargetNames.size() != c.Weights.size() )
        {
            LOG_WARN( "[Scene] Morph block carries {0} weights against {1} target names; the names were "
                      "aligned to the weights.",
                      c.Weights.size(), c.TargetNames.size() );
            c.TargetNames.resize( c.Weights.size() );
        }
    }

    // ── SOCKET ATTACHMENT ──────────────────────────────────────────────────────────────────────────
    // `Target` is a reference to another ENTITY of the same scene, written as its UUID (rule 2 above).
    // It is deliberately NOT remapped on duplicate or on prefab instancing — no entity reference in
    // this engine is, ProjectileComponent::Owner included — so a copied weapon follows the same
    // character the original followed. That is right for a weapon and wrong for a prefab that
    // contains both ends; the general remap is a separate piece of work and is named in U13's report.
    inline rfl::Generic::Object WriteComponent( const ECS::SocketAttachmentComponent& c )
    {
        rfl::Generic::Object o;
        o["Target"]            = AuthoredIO::WriteUUID( c.Target );
        o["BoneName"]          = rfl::Generic( c.BoneName );
        o["OffsetTranslation"] = AuthoredIO::WriteVec3( c.OffsetTranslation );
        o["OffsetRotation"]    = AuthoredIO::WriteVec3( c.OffsetRotation );
        o["OffsetScale"]       = AuthoredIO::WriteVec3( c.OffsetScale );
        return o;
    }

    inline void ReadComponent( const rfl::Generic::Object& from, ECS::SocketAttachmentComponent& c )
    {
        AuthoredIO::ReadUUID( from, "Target", c.Target );
        AuthoredIO::ReadString( from, "BoneName", c.BoneName );
        AuthoredIO::ReadVec3( from, "OffsetTranslation", c.OffsetTranslation );
        AuthoredIO::ReadVec3( from, "OffsetRotation", c.OffsetRotation );
        AuthoredIO::ReadVec3( from, "OffsetScale", c.OffsetScale );
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
    inline rfl::Generic::Object WriteComponent( const ECS::ProjectileComponent& c )
    {
        rfl::Generic::Object o;
        o["Velocity"]      = AuthoredIO::WriteVec3( c.Velocity );
        o["GravityScale"]  = rfl::Generic( static_cast<double>( c.GravityScale ) );
        o["LifeRemaining"] = rfl::Generic( static_cast<double>( c.LifeRemaining ) );
        o["Damage"]        = rfl::Generic( static_cast<double>( c.Damage ) );
        o["Owner"]         = AuthoredIO::WriteUUID( c.Owner );
        return o;
    }

    inline void ReadComponent( const rfl::Generic::Object& from, ECS::ProjectileComponent& c )
    {
        AuthoredIO::ReadVec3( from, "Velocity", c.Velocity );
        AuthoredIO::ReadFloat( from, "GravityScale", c.GravityScale );
        AuthoredIO::ReadFloat( from, "LifeRemaining", c.LifeRemaining );
        AuthoredIO::ReadFloat( from, "Damage", c.Damage );
        AuthoredIO::ReadUUID( from, "Owner", c.Owner );
    }
    // ── LANDSCAPE ──────────────────────────────────────────────────────────────────────────────────
    // Hand-mapped for the reason the file header gives: the tile's `Landscape` is a Common::UUID, which is
    // not a reflectable field type, and the tile carries loaded heights that must NOT be written. The root
    // goes with it so the two halves of one feature are read in one place.
    inline rfl::Generic::Object WriteComponent( const ECS::LandscapeComponent& c )
    {
        rfl::Generic::Object o;
        o["QuadsPerTile"] = rfl::Generic( static_cast<int64_t>( c.QuadsPerTile ) );
        o["SpacingCm"]    = rfl::Generic( static_cast<double>( c.SpacingCm ) );
        o["ZScale"]       = rfl::Generic( static_cast<double>( c.ZScale ) );
        return o;
    }

    inline void ReadComponent( const rfl::Generic::Object& from, ECS::LandscapeComponent& c )
    {
        AuthoredIO::ReadInteger( from, "QuadsPerTile", c.QuadsPerTile );
        AuthoredIO::ReadFloat( from, "SpacingCm", c.SpacingCm );
        AuthoredIO::ReadFloat( from, "ZScale", c.ZScale );
    }

    // `Heights` is not written: it is what `HeightFile` decodes to, and the registry's LandscapeTile
    // serializer is what loads it (ComponentRegistry.cpp). Writing both would be two copies of one terrain.
    inline rfl::Generic::Object WriteComponent( const ECS::LandscapeTileComponent& c )
    {
        rfl::Generic::Object o;
        o["Landscape"]  = AuthoredIO::WriteUUID( c.Landscape );
        o["TileX"]      = rfl::Generic( static_cast<int64_t>( c.TileX ) );
        o["TileZ"]      = rfl::Generic( static_cast<int64_t>( c.TileZ ) );
        o["HeightFile"] = rfl::Generic( c.HeightFile );
        return o;
    }

    inline void ReadComponent( const rfl::Generic::Object& from, ECS::LandscapeTileComponent& c )
    {
        AuthoredIO::ReadUUID( from, "Landscape", c.Landscape );
        AuthoredIO::ReadInteger( from, "TileX", c.TileX );
        AuthoredIO::ReadInteger( from, "TileZ", c.TileZ );
        AuthoredIO::ReadString( from, "HeightFile", c.HeightFile );
    }
} // namespace Desert::Core::Serialize
