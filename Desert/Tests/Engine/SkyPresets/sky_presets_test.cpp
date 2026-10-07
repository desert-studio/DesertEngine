// The sky palettes, checked as the pure functions they are.
//
// ActivePreset is a name the scene file carries forever. The one failure worth building a mechanism
// against is a name that keeps claiming "Clear Noon" after the artist dragged the zenith to purple - so
// the tests below are mostly about which edits are allowed to change that name and which are not.
//
// The requirement specified the discriminator as a 64-bit hash of the palette. This uses a lossless copy
// of the thirteen fields instead (Graphic::ExtractSkyPresetValues), because two palettes sharing a hash
// produce exactly the lie the mechanism exists to prevent, and comparing 52 bytes with == is cheaper than
// hashing them anyway. The assertions the requirement asked of the hash are made of the copy verbatim:
// it changes for each of the thirteen palette fields, and for none of the ten authored/quality ones.

#include <Engine/ECS/SkyAtmosphereComponent.hpp>
#include <Engine/Graphic/SkyPresets.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Reflection/ReflectionTypes.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

using Desert::ECS::SkyAtmosphereData;
using Desert::ECS::SkyEnvironmentResolution;
using Desert::ECS::SkyPreset;
using Desert::Reflection::FieldInfo;
using Desert::Reflection::FieldType;
using Desert::Reflection::ReflectionRegistry;
using Desert::Reflection::TypeInfo;

namespace Graphic = Desert::Graphic;

namespace
{
    // The thirteen fields a preset owns (SkyAtmosphereData rows 2-14).
    constexpr const char* kPaletteFields[] = {
         "SkyBrightness", "HorizonFalloff",  "ZenithColor",   "HorizonColor",       "GroundColor",
         "NightColor",    "SunIntensity",    "SunColor",      "SunAngularDiameter", "SunGlow",
         "SunsetColor",   "SunsetIntensity", "StarIntensity",
    };

    const TypeInfo& SkyType()
    {
        const TypeInfo* t = ReflectionRegistry::Get().Find( "SkyAtmosphereData" );
        EXPECT_NE( t, nullptr ) << "SkyAtmosphereData is not registered - the codegen did not run";
        return *t;
    }

    const FieldInfo* Field( const char* name )
    {
        const TypeInfo& type = SkyType();
        const auto      it   = std::find_if( type.Fields.begin(), type.Fields.end(),
                                             [name]( const FieldInfo& f ) { return f.Name == name; } );
        return it == type.Fields.end() ? nullptr : &*it;
    }

    bool IsPaletteField( const std::string& name )
    {
        return std::find_if( std::begin( kPaletteFields ), std::end( kPaletteFields ),
                             [&name]( const char* p ) { return name == p; } ) != std::end( kPaletteFields );
    }

    // Everything a preset must leave alone: the time-of-day block, the environment-bake knobs, the planet
    // radius, the master switch and the preset label itself.
    struct AuthoredState
    {
        bool                     Enabled;
        bool                     DriveSunFromTimeOfDay;
        float                    TimeOfDay;
        float                    DayLengthSeconds;
        float                    Latitude;
        float                    NorthOffset;
        bool                     AutoRebakeEnvironment;
        float                    RebakeSunAngleThreshold;
        SkyEnvironmentResolution EnvironmentResolution;
        SkyPreset                ActivePreset;
        float                    PlanetRadius;

        bool operator==( const AuthoredState& ) const = default;
    };

    AuthoredState CaptureAuthored( const SkyAtmosphereData& d )
    {
        return AuthoredState{ d.Enabled,
                              d.DriveSunFromTimeOfDay,
                              d.TimeOfDay,
                              d.DayLengthSeconds,
                              d.Latitude,
                              d.NorthOffset,
                              d.AutoRebakeEnvironment,
                              d.RebakeSunAngleThreshold,
                              d.EnvironmentResolution,
                              d.ActivePreset,
                              d.PlanetRadius };
    }

