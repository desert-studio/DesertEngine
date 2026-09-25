#include <Engine/Assets/CloudTypeData.hpp>
#include <Engine/Assets/TextAssetHeaderCheck.hpp>

#include <algorithm>
#include <vector>
#include <cmath>

#include <rflcpp/rfl/json.hpp>

namespace Desert::Assets
{
    namespace
    {
        // Every number that reaches the profile generator has to be a number. A NaN multiplied into the
        // table produces a shell whose bounds are NaN, a march whose step count is NaN and a sky that is
        // black with nothing in the log — and it takes a day to find out which of twelve floats it came
        // from. Naming the field here is the whole difference.
        Common::BoolResultStr Finite( const char* field, float value )
        {
            if ( !std::isfinite( value ) )
                return Common::MakeFormattedError<bool>( "{} is {}, which is not a finite number", field, value );
            return BOOLSUCCESS;
        }

        Common::BoolResultStr InRange( const char* field, float value, float low, float high )
        {
            if ( auto finite = Finite( field, value ); !finite )
                return finite;
            if ( value < low || value > high )
                return Common::MakeFormattedError<bool>( "{} is {}, outside the legal range [{}, {}]", field,
                                                         value, low, high );
            return BOOLSUCCESS;
        }
    } // namespace

    const Graphic::CloudTypeShape& CloudTypeDefaultShape()
    {
        // T0'S CUMULUS CONGESTUS, DIGIT FOR DIGIT. Base and top from meteorology, an edge fraction of 0.15
        // because a congestus is a flat pad at the rim of a patch and a tower in its middle, and a density
        // slightly above a cumulus because it is made of more water. The three factors that T0 did not have
        // are 1 here by construction: this row IS the reference the artist's Density Scale, Extinction
        // Scale and Detail Strength are relative to, so anything else would move the sky of every existing
        // scene while claiming to be its default.
        static const Graphic::CloudTypeShape kDefault{
             /* BaseAltitudeKm   */ 2.20f,
             /* TopAltitudeKm    */ 5.80f,
             /* EdgeTopFraction  */ 0.15f,
             /* BaseRampFraction */ 0.04f,
             // WHERE `TopTaper` 0.50 WENT. The curve is that law sampled, so this row is still T0's
             // congestus digit for digit — what changed is that the digits are now in the asset instead of
             // in a closed form in the generator.
             /* Profile          */ Graphic::CloudProfileDefault(),
             /* AnvilAltitudeKm  */ 0.0f,
             /* AnvilThicknessKm */ 0.0f,
             /* AnvilStrength    */ 0.0f,
             /* DetailCharacter  */ 1.00f,
             /* DetailFactor     */ 1.00f,
             /* DensityFactor    */ 1.15f,
             /* ExtinctionFactor */ 1.00f,
             // T3'S TWO, AND BOTH ARE THE IDENTITY HERE, for exactly the reason the three factors above
             // are 1: this row is the reference every other type is stated against, and a placement scale
             // of anything but 1 would move the sky of every scene that has ever used the default while
             // claiming to be the default. A congestus field at the layer's own tile with round patches is
             // what T1 shipped, and this is that, digit for digit.
             /* PlacementScale      */ 1.00f,
             /* PlacementAnisotropy */ 1.00f,
        };
        return kDefault;
    }

    CloudTypeData CloudTypeDefault()
    {
        CloudTypeData data;
        data.DisplayName   = "Cumulus congestus (built-in)";
        data.Notes         = "The type an empty slot resolves to. It is not a file: a scene that names no "
                             "type still has to have a sky.";
        data.Shape         = CloudTypeDefaultShape();
        return data;
    }