    // A value the field has not got, so "did this edit register?" never depends on luck.
    void PerturbField( SkyAtmosphereData& d, const FieldInfo& f )
    {
        auto* bytes = reinterpret_cast<std::byte*>( &d ) + f.Offset;
        switch ( f.Type )
        {
            case FieldType::Bool:
            {
                bool v = false;
                std::memcpy( &v, bytes, sizeof( v ) );
                v = !v;
                std::memcpy( bytes, &v, sizeof( v ) );
                break;
            }
            // RELATIVE, not "+ 0.125": these fields span 0 to 86400, and an absolute nudge can fall below
            // the float's resolution at the top of that spread and silently do nothing, which would read
            // here as "the field is not part of the palette".
            case FieldType::Float:
            case FieldType::Vec3: // moving the first component is enough to move the colour
            {
                float v = 0.0f;
                std::memcpy( &v, bytes, sizeof( v ) );
                v = v * 1.5f + 1.0f;
                std::memcpy( bytes, &v, sizeof( v ) );
                break;
            }
            case FieldType::Enum:
            {
                // Both sky enums are uint8_t; step to the next enumerator, wrapping at the end.
                uint8_t v = 0;
                std::memcpy( &v, bytes, sizeof( v ) );
                v = static_cast<uint8_t>( ( v + 1 ) % f.EnumValues.size() );
                std::memcpy( bytes, &v, sizeof( v ) );
                break;
            }
            default:
                FAIL() << f.Name << " has a type this test does not know how to perturb";
        }
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// The table itself (SKY-12)
// ---------------------------------------------------------------------------------------------------

TEST( SkyPresets, EveryEnumeratorExceptCustomHasExactlyOneRow )
{
    const FieldInfo* preset = Field( "ActivePreset" );
    ASSERT_NE( preset, nullptr );
    ASSERT_EQ( preset->Type, FieldType::Enum );

    EXPECT_EQ( std::size( Graphic::kSkyPresets ), preset->EnumValues.size() - 1u )
         << "kSkyPresets must have one row per SkyPreset enumerator except Custom";

    for ( const auto& enumerator : preset->EnumValues )
    {
        const auto id     = static_cast<SkyPreset>( enumerator.Value );
        const bool custom = id == SkyPreset::Custom;
        EXPECT_EQ( Graphic::FindSkyPreset( id ) == nullptr, custom ) << enumerator.Name;
    }
}

// Two presets that look the same are one preset with two names in the menu.
TEST( SkyPresets, EveryPresetHasItsOwnZenithColour )
{
    for ( const Graphic::SkyPresetEntry& a : Graphic::kSkyPresets )
    {
        for ( const Graphic::SkyPresetEntry& b : Graphic::kSkyPresets )
        {
            if ( a.Id == b.Id )
                continue;
            EXPECT_FALSE( a.Values.ZenithColor == b.Values.ZenithColor )
                 << a.Name << " and " << b.Name << " share a zenith colour";
        }
    }
}

TEST( SkyPresets, ComponentDefaultsAreTheClearNoonRow )
{
    SkyAtmosphereData applied{};
    Graphic::ApplySkyPreset( SkyPreset::ClearNoon, applied );

    const SkyAtmosphereData fresh{};
    EXPECT_TRUE( Graphic::ExtractSkyPresetValues( applied ) == Graphic::ExtractSkyPresetValues( fresh ) );
    EXPECT_EQ( Graphic::MatchSkyPreset( fresh ), SkyPreset::ClearNoon );
}

TEST( SkyPresets, ApplyThenMatchRoundTripsAndIsIdempotent )
{
    for ( const Graphic::SkyPresetEntry& entry : Graphic::kSkyPresets )
    {
        SkyAtmosphereData d{};
        Graphic::ApplySkyPreset( entry.Id, d );
        EXPECT_EQ( Graphic::MatchSkyPreset( d ), entry.Id ) << entry.Name;

        const Graphic::SkyPresetValues once = Graphic::ExtractSkyPresetValues( d );
        Graphic::ApplySkyPreset( entry.Id, d );
        EXPECT_TRUE( Graphic::ExtractSkyPresetValues( d ) == once ) << entry.Name;
    }
}

TEST( SkyPresets, ApplyingCustomLeavesEveryValueAlone )
{
    SkyAtmosphereData d{};
    Graphic::ApplySkyPreset( SkyPreset::Night, d );
    const Graphic::SkyPresetValues before = Graphic::ExtractSkyPresetValues( d );

    Graphic::ApplySkyPreset( SkyPreset::Custom, d );
    EXPECT_TRUE( Graphic::ExtractSkyPresetValues( d ) == before );
}

// ---------------------------------------------------------------------------------------------------
// A preset is a palette and nothing else (SKY-07)
// ---------------------------------------------------------------------------------------------------

// Including ActivePreset: Apply does not stamp its own name into the data. That is what makes this
// assertion possible at all - a function that recorded itself could never be checked against "the
// authored fields are unchanged".
TEST( SkyPresets, ApplyingAPresetTouchesNothingOutsideThePalette )
{
    for ( const Graphic::SkyPresetEntry& entry : Graphic::kSkyPresets )
    {
        SkyAtmosphereData d{};
        d.Enabled                 = false;
        d.DriveSunFromTimeOfDay   = true;
        d.TimeOfDay               = 5.25f;
        d.DayLengthSeconds        = 1234.0f;
        d.Latitude                = -12.5f;
        d.NorthOffset             = 200.0f;
        d.AutoRebakeEnvironment   = false;
        d.RebakeSunAngleThreshold = 17.0f;
        d.EnvironmentResolution   = SkyEnvironmentResolution::High;
        d.ActivePreset            = SkyPreset::OvercastGrey;
        d.PlanetRadius            = 1234.0f;

        const AuthoredState before = CaptureAuthored( d );
        Graphic::ApplySkyPreset( entry.Id, d );
        EXPECT_TRUE( CaptureAuthored( d ) == before ) << entry.Name << " wrote outside the palette";
    }
}

// ---------------------------------------------------------------------------------------------------
// Which edits cost the preset name (SKY-37)
// ---------------------------------------------------------------------------------------------------

TEST( SkyPresets, TheExtractedPaletteIsStableAcrossANoOpRoundTrip )
{
    SkyAtmosphereData d{};
    Graphic::ApplySkyPreset( SkyPreset::GoldenHour, d );

    const Graphic::SkyPresetValues a = Graphic::ExtractSkyPresetValues( d );
    const Graphic::SkyPresetValues b = Graphic::ExtractSkyPresetValues( d );
    EXPECT_TRUE( a == b );

    SkyAtmosphereData copy = d;
    EXPECT_TRUE( Graphic::ExtractSkyPresetValues( copy ) == a );
}

// The loop runs over the reflection metadata rather than a hand-written list, so a field added to the
// palette set without being picked up by the extractor fails here instead of becoming a value the preset
// name silently stops describing.
TEST( SkyPresets, EveryPaletteFieldChangesTheDiscriminatorAndNoOtherFieldDoes )
{
    std::size_t palette  = 0;
    std::size_t authored = 0;

    for ( const FieldInfo& f : SkyType().Fields )
    {
        SkyAtmosphereData d{};
        Graphic::ApplySkyPreset( SkyPreset::ClearNoon, d );
        const Graphic::SkyPresetValues before = Graphic::ExtractSkyPresetValues( d );

        PerturbField( d, f );
        const bool moved = !( Graphic::ExtractSkyPresetValues( d ) == before );

        if ( IsPaletteField( f.Name ) )
        {
            ++palette;
            EXPECT_TRUE( moved ) << f.Name << " is a palette field the discriminator does not see";
            EXPECT_EQ( Graphic::MatchSkyPreset( d ), SkyPreset::Custom ) << f.Name;
        }
        else
        {
            ++authored;
            EXPECT_FALSE( moved ) << f.Name << " is not a palette field but moved the discriminator";
            EXPECT_EQ( Graphic::MatchSkyPreset( d ), SkyPreset::ClearNoon )
                 << "editing " << f.Name << " cost the preset its name";
        }
    }

    // 13 palette + 34 authored/quality (the master switch, the five time-of-day rows, the three
    // environment-bake rows, the preset label, the planet radius, and the 23 physical-atmosphere
    // fields — a preset is a palette of the GRADIENT model and must leave the physical medium, and the
    // aerial perspective it feeds, alone) = the component's 47 fields.
    EXPECT_EQ( palette, 13u );
    EXPECT_EQ( authored, 34u ) << "the component gained or lost a field without this test being revisited";
}

// Dialling the values back by hand restores the name rather than leaving "Custom" behind. This is why the
// editor derives the name from the values instead of clearing it on the first keystroke.
TEST( SkyPresets, RestoringThePaletteByHandRestoresThePresetName )
{
    SkyAtmosphereData d{};
    Graphic::ApplySkyPreset( SkyPreset::Night, d );

    const glm::vec3 night = d.ZenithColor;
    d.ZenithColor         = { 0.9f, 0.1f, 0.4f };
    EXPECT_EQ( Graphic::MatchSkyPreset( d ), SkyPreset::Custom );

    d.ZenithColor = night;
    EXPECT_EQ( Graphic::MatchSkyPreset( d ), SkyPreset::Night );
}

// ---------------------------------------------------------------------------------------------------
// Every authored number lands inside the slider the artist will see
// ---------------------------------------------------------------------------------------------------

TEST( SkyPresets, EveryPresetValueLiesInsideItsFieldsDeclaredRangeOrColourGamut )
{
    for ( const Graphic::SkyPresetEntry& entry : Graphic::kSkyPresets )
    {
        SkyAtmosphereData d{};
        Graphic::ApplySkyPreset( entry.Id, d );

        for ( const FieldInfo& f : SkyType().Fields )
        {
            if ( !IsPaletteField( f.Name ) )
                continue;

            const auto* bytes = reinterpret_cast<const std::byte*>( &d ) + f.Offset;
            if ( f.Type == FieldType::Float )
            {
                ASSERT_TRUE( f.Meta.HasRange ) << f.Name << " is a numeric palette field with no Range";
                float v = 0.0f;
                std::memcpy( &v, bytes, sizeof( v ) );
                EXPECT_GE( v, f.Meta.RangeMin ) << entry.Name << '.' << f.Name;
                EXPECT_LE( v, f.Meta.RangeMax ) << entry.Name << '.' << f.Name;
            }
            else if ( f.Type == FieldType::Vec3 )
            {
                // Colours carry PROPERTY(Color) rather than a Range; the swatch is 0..1 per channel.
                ASSERT_TRUE( f.Meta.IsColor ) << f.Name;
                float v[3] = { 0.0f, 0.0f, 0.0f };
                std::memcpy( v, bytes, sizeof( v ) );
                for ( int i = 0; i < 3; ++i )
                {
                    EXPECT_GE( v[i], 0.0f ) << entry.Name << '.' << f.Name << '[' << i << ']';
                    EXPECT_LE( v[i], 1.0f ) << entry.Name << '.' << f.Name << '[' << i << ']';
                }
            }
        }
    }
}