    Common::BoolResultStr ValidateCloudTypeShape( const Graphic::CloudTypeShape& shape )
    {
        // THE ALTITUDES FIRST, because everything else is a fraction of the span they define. A span that
        // is zero or negative is a division the generator guards against and a shell the packer floors at a
        // metre — both of which produce a cloud nobody can see rather than a crash, which is precisely the
        // kind of failure that has to be refused at the door instead of survived.
        if ( auto r = InRange( "BaseAltitudeKm", shape.BaseAltitudeKm, 0.0f, 20.0f ); !r )
            return r;
        if ( auto r = InRange( "TopAltitudeKm", shape.TopAltitudeKm, 0.0f, 20.0f ); !r )
            return r;
        if ( !( shape.TopAltitudeKm > shape.BaseAltitudeKm ) )
            return Common::MakeFormattedError<bool>(
                 "TopAltitudeKm is {} and BaseAltitudeKm is {}: a type has to have a top above its base",
                 shape.TopAltitudeKm, shape.BaseAltitudeKm );

        if ( auto r = InRange( "EdgeTopFraction", shape.EdgeTopFraction, 0.0f, 1.0f ); !r )
            return r;

        // Strictly above zero rather than merely finite: the generator divides by each of them, and clamps
        // the divisor at a thousandth. A zero authored here would be silently answered by that clamp, which
        // is a value nobody chose standing in for one somebody typed.
        if ( auto r = InRange( "BaseRampFraction", shape.BaseRampFraction, 0.001f, 1.0f ); !r )
            return r;

        // THE PROFILE, SAMPLE BY SAMPLE AND NAMED BY INDEX. A NaN or a negative width anywhere in the row
        // is a lump radius the layout will clamp or a body turned inside out, and a message that said only
        // "the profile is invalid" would leave sixteen numbers to search by hand.
        for ( uint32_t i = 0; i < Graphic::kCloudProfileSamples; ++i )
        {
            const float halfWidth = shape.Profile.HalfWidth[i];
            if ( !Graphic::CloudProfileSampleIsLegal( halfWidth ) )
                return Common::MakeFormattedError<bool>(
                     "Profile.HalfWidth[{}] is {}, which is not a finite width in [0, 4] cluster radii", i,
                     halfWidth );
        }

        // A CURVE OF ALL ZEROES IS NOT A TYPE, and it is refused here rather than drawn. Every lump would
        // be floored at the march's resolvable chord, so the sky would come out as a field of identical
        // minimum-sized specks — a shape nobody authored, produced by a clamp. The threshold is a
        // hundredth of a cluster radius, which is the same figure the footprint's floor uses.
        float widest = 0.0f;
        for ( uint32_t i = 0; i < Graphic::kCloudProfileSamples; ++i )
            widest = std::max( widest, shape.Profile.HalfWidth[i] );

        if ( !( widest > 0.01f ) )
            return Common::MakeFormattedError<bool>(
                 "the vertical profile is {} at its widest: a type has to be wider than a hundredth of its "
                 "own cluster radius somewhere, or it draws nothing but specks",
                 widest );

        if ( auto r = InRange( "AnvilStrength", shape.AnvilStrength, 0.0f, 1.0f ); !r )
            return r;
        if ( auto r = InRange( "AnvilAltitudeKm", shape.AnvilAltitudeKm, 0.0f, 20.0f ); !r )
            return r;
        if ( auto r = InRange( "AnvilThicknessKm", shape.AnvilThicknessKm, 0.0f, 10.0f ); !r )
            return r;
        // A LOBE WITH NO THICKNESS IS NOT A LOBE, AND NEITHER IS ONE UNDER THE GENERATOR'S OWN THRESHOLDS.
        // Graphic::CloudTypeHasAnvil is what decides whether a canopy is drawn AND how tall the layer's
        // shell has to be, so a file that sits between "above zero" and "above that predicate" declares a
        // canopy the artist can see in the numbers and never in the sky. Refused here BY NAME rather than
        // answered with silence, because the shell is the thing that pays for it: an anvil declared at
        // 16 km takes a 0.40 km stratus layer to 15.85 km, and the vertical voxel from 12.5 m to 495 m.
        if ( shape.AnvilStrength > 0.0f && !Graphic::CloudTypeHasAnvil( shape ) )
            return Common::MakeFormattedError<bool>(
                 "AnvilStrength is {} and AnvilThicknessKm is {}: an anvil is drawn only above {} and {} km, "
                 "so this one never appears",
                 shape.AnvilStrength, shape.AnvilThicknessKm, Graphic::kCloudAnvilMinStrength,
                 Graphic::kCloudAnvilMinThicknessKm );

        if ( auto r = InRange( "DetailCharacter", shape.DetailCharacter, 0.0f, 1.0f ); !r )
            return r;
        // The three factors multiply the artist's own scales, so their ceiling is generous — a cirrus at a
        // twentieth of a cumulus' extinction and a cumulonimbus at four times it are both real weather.
        if ( auto r = InRange( "DetailFactor", shape.DetailFactor, 0.0f, 8.0f ); !r )
            return r;
        if ( auto r = InRange( "DensityFactor", shape.DensityFactor, 0.0f, 8.0f ); !r )
            return r;
        if ( auto r = InRange( "ExtinctionFactor", shape.ExtinctionFactor, 0.0f, 8.0f ); !r )
            return r;

        // THE PLACEMENT PAIR. Strictly above zero on the scale, because the packer divides by it to build
        // the field's frequency and a zero there is an infinity in a texture coordinate — a sky that is
        // black or white in bands, with nothing in the log. The ceiling of 8 is a placement period eight
        // times the layer's tile, which at the shipped 12 km is a 96 km cell: one cloud from horizon to
        // horizon, which is a legitimate overcast and the largest thing worth authoring.
        if ( auto r = InRange( "PlacementScale", shape.PlacementScale, 0.05f, 8.0f ); !r )
            return r;
        // BELOW ONE IS LEGAL AND MEANS SOMETHING. Above 1 the patches are drawn out ALONG the wind, which
        // is fibrous cirrus and cloud streets; below 1 they are drawn out ACROSS it, which is what a wave
        // cloud is — a lenticular stands in a mountain's lee wave with its crest perpendicular to the flow,
        // and so do the bars of an altocumulus undulatus. One number covers both because they are the same
        // stretch about the same axis, and forbidding one half of it would have made the lenticular
        // unreachable.
        if ( auto r = InRange( "PlacementAnisotropy", shape.PlacementAnisotropy, 0.1f, 16.0f ); !r )
            return r;

        return BOOLSUCCESS;
    }

    Common::ResultStr<CloudTypeData> ParseCloudType( const std::string& text )
    {
        if ( text.empty() )
            return Common::MakeFormattedError<CloudTypeData>( "the file is empty" );

        // A version-3 file (no header) is refused by name: see RefuseTextWithoutHeader.
        if ( auto headed = RefuseTextWithoutHeader( text, kCloudTypeFormatVersion, std::nullopt ); !headed )
            return Common::MakeFormattedError<CloudTypeData>( "{}", headed.GetError() );

        // THE HEADER ALONE FIRST: a version-4 file names its volume by a bare path, a shape the typed read
        // below cannot take, so its refusal would be a JSON type error that names no version.
        {
            struct HeaderOnly
            {
                std::optional<Common::Content::TextAssetHeaderSerialized> Header;
            };
            const auto headerOnly = rfl::json::read<HeaderOnly>( text );
            if ( !headerOnly )
                return Common::MakeFormattedError<CloudTypeData>( "{}", headerOnly.error().what() );
            if ( auto header =
                      CheckStatedHeader( headerOnly.value().Header, Common::Content::ContentKind::CloudType,
                                         kCloudTypeSchemaTag, kCloudTypeFormatVersion, CloudTypeTextSubsystems() );
                 !header )
                return Common::MakeFormattedError<CloudTypeData>( "{}; run Tools/SceneMigrator",
                                                                  header.GetError() );
        }

        const auto parsed = rfl::json::read<CloudTypeData>( text );
        if ( !parsed )
            return Common::MakeFormattedError<CloudTypeData>( "{}", parsed.error().what() );

        CloudTypeData data = parsed.value();
        // CheckStatedHeader above refused an absent header; the typed read is a second read of the text,
        // so its Header is checked again rather than assumed.
        if ( !data.Header )
            return Common::MakeFormattedError<CloudTypeData>( "the typed read states no Header" );

        // THE GUID RESOLVES, the path only names: a volume reference without one is a bare path again (v4).
        std::vector<std::string> dependencies;
        if ( data.NoiseVolume )
        {
            if ( const auto guid = Common::Content::AssetGuidFromText( data.NoiseVolume->Guid );
                 !guid || guid.GetValue().IsNull() )
                return Common::MakeFormattedError<CloudTypeData>(
                     "noise volume '{}' states no well-formed GUID ('{}'); run Tools/SceneMigrator",
                     data.NoiseVolume->Path, data.NoiseVolume->Guid );
            dependencies.push_back( data.NoiseVolume->Guid );
        }
        // ONE REFERENCE, TWO STATEMENTS OF IT: the header's Dependencies must be exactly the volume's GUID
        // (none for the built-in default), or the registry's edge and the resolver's volume disagree.
        if ( data.Header->Dependencies != dependencies )
            return Common::MakeFormattedError<CloudTypeData>(
                 "the header's Dependencies ({} entries) do not state exactly the noise volume's GUID ({})",
                 data.Header->Dependencies.size(), data.NoiseVolume ? data.NoiseVolume->Guid : "none" );

        if ( auto valid = ValidateCloudTypeShape( data.Shape ); !valid )
            return Common::MakeFormattedError<CloudTypeData>( "{}", valid.GetError() );

        return Common::MakeSuccess( std::move( data ) );
    }

    std::string WriteCloudType( const CloudTypeData& data )
    {
        CloudTypeData stamped = data;
        stamped.Header =
             StampTextHeader( data.Header, Common::Content::ContentKind::CloudType, CloudTypeTextSubsystems() );
        if ( data.NoiseVolume )
            stamped.Header->Dependencies = { data.NoiseVolume->Guid };
        return rfl::json::write( stamped, YYJSON_WRITE_PRETTY );
    }
} // namespace Desert::Assets
