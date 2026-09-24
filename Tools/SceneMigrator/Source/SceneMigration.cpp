#include "SceneMigration.hpp"

// The graph model and its JSON round trip, for the v20 -> v21 step: the blob it moves out of the entity
// IS this type serialized, so reading it with anything else would be a second statement of the format.
#include <Engine/Animation/Graph/AnimGraph.hpp>

#include <Engine/Core/SceneSettings.hpp>
#include <Engine/Geometry/EditMeshConversion.hpp>
#include <Engine/Geometry/EditMeshSerialization.hpp>
#include <Engine/Core/Serialize/AuthoredComponentIO.hpp>
#include <Engine/World/Landscape/LandscapeData.hpp>
#include <Engine/World/Landscape/LandscapeLayout.hpp>
#include <Engine/World/Landscape/LandscapeTileFiles.hpp>
// For the shipped presets' names and the directory they live in, and nothing else. The v4 -> v5 migration
// turns the species integer a v4 file carries into the PATH of the preset that holds the same twelve
// numbers, and spelling that path here as a literal would be a second statement of it — the exact
// duplication that agrees with itself until somebody moves the library. The header carries no renderer and
// no asset manager, so it does not bring another layer into this one.
#include <Engine/Assets/CloudTypeData.hpp>

#include <Engine/Assets/MaterialData.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/Units.hpp>

#include <glm/trigonometric.hpp>

// The cloud material step is the only migration that PRODUCES a file rather than only rewriting the
// property tree it was handed, so it is the only one that needs the serializer here. It still writes
// nothing itself: it returns the JSON text in the report and MigratorMain owns the atomic write, which
// is what keeps this function pure and testable (contract Section 4.4).
#include <rflcpp/rfl/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>

namespace Desert::Migration
{
    namespace
    {
        // How an old "Skybox" value has to be read, and what it becomes. Only four shapes exist because
        // SKY-05 kept the C++ member names byte-identical across the move: twelve of the fourteen mappings
        // below are the same name on both sides, which makes them copies that cannot be mistyped.
        enum class MappedKind
        {
            Bool,            // JSON true/false
            Scalar,          // JSON number -> float
            Color3,          // JSON array of exactly three finite numbers -> glm::vec3
            AngularDiameter, // JSON number in radians, and a RADIUS -> degrees, and a DIAMETER
        };

        struct SkyFieldMapping
        {
            const char* Old;
            const char* New;
            MappedKind  Kind;
        };

        // The mapping table of SKY-25, in the field order of ECS::SkyAtmosphereData so the payload this
        // function writes reads like the one a save would produce.
        //
        // What is deliberately NOT here: SkyboxHandle and Intensity stay under "Skybox" (they are the HDR
        // path, which did not move) - this
        // function neither reads nor removes them.
        //
        // std::to_array rather than a C array, like every table in this tool: see the rule at the top of
        // SceneMigration.hpp. Here it also keeps the row count out of the source - `std::array<T, 14>`
        // would ZERO-FILL a miscount rather than reject it, and a { nullptr, nullptr } mapping row is the
        // one shape the static_assert below cannot see.
        constexpr auto kSkyFieldMappings = std::to_array<SkyFieldMapping>( {
             { "Procedural", "Enabled", MappedKind::Bool },
             { "SkyBrightness", "SkyBrightness", MappedKind::Scalar },
             { "HorizonFalloff", "HorizonFalloff", MappedKind::Scalar },
             { "ZenithColor", "ZenithColor", MappedKind::Color3 },
             { "HorizonColor", "HorizonColor", MappedKind::Color3 },
             { "GroundColor", "GroundColor", MappedKind::Color3 },
             { "NightColor", "NightColor", MappedKind::Color3 },
             { "SunIntensity", "SunIntensity", MappedKind::Scalar },
             { "SunColor", "SunColor", MappedKind::Color3 },
             { "SunDiskRadius", "SunAngularDiameter", MappedKind::AngularDiameter },
             { "SunGlow", "SunGlow", MappedKind::Scalar },
             { "SunsetColor", "SunsetColor", MappedKind::Color3 },
             { "SunsetIntensity", "SunsetIntensity", MappedKind::Scalar },
             { "StarIntensity", "StarIntensity", MappedKind::Scalar },
        } );

        constexpr int kMappedFieldCount = static_cast<int>( std::size( kSkyFieldMappings ) );

        static_assert( kMappedFieldCount <= kSkyAtmosphereFieldCount,
                       "every mapped field must land on a field the new component actually has" );

        // A number, whatever JSON spelling it arrived in. Integers matter: a hand-edited scene writes
        // "SunIntensity":22, which reflect-cpp parses as int64 and to_double() then refuses.
        // Non-finite is not a number we accept anywhere - a NaN that reaches the sky poisons every pixel
        // it touches and leaves no trace of where it came from.
        std::optional<double> AsFiniteNumber( const rfl::Generic& g )
        {
            if ( const auto d = g.to_double(); d.has_value() )
            {
                if ( std::isfinite( d.value() ) )
                    return d.value();
                return std::nullopt;
            }
            if ( const auto i = g.to_int64(); i.has_value() )
                return static_cast<double>( i.value() );
            return std::nullopt;
        }

        // The offending value, spelled out. A warning that says "wrong type" without saying WHAT was in
        // the file sends the next reader back to the file anyway.
        std::string Describe( const rfl::Generic& g )
        {
            if ( const auto b = g.to_bool(); b.has_value() )
                return b.value() ? "true" : "false";
            if ( const auto i = g.to_int64(); i.has_value() )
                return std::to_string( i.value() );
            if ( const auto d = g.to_double(); d.has_value() )
                return std::to_string( d.value() );
            if ( const auto s = g.to_string(); s.has_value() )
                return "\"" + s.value() + "\"";
            if ( const auto a = g.to_array(); a.has_value() )
                return "an array of " + std::to_string( a.value().size() ) + " element(s)";
            if ( g.to_object().has_value() )
                return "an object";
            return "null";
        }

        void WarnRejected( const std::string& tag, const SkyFieldMapping& mapping, const rfl::Generic& value,
                           const char* expected )
        {
            LOG_WARN( "[SceneMigration] entity '{0}': Skybox.{1} is {2}, expected {3} - SkyAtmosphere.{4} "
                      "keeps its default",
                      tag, mapping.Old, Describe( value ), expected, mapping.New );
        }

        // Reads one old value and, if it is usable, writes the new one. Returns false when the value was
        // present but unusable, which is the ONLY case the caller counts as rejected: a value that is
        // simply absent is not an error, it is a scene that predates the field.
        bool CarryField( const std::string& tag, const SkyFieldMapping& mapping, const rfl::Generic& value,
                         rfl::Generic::Object& out )
        {
            switch ( mapping.Kind )
            {
                case MappedKind::Bool:
                {
                    const auto b = value.to_bool();
                    if ( !b.has_value() )
                    {
                        WarnRejected( tag, mapping, value, "a boolean" );
                        return false;
                    }
                    out[mapping.New] = b.value();
                    return true;
                }
                case MappedKind::Scalar:
                {
                    const auto n = AsFiniteNumber( value );
                    if ( !n.has_value() )
                    {
                        WarnRejected( tag, mapping, value, "a finite number" );
                        return false;
                    }
                    out[mapping.New] = n.value();
                    return true;
                }
                case MappedKind::Color3:
                {
                    const auto arr = value.to_array();
                    if ( !arr.has_value() || arr.value().size() != 3 )
                    {
                        WarnRejected( tag, mapping, value, "an array of 3 numbers" );
                        return false;
                    }

                    rfl::Generic::Array colour;
                    for ( const auto& component : arr.value() )
                    {
                        const auto n = AsFiniteNumber( component );
                        if ( !n.has_value() )
                        {
                            WarnRejected( tag, mapping, value, "an array of 3 finite numbers" );
                            return false;
                        }
                        colour.push_back( rfl::Generic( n.value() ) );
                    }
                    out[mapping.New] = std::move( colour );
                    return true;
                }
                case MappedKind::AngularDiameter:
                {
                    // The one conversion in the whole table: the old field was the angular RADIUS in
                    // RADIANS, the new one is the angular DIAMETER in DEGREES. A radius of zero or less is
                    // not a sun that ever rendered, so it is rejected rather than converted to a zero disk.
                    const auto n = AsFiniteNumber( value );
                    if ( !n.has_value() || n.value() <= 0.0 )
                    {
                        WarnRejected( tag, mapping, value, "a finite number greater than 0" );
                        return false;
                    }
                    out[mapping.New] = glm::degrees( n.value() ) * 2.0;
                    return true;
                }
            }
            return false;
        }
    } // namespace

    SkyMigrationReport MigrateSkyV0ToV1( std::vector<Assets::EntityData>& entities )
    {
        SkyMigrationReport report;

        for ( auto& entity : entities )
        {
            const auto skybox = entity.Components.get( "Skybox" );
            if ( !skybox.has_value() )
                continue; // no sky on this entity - nothing to move, and nothing to report

            // Idempotence, and the reason it matters: a scene may be re-read (undo, prefab instancing,
            // a second load) after it was already raised. Re-running the mapping would overwrite values
            // the user has since edited with the stale ones still sitting under "Skybox".
            if ( entity.Components.get( "SkyAtmosphere" ).has_value() )
                continue;

            const std::string tag = entity.Tag.value_or( "Entity" );

            const auto oldFields = skybox.value().to_object();
            if ( !oldFields.has_value() )
            {
                LOG_WARN( "[SceneMigration] entity '{0}': the Skybox payload is {1}, not an object - no "
                          "SkyAtmosphere component was created for it",
                          tag, Describe( skybox.value() ) );
                continue;
            }

            rfl::Generic::Object sky;
            int                  carried  = 0;
            int                  rejected = 0;

            for ( const auto& mapping : kSkyFieldMappings )
            {
                const auto value = oldFields.value().get( mapping.Old );
                if ( !value.has_value() )
                    continue; // absent: the new field keeps its C++ default, which is not a failure

                if ( CarryField( tag, mapping, value.value(), sky ) )
                    ++carried;
                else
                    ++rejected;
            }

            // Everything the new component has and the old payload could not fill - the time-of-day
            // block, the environment-bake knobs, ActivePreset and PlanetRadius - is left OUT of the
            // payload on purpose: an absent key is exactly how the reflection serializer spells "keep the
            // C++ default", so the defaults live in one place (the component) instead of two.
            entity.Components["SkyAtmosphere"] = rfl::Generic( std::move( sky ) );

            report.Entities += 1;
            report.FieldsCarried += carried;
            report.FieldsRejected += rejected;
            report.FieldsDefaulted += kSkyAtmosphereFieldCount - carried - rejected;
        }

        return report;
    }

    namespace
    {
        // One world unit used to be a metre and is now a centimetre, so every length in an unstamped file
        // is short by this factor. Named once here so the migration and Units.hpp cannot drift.
        constexpr double kMetresToUnits = static_cast<double>( Common::Units::UnitsPerMetre );

        enum class Arity
        {
            Scalar, // a single number
            Vec3,   // an array of exactly three numbers
        };

        // Every LENGTH a component payload can carry, by the key the ComponentRegistry writes it under.
        // This is the complete list - a field that is not here is not a distance (an angle, a colour, a
        // count, a frequency), and a field that is here and is NOT a distance would silently inflate a
        // scene by a hundred. It is stated as data rather than code so the census can be read in one look.
        struct ScaledField
        {
            const char* Component;
            const char* Field;
            Arity       Kind;
        };

        constexpr auto kScaledFields = std::to_array<ScaledField>( {
             { "Camera", "Near", Arity::Scalar },
             { "Camera", "Far", Arity::Scalar },
             { "PointLight", "Radius", Arity::Scalar },
             { "PointLight", "MinRadius", Arity::Scalar },
             { "SpotLight", "Range", Arity::Scalar },
             { "Collider", "HalfExtents", Arity::Vec3 },
             { "Collider", "Radius", Arity::Scalar },
             { "Collider", "HalfHeight", Arity::Scalar },
             { "CharacterController", "Radius", Arity::Scalar },
             { "CharacterController", "Height", Arity::Scalar },
             { "CharacterController", "Gravity", Arity::Scalar },
             { "Terrain", "Size", Arity::Scalar },
             { "Terrain", "HeightScale", Arity::Scalar },
             // `Terrain.GrassHeight` was the row after this one until Г25. It is gone rather than kept
             // "for old files": this census is a statement about the LENGTH FIELDS THE SCHEMA HAS, and
             // the v17 -> v18 step deletes that key from every payload it can reach, so a row here could
             // only ever convert a number on its way to being removed. A census naming a field the schema
             // does not have is the stale row this project has paid for twice.
             { "Text", "Size", Arity::Scalar },
        } );

        // Multiplies one value in place. Returns false when the key was there but unusable, which is the
        // only case worth reporting - an absent key is a scene that predates the field, not a failure.
        bool ScaleValue( const std::string& where, const char* field, Arity kind, rfl::Generic::Object& obj )
        {
            const auto value = obj.get( field );
            if ( !value.has_value() )
                return true; // absent: nothing to scale, and nothing went wrong

            if ( kind == Arity::Scalar )
            {
                const auto n = AsFiniteNumber( value.value() );
                if ( !n.has_value() )
                {
                    LOG_WARN( "[SceneMigration] {0}.{1} is {2}, expected a finite number - left in metres", where,
                              field, Describe( value.value() ) );
                    return false;
                }
                obj[field] = n.value() * kMetresToUnits;
                return true;
            }

            const auto arr = value.value().to_array();
            if ( !arr.has_value() || arr.value().size() != 3 )
            {
                LOG_WARN( "[SceneMigration] {0}.{1} is {2}, expected an array of 3 numbers - left in metres",
                          where, field, Describe( value.value() ) );
                return false;
            }

            rfl::Generic::Array scaled;
            for ( const auto& component : arr.value() )
            {
                const auto n = AsFiniteNumber( component );
                if ( !n.has_value() )
                {
                    LOG_WARN( "[SceneMigration] {0}.{1} is {2}, expected 3 finite numbers - left in metres", where,
                              field, Describe( value.value() ) );
                    return false;
                }
                scaled.push_back( rfl::Generic( n.value() * kMetresToUnits ) );
            }
            obj[field] = std::move( scaled );
            return true;
        }

        // A top-level transform vector (Translation / Scale). Absent means the entity never authored one,
        // and the component default it will be created with is not a metres-era length - see the header.
        bool ScaleTransform( const std::string& tag, const char* what, std::optional<glm::vec3>& v )
        {
            if ( !v.has_value() )
                return false;

            const glm::vec3 before = *v;
            *v *= static_cast<float>( kMetresToUnits );
            if ( !std::isfinite( v->x ) || !std::isfinite( v->y ) || !std::isfinite( v->z ) )
            {
                LOG_WARN( "[SceneMigration] entity '{0}': {1} is not finite after scaling - restored", tag, what );
                *v = before;
                return false;
            }
            return true;
        }

        // A procedural primitive is REGENERATED at its authored size by the factory, so its Scale is a
        // multiplier on geometry the engine builds, not a length the file owns - scaling it would cube the
        // object. A file-backed mesh has no such regeneration and its Scale is a real length.
        bool HasProceduralMesh( const Assets::EntityData& entity )
        {
            const auto mesh = entity.Components.get( "StaticMesh" );
            if ( !mesh.has_value() )
                return false;
            const auto fields = mesh.value().to_object();
            return fields.has_value() && fields.value().get( "Primitive" ).has_value();
        }
    } // namespace

    UnitMigrationReport MigrateMetresToUnits( std::vector<Assets::EntityData>& entities,
                                              std::optional<rfl::Generic>&     settings )
    {
        UnitMigrationReport report;

        for ( auto& entity : entities )
        {
            const std::string tag     = entity.Tag.value_or( "Entity" );
            int               values  = 0;
            int               refused = 0;

            if ( entity.Translation.has_value() )
            {
                if ( ScaleTransform( tag, "Translation", entity.Translation ) )
                    ++values;
                else
                    ++refused;
            }

            if ( entity.Scale.has_value() && !HasProceduralMesh( entity ) )
            {
                if ( ScaleTransform( tag, "Scale", entity.Scale ) )
                    ++values;
                else
                    ++refused;
            }

            for ( const auto& scaled : kScaledFields )
            {
                const auto payload = entity.Components.get( scaled.Component );
                if ( !payload.has_value() )
                    continue;

                auto fields = payload.value().to_object();
                if ( !fields.has_value() )
                {
                    LOG_WARN( "[SceneMigration] entity '{0}': the {1} payload is {2}, not an object - its "
                              "lengths stay in metres",
                              tag, scaled.Component, Describe( payload.value() ) );
                    ++refused;
                    continue;
                }
                if ( !fields.value().get( scaled.Field ).has_value() )
                    continue; // the field is absent; the component default applies and is already in units

                const std::string where = "entity '" + tag + "': " + scaled.Component;
                if ( ScaleValue( where, scaled.Field, scaled.Kind, fields.value() ) )
                    ++values;
                else
                    ++refused;

                entity.Components[scaled.Component] = rfl::Generic( std::move( fields.value() ) );
            }

            if ( values > 0 )
                report.Entities += 1;
            report.Values += values;
            report.Rejected += refused;
        }

        // Gravity is the one scene-wide length-per-second-squared. Absent keeps the C++ default, which is
        // already stated in world units.
        if ( settings.has_value() )
        {
            auto fields = settings->to_object();
            if ( fields.has_value() && fields.value().get( "Gravity" ).has_value() )
            {
                if ( ScaleValue( "Settings", "Gravity", Arity::Scalar, fields.value() ) )
                    report.Values += 1;
                else
                    report.Rejected += 1;
                settings = rfl::Generic( std::move( fields.value() ) );
            }
        }

        return report;
    }

    namespace
    {
        // The reflected field name of SceneSettings::Tonemapper - the key the generic serializer reads
        // and writes. Stated once so the migration and the round trip cannot disagree about spelling.
        constexpr const char* kTonemapperKey = "Tonemapper";
    } // namespace

    TonemapMigrationReport MigrateTonemapperV1ToV2( std::optional<rfl::Generic>& settings )
    {
        TonemapMigrationReport report;

        rfl::Generic::Object fields;
        if ( settings.has_value() )
        {
            auto parsed = settings->to_object();
            if ( !parsed.has_value() )
            {
                // Not an object: this scene's whole settings block is unreadable. Replacing it with a
                // fresh one would discard every other scene-wide value to save this single field, so it
                // is left exactly as found and said out loud instead - the scene will load on the C++
                // default (ACES) and its author needs to know that before wondering why it re-graded.
                LOG_WARN( "[SceneMigration] the Settings payload is {0}, not an object - the tonemapper "
                          "could not be pinned and this scene will load on the default operator",
                          Describe( *settings ) );
                return report;
            }
            fields = std::move( parsed.value() );
        }
        else
        {
            report.SettingsCreated = true;
        }

        if ( fields.get( kTonemapperKey ).has_value() )
            return report; // the file already states its operator - see the idempotence note in the header

        fields[kTonemapperKey] = static_cast<int64_t>( Core::TonemapOperator::Reinhard );
        report.OperatorPinned  = true;

        settings = rfl::Generic( std::move( fields ) );
        return report;
    }

    CloudNoiseMigrationReport MigrateCloudNoiseV2ToV3( std::vector<Assets::EntityData>& entities )
    {
        // The four keys the GPU bake was parameterised by. Named as data rather than tested for one at a
        // time so the list can be read in one look and so the count in the report cannot drift from it.
        static constexpr std::array kRemovedBakeKeys = { "WeatherSeed", "WeatherOctaves", "DetailSeed",
                                                         "DetailOctaves" };

        CloudNoiseMigrationReport report;

        for ( auto& entity : entities )
        {
            const auto clouds = entity.Components.get( "VolumetricCloud" );
            if ( !clouds.has_value() )
                continue;

            const auto fields = clouds.value().to_object();
            if ( !fields.has_value() )
            {
                LOG_WARN( "[SceneMigration] entity '{0}': the VolumetricCloud payload is {1}, not an object - "
                          "its bake settings could not be removed",
                          entity.Tag.value_or( "Entity" ), Describe( clouds.value() ) );
                continue;
            }

            // Rebuilt rather than erased in place: rfl::Object is an ordered vector of pairs with no erase,
            // and copying every key except the four states the intent more plainly than an index dance
            // would. Order is preserved, so a re-saved file differs from the old one only by the four lines.
            rfl::Generic::Object kept;
            int                  dropped = 0;

            for ( const auto& [key, value] : fields.value() )
            {
                const bool isBakeKey = std::any_of( std::begin( kRemovedBakeKeys ), std::end( kRemovedBakeKeys ),
                                                    [&key]( const char* removed ) { return key == removed; } );
                if ( isBakeKey )
                {
                    ++dropped;
                    continue;
                }
                kept[key] = value;
            }

            if ( dropped == 0 )
                continue; // already raised, or authored after the move - leave the tree byte-identical

            entity.Components["VolumetricCloud"] = rfl::Generic( std::move( kept ) );
            report.Entities += 1;
            report.FieldsDropped += dropped;
        }

        return report;
    }

    CloudSpeciesMigrationReport MigrateCloudSpeciesV3ToV4( std::vector<Assets::EntityData>& entities )
    {
        // The three keys with nowhere to go. Named as data rather than tested one at a time so the list
        // reads in one look and the count in the report cannot drift from it.
        static constexpr std::array  kRemovedKeys   = { "LayerBottomAltitude", "LayerThickness",
                                                        "CloudTypeVariance" };
        static constexpr const char* kTypeKey       = "CloudType";
        static constexpr const char* kSpeciesKey    = "Species";

        CloudSpeciesMigrationReport report;

        for ( auto& entity : entities )
        {
            const auto clouds = entity.Components.get( "VolumetricCloud" );
            if ( !clouds.has_value() )
                continue;

            const auto fields = clouds.value().to_object();
            if ( !fields.has_value() )
            {
                LOG_WARN( "[SceneMigration] entity '{0}': the VolumetricCloud payload is {1}, not an object - "
                          "its cloud type could not be turned into a species",
                          entity.Tag.value_or( "Entity" ), Describe( clouds.value() ) );
                continue;
            }

            // Rebuilt rather than erased in place, like the migration above it: rfl::Object is an ordered
            // vector of pairs with no erase, and copying every key except the dropped ones states the
            // intent more plainly than an index dance. Order is preserved.
            rfl::Generic::Object kept;
            int                  dropped = 0;
            bool                 typed   = false;

            for ( const auto& [key, value] : fields.value() )
            {
                const bool isRemoved = std::any_of( std::begin( kRemovedKeys ), std::end( kRemovedKeys ),
                                                    [&key]( const char* removed ) { return key == removed; } );
                if ( isRemoved )
                {
                    ++dropped;
                    continue;
                }

                if ( key == kTypeKey )
                {
                    ++dropped;

                    const auto scalar = value.to_double();
                    if ( !scalar.has_value() )
                    {
                        // A CloudType that is not a number tells us nothing about what the author wanted,
                        // and the C++ default is a species in its own right. Said out loud, because a
                        // value we drop silently is a value nobody will ever find again.
                        LOG_WARN( "[SceneMigration] entity '{0}': CloudType is {1}, not a number - the layer "
                                  "keeps the default species",
                                  entity.Tag.value_or( "Entity" ), Describe( value ) );
                        continue;
                    }

                    // The library is ordered from the flattest species to the tallest, which is the axis
                    // the old scalar ran along, so the quarters below are a translation rather than a
                    // guess. 0.6 - the component's own former default - lands on cumulus congestus, which
                    // the default of the version after this one also names.
                    //
                    // The integers are indices into kSpeciesOrder below, which is the ONE statement of that
                    // order left in this file; the enumerator they used to name was deleted when the
                    // species became an asset, and this migration deliberately still writes the OLD key,
                    // because the v4 -> v5 step is what turns it into a handle. Chaining like that is the
                    // whole reason each step is gated on its own version integer.
                    const double type = scalar.value();
                    int          species;
                    if ( type < 0.25 )
                        species = 0; // Stratus
                    else if ( type < 0.55 )
                        species = 1; // Cumulus mediocris
                    else if ( type < 0.85 )
                        species = 2; // Cumulus congestus
                    else
                        species = 3; // Cumulonimbus

                    kept[kSpeciesKey] = static_cast<int64_t>( species );
                    typed             = true;
                    continue;
                }

                kept[key] = value;
            }

            if ( dropped == 0 )
                continue; // already raised, or authored after the move - leave the tree byte-identical

            entity.Components["VolumetricCloud"] = rfl::Generic( std::move( kept ) );
            report.Entities += 1;
            report.FieldsDropped += dropped;
            report.SpeciesSet += typed ? 1 : 0;
        }

        return report;
    }

    CloudTypeMigrationReport MigrateCloudTypeV4ToV5( std::vector<Assets::EntityData>& entities )
    {
        static constexpr const char* kSpeciesKey = "Species";
        static constexpr const char* kNoiseKey   = "NoiseVolume";
        static constexpr const char* kTypeKey    = "CloudType";

        // THE FOUR KINDS T0 COMPILED IN, IN ITS ORDER, and the only place that order is written down now
        // that the enumerator is gone. The integer a v4 file carries in "Species" indexes this array, and
        // each name is the stem of a shipped `.decloudtype` carrying the same twelve numbers T0 compiled
        // in. Two statements of one library, and Desert/Tests/Engine/CloudType is what keeps them equal —
        // it opens each file this array names and compares it against what T0 shipped.
        static constexpr std::array kSpeciesOrder = {
             Assets::kCloudTypeStratus,
             Assets::kCloudTypeCumulusMediocris,
             Assets::kCloudTypeCumulusCongestus,
             Assets::kCloudTypeCumulonimbus,
        };
        constexpr int kSpeciesCount = static_cast<int>( std::size( kSpeciesOrder ) );

        CloudTypeMigrationReport report;

        for ( auto& entity : entities )
        {
            const auto clouds = entity.Components.get( "VolumetricCloud" );
            if ( !clouds.has_value() )
                continue;

            const auto fields = clouds.value().to_object();
            if ( !fields.has_value() )
            {
                LOG_WARN( "[SceneMigration] entity '{0}': the VolumetricCloud payload is {1}, not an object - "
                          "its species could not be turned into a cloud type asset",
                          entity.Tag.value_or( "Entity" ), Describe( clouds.value() ) );
                continue;
            }

            // Rebuilt rather than erased in place, like the two migrations above it: rfl::Object is an
            // ordered vector of pairs with no erase, and copying every key except the ones that move states
            // the intent more plainly than an index dance. Order is preserved.
            rfl::Generic::Object kept;
            bool                 touched = false;

            for ( const auto& [key, value] : fields.value() )
            {
                if ( key == kNoiseKey )
                {
                    touched = true;

                    // NAMED, NOT SWALLOWED. The slot moved onto the cloud type and a pure function cannot
                    // create the file it would have to move it into, so the artist is told exactly which
                    // layer lost which volume and where to put it back. A value dropped without a message
                    // is a value nobody will ever find again (§1.4).
                    const auto handle = value.to_int();
                    if ( handle.has_value() && handle.value() != 0 )
                    {
                        report.VolumesLost += 1;
                        LOG_WARN( "[SceneMigration] entity '{0}': the layer named noise volume {1}, which "
                                  "is now a field of the CLOUD TYPE rather than of the layer. Open the "
                                  "type in Window > Cloud Type and point its Noise Volume at that .dcnv; "
                                  "until then the layer uses the built-in default volume.",
                                  entity.Tag.value_or( "Entity" ), handle.value() );
                    }
                    continue;
                }

                if ( key == kSpeciesKey )
                {
                    touched = true;

                    const auto index = value.to_int();
                    if ( !index.has_value() )
                    {
                        // A species that is not an integer says nothing about what the author wanted, and
                        // the empty slot is a kind of cloud in its own right - the built-in congestus.
                        report.FieldsBroken += 1;
                        LOG_WARN( "[SceneMigration] entity '{0}': Species is {1}, not an integer - the "
                                  "layer keeps the built-in default cloud type",
                                  entity.Tag.value_or( "Entity" ), Describe( value ) );
                        continue;
                    }

                    const int64_t species = index.value();
                    if ( species < 0 || species >= kSpeciesCount )
                    {
                        // A hand-edited file can carry any integer. T0's own reader clamped this to the
                        // first species; here it becomes the EMPTY slot instead, because "the value is not
                        // one of the four" and "the author chose stratus" are different statements and only
                        // the first one is true.
                        report.FieldsBroken += 1;
                        LOG_WARN( "[SceneMigration] entity '{0}': Species is {1}, which is not one of the {2} "
                                  "kinds this file could name - the layer keeps the built-in default cloud "
                                  "type",
                                  entity.Tag.value_or( "Entity" ), species, kSpeciesCount );
                        continue;
                    }

                    // A PATH AND NOT A HANDLE, because that is what a reflected asset field is written as
                    // in this engine: Core::MakeAssetResolver turns every one of them into a path on save
                    // and back into a handle on load, so a scene carrying a raw integer here would be read
                    // through the resolver's string branch, fail it, and land on the empty slot. It is
                    // relative to the assets root for the reason CloudTypeAssetRelativePath states — a
                    // pure function cannot know where the project is installed, and the library ships
                    // inside it.
                    kept[kTypeKey] = Assets::CloudTypeAssetRelativePath( kSpeciesOrder[species] );
                    report.TypesSet += 1;
                    continue;
                }

                kept[key] = value;
            }

            if ( !touched )
                continue; // already raised, or authored after the move - leave the tree byte-identical

            entity.Components["VolumetricCloud"] = rfl::Generic( std::move( kept ) );
            report.Entities += 1;
        }

        return report;
    }

    CloudSetMigrationReport MigrateCloudSetV5ToV6( std::vector<Assets::EntityData>& entities )
    {
        static constexpr const char* kTypeKey = "CloudType";
        static constexpr const char* kSlotKey = "CloudType1";

        CloudSetMigrationReport report;

        for ( auto& entity : entities )
        {
            const auto clouds = entity.Components.get( "VolumetricCloud" );
            if ( !clouds.has_value() )
                continue;

            const auto fields = clouds.value().to_object();
            if ( !fields.has_value() )
            {
                LOG_WARN( "[SceneMigration] entity '{0}': the VolumetricCloud payload is {1}, not an object - "
                          "its cloud type could not be moved into the first slot of the set",
                          entity.Tag.value_or( "Entity" ), Describe( clouds.value() ) );
                continue;
            }

            // Rebuilt rather than renamed in place, like every migration above it: rfl::Object is an
            // ordered vector of pairs with no rename, and copying every key while changing one name states
            // the intent more plainly than an index dance. Order is preserved, so the slot lands exactly
            // where the single type used to be.
            rfl::Generic::Object kept;
            bool                 touched = false;

            for ( const auto& [key, value] : fields.value() )
            {
                if ( key != kTypeKey )
                {
                    kept[key] = value;
                    continue;
                }

                touched = true;
                report.SlotsCarried += 1;

                // NOT INSPECTED BEYOND THIS. Whatever the value is — a path to a `.decloudtype`, the empty
                // handle, or something a hand-edit put there — it meant "the kind of cloud this layer is
                // made of" and it still does; the key it lives under is the only thing that changed. A
                // migration that also validated would be answering a question the loader answers next, and
                // answering it twice is how two readers of one field end up disagreeing.
                const auto text = value.to_string();
                if ( ( text.has_value() && text.value().empty() ) ||
                     ( value.to_int().has_value() && value.to_int().value() == 0 ) )
                    report.SlotsEmpty += 1;

                kept[kSlotKey] = value;
            }

            if ( !touched )
                continue; // already raised, or authored after the move - leave the tree byte-identical

            entity.Components["VolumetricCloud"] = rfl::Generic( std::move( kept ) );
            report.Entities += 1;
        }

        return report;
    }

    TerrainMaterialMigrationReport MigrateTerrainMaterialV6ToV7( std::vector<Assets::EntityData>& entities )
    {
        static constexpr const char* kTerrainKey  = "Terrain";
        static constexpr const char* kMaterialKey = "Material";

        TerrainMaterialMigrationReport report;

        for ( auto& entity : entities )
        {
            // BOTH keys, and that pairing is the whole gate. A "Material" on an entity with no terrain is
            // the runtime/script override channel and stays exactly where it is; only the terrain's copy of
            // it was ever an AUTHORING surface, and only the terrain's copy goes.
            if ( !entity.Components.get( kTerrainKey ).has_value() )
                continue;

            const auto material = entity.Components.get( kMaterialKey );
            if ( !material.has_value() )
                continue; // already raised, or a terrain that never had one - leave the tree byte-identical

            // Counted before it is dropped. A malformed payload (a number, a string, a hand-edit) still
            // COUNTS as a removal and is still reported: the entity carried something under that key, and
            // saying "nothing was there" because it could not be read would be the quiet substitution this
            // whole clause exists to forbid.
            const auto fields = material.value().to_object();
            if ( !fields.has_value() )
            {
                LOG_WARN( "[SceneMigration] entity '{0}': the terrain's Material payload is {1}, not an "
                          "object - it is removed with the authoring path it belonged to, and nothing "
                          "could be read out of it to name here",
                          entity.Tag.value_or( "Entity" ), Describe( material.value() ) );
            }
            else
            {
                for ( const auto& [key, value] : fields.value() )
                {
                    // The two vectors of values. "ShaderName" is deliberately not among them: the new model
                    // has exactly one Terrain-domain program and a terrain material is created on it, so the
                    // name was never a value anybody has to re-author.
                    if ( key != "Params" && key != "Textures" )
                        continue;

                    const auto rows = value.to_array();
                    if ( !rows.has_value() )
                    {
                        LOG_WARN( "[SceneMigration] entity '{0}': the terrain's Material.{1} is {2}, not an "
                                  "array - it is removed and its contents cannot be named",
                                  entity.Tag.value_or( "Entity" ), key, Describe( value ) );
                        continue;
                    }

                    for ( const auto& row : rows.value() )
                    {
                        // The name is what makes this reportable at all, so a row without one is still
                        // counted and reported under a placeholder rather than skipped.
                        std::string name = "<unnamed>";
                        if ( const auto rowFields = row.to_object(); rowFields.has_value() )
                        {
                            if ( const auto named = rowFields.value().get( "Name" ); named.has_value() )
                            {
                                if ( const auto text = named.value().to_string(); text.has_value() )
                                    name = text.value();
                            }
                        }

                        if ( key == "Params" )
                            report.Params += 1;
                        else
                            report.Textures += 1;
                        report.DroppedNames.push_back( std::move( name ) );
                    }
                }
            }

            // Rebuilt rather than erased, exactly like the payload migrations above: rfl::Object is an
            // ordered vector of pairs with no erase, so every OTHER component is copied across in order and
            // the one being retired is simply not.
            rfl::ExtraFields<rfl::Generic> kept;
            for ( const auto& [key, value] : entity.Components )
            {
                if ( key != kMaterialKey )
                    kept[key] = value;
            }
            entity.Components = std::move( kept );
            report.Entities += 1;
        }

        return report;
    }

    namespace
    {
        // The components that can name a material, and the key each names it under. `MaterialPaths` is a
        // LIST (one per mesh slot); `Terrain.Material` is a single string. Kept as one table so a fifth
        // component that gains a material slot is one line here rather than a fourth copy of the loop.
        struct MaterialPathSite
        {
            const char* Component;
            const char* Key;
            bool        IsList;
        };

        constexpr auto kMaterialPathSites = std::to_array<MaterialPathSite>( {
             { "StaticMesh", "MaterialPaths", true },
             { "InstancedStaticMesh", "MaterialPaths", true },
             { "SkinnedMesh", "MaterialPaths", true },
             { "Terrain", "Material", false },
        } );

        // The path `stored` names, expressed relative to `assetsRoot`, or nullopt when it already is (or
        // lies outside the root, or is empty).
        //
        // Lexical on purpose - see the header. The root's own components are matched as a contiguous run
        // inside the stored path and the LAST match wins, so the answer does not depend on whether the
        // root arrived spelled relatively or absolutely.
        // Does this string LOOK rooted, on any platform rather than on this one?
        //
        // std::filesystem::path::is_absolute() asks the grammar of the HOST, and that is the wrong question
        // here. A scene travels between developers: a POSIX "/Users/dan/Proj/.../M.demat" read on Windows is
        // is_absolute() == FALSE, because Windows wants a drive letter. Used to decide whether to REPORT an
        // unrewritable path, that hole is silent exactly where it matters most -- the Mac-authored scene
        // opened on Windows, which is the defect this whole migration exists to prevent. Found by CI: the
        // test was right and the code was wrong, on Windows only, with macOS green.
        //
        // So: a leading '/', a drive letter, or a UNC prefix. Any of the three means "this was rooted
        // somewhere, and it is not under our assets root" — which is a fact about the string, not about the
        // machine reading it.
        bool LooksRootedOnAnyPlatform( const std::string& stored )
        {
            if ( stored.empty() )
                return false;
            if ( stored[0] == '/' || stored[0] == '\\' ) // POSIX absolute, or a UNC / drive-relative root
                return true;
            return stored.size() >= 3 && std::isalpha( static_cast<unsigned char>( stored[0] ) ) &&
                   stored[1] == ':' && ( stored[2] == '/' || stored[2] == '\\' );
        }

        std::optional<std::string> RelativeToAssetsRoot( const std::string&           stored,
                                                         const std::filesystem::path& assetsRoot )
        {
            if ( stored.empty() )
                return std::nullopt; // "no material in this slot" - not a path to rewrite

            std::vector<std::string> rootParts;
            for ( const auto& part : assetsRoot.lexically_normal() )
            {
                // A trailing separator makes the last component an empty string ("Resources/Assets/" ->
                // {"Resources","Assets",""}), and matching on it would match everywhere.
                if ( !part.empty() && part != "." )
                    rootParts.push_back( part.generic_string() );
            }
            if ( rootParts.empty() )
                return std::nullopt;

            std::vector<std::string> pathParts;
            for ( const auto& part : std::filesystem::path( stored ).lexically_normal() )
                pathParts.push_back( part.generic_string() );

            if ( pathParts.size() <= rootParts.size() )
                return std::nullopt;

            std::size_t bestEnd = 0; // one past the last component of the best match, 0 = no match
            for ( std::size_t start = 0; start + rootParts.size() < pathParts.size(); ++start )
            {
                if ( std::equal( rootParts.begin(), rootParts.end(), pathParts.begin() + start ) )
                    bestEnd = start + rootParts.size();
            }
            if ( bestEnd == 0 )
                return std::nullopt; // not under this root - the caller reports it rather than guessing

            std::string relative;
            for ( std::size_t i = bestEnd; i < pathParts.size(); ++i )
            {
                if ( !relative.empty() )
                    relative += '/';
                relative += pathParts[i];
            }
            if ( relative.empty() || relative == stored )
                return std::nullopt;
            return relative;
        }

        // `stored` with `root`'s leading components removed, as a generic ('/') string.
        //
        // NO STRING MATCHING, deliberately, and that is what makes it different from
        // RelativeToAssetsRoot above: `root` here is not a root somebody handed us to look for, it is the
        // one Constants::Path::RootForContentPath BUILT out of this very path's own components. So the
        // number of components to drop is already known exactly, and searching for them again would only
        // create a second answer that could disagree with the first.
        std::string StripRoot( const std::string& stored, const std::filesystem::path& root )
        {
            std::size_t drop = 0;
            for ( const auto& part : root )
            {
                if ( !part.empty() && part != "." )
                    ++drop;
            }

            std::string relative;
            std::size_t index = 0;
            for ( const auto& part : std::filesystem::path( stored ).lexically_normal() )
            {
                if ( index++ < drop )
                    continue;
                if ( !relative.empty() )
                    relative += '/';
                relative += part.generic_string();
            }
            return relative;
        }

        // Components of a path, normalized, with the empty trailing part and "." dropped. A trailing
        // separator makes the last component an empty string ("Resources/" -> {"Resources",""}) and
        // matching on it would match everywhere.
        std::vector<std::string> PathComponents( const std::filesystem::path& p )
        {
            std::vector<std::string> parts;
            for ( const auto& part : p.lexically_normal() )
            {
                if ( !part.empty() && part != "." )
                    parts.push_back( part.generic_string() );
            }
            return parts;
        }

        // Where a candidate root ends inside `path`, and how much of the root that match was worth.
        //
        // BOTH SIDES CAN CARRY AN EXTRA PREFIX, which is what makes this more than a prefix compare. The
        // editor writes paths from its own working directory (`Editor/`) and this tool is run from the
        // repository root, so one file is spelled `Resources/Icons/gear.svg` in the scene and
        // `Editor/Resources/Icons/gear.svg` here: the root can be missing components at its front OR be
        // preceded by components the path adds at its front. So the search is for the LAST occurrence,
        // anywhere in the path, of the LONGEST TRAILING RUN of the root's own components.
        //
        // `Run` is how many root components the match accounted for, and it is what decides between the
        // two candidate roots — the lexical analogue of the run-time rule, where StableKeyForPath keeps
        // the match against the LONGEST absolute root. That rule is load-bearing rather than tidy: in the
        // sandbox layout `Resources/Assets/` is NESTED INSIDE `Resources/`, so a shorter match would tag
        // every project asset as an engine resource — a reference that resolves in the development tree,
        // where both roots hang off one directory, and names nothing in a package.
        struct RootMatch
        {
            std::size_t Run = 0; // root components matched; 0 = this root does not contain the path
            std::size_t End = 0; // one past the last path component the match consumed
        };

        RootMatch MatchRoot( const std::vector<std::string>& path, const std::vector<std::string>& root )
        {
            for ( std::size_t take = root.size(); take >= 1; --take )
            {
                const auto runBegin = root.end() - static_cast<std::ptrdiff_t>( take );
                RootMatch  best;
                for ( std::size_t start = 0; start + take < path.size(); ++start )
                {
                    if ( std::equal( runBegin, root.end(), path.begin() + static_cast<std::ptrdiff_t>( start ) ) )
                        best = RootMatch{ take, start + take }; // last occurrence wins
                }
                if ( best.Run > 0 )
                    return best;
            }
            return {};
        }
    } // namespace

    GravityUnitsMigrationReport MigrateGravityUnitsV8ToV9( std::optional<rfl::Generic>& settings )
    {
        GravityUnitsMigrationReport report;

        if ( !settings.has_value() )
            return report; // a scene with no Settings block states no gravity; nothing to restate

        const auto fields = settings.value().to_object();
        if ( !fields.has_value() )
        {
            LOG_WARN( "[SceneMigration] the Settings block is {0}, not an object - the scene's gravity "
                      "could not be restated in centimetres and stays as it is",
                      Describe( settings.value() ) );
            return report;
        }

        // Earth, in the two spellings the repository actually contains. Compared with a tolerance because
        // both arrived through a float round-trip: 9.81 is stored as 9.8100004196167 and the value the
        // earlier x100 produced is 981.0000419616699.
        constexpr double kEarthMetres      = 9.81;
        constexpr double kEarthCentimetres = 981.0;
        constexpr double kTolerance        = 0.01;

        rfl::Generic::Object kept;
        for ( const auto& [key, value] : fields.value() )
        {
            if ( key != "Gravity" )
            {
                kept[key] = value;
                continue;
            }

            // AsFiniteNumber, not to_double: a hand-edited scene writes "Gravity":981, which reflect-cpp
            // parses as int64 and to_double() then refuses - the same trap the sky migration documents.
            const auto number = AsFiniteNumber( value );
            if ( !number.has_value() )
            {
                LOG_WARN( "[SceneMigration] Settings.Gravity is {0}, not a finite number - it is left "
                          "exactly as it is and the scene still states no usable gravity",
                          Describe( value ) );
                kept[key] = value;
                continue;
            }

            report.Found  = true;
            report.Before = static_cast<float>( number.value() );

            if ( std::abs( number.value() - kEarthMetres ) < kTolerance )
            {
                report.Scaled = true;
                report.After  = static_cast<float>( kEarthCentimetres );
                kept[key]     = kEarthCentimetres;
            }
            else if ( std::abs( number.value() - kEarthCentimetres ) < kTolerance )
            {
                // Already centimetres. Rewritten anyway when it carries the earlier pass's rounding, so the
                // repository stops storing the arithmetic of a migration instead of a value.
                report.Tidied = ( number.value() != kEarthCentimetres );
                report.After  = static_cast<float>( kEarthCentimetres );
                kept[key]     = kEarthCentimetres;
            }
            else
            {
                // Neither Earth in metres nor Earth in centimetres. A threshold cannot tell a deliberate
                // low-gravity level from a forgotten metre-era value, so this refuses to guess and says so.
                report.Unrecognised = true;
                report.After        = report.Before;
                kept[key]           = value;
                LOG_WARN( "[SceneMigration] Settings.Gravity is {0}, which is neither Earth in metres "
                          "({1}) nor Earth in centimetres ({2}) - it is LEFT UNCHANGED because guessing "
                          "which was meant would silently rescale a deliberately authored value. If this "
                          "scene predates centimetres, its gravity is a hundred times too weak and wants "
                          "editing by hand",
                          number.value(), kEarthMetres, kEarthCentimetres );
            }
        }

        settings = rfl::Generic( kept );
        return report;
    }

    MaterialPathMigrationReport MigrateMaterialPathV7ToV8( std::vector<Assets::EntityData>& entities,
                                                           const std::filesystem::path&     assetsRoot )
    {
        MaterialPathMigrationReport report;

        for ( auto& entity : entities )
        {
            const std::string tag     = entity.Tag.value_or( "Entity" );
            bool              touched = false;

            for ( const auto& site : kMaterialPathSites )
            {
                const auto payload = entity.Components.get( site.Component );
                if ( !payload.has_value() )
                    continue;

                const auto fields = payload.value().to_object();
                if ( !fields.has_value() )
                {
                    LOG_WARN( "[SceneMigration] entity '{0}': the {1} payload is {2}, not an object - the "
                              "material path(s) in it could not be made relative and stay as they are",
                              tag, site.Component, Describe( payload.value() ) );
                    continue;
                }

                const auto named = fields.value().get( site.Key );
                if ( !named.has_value() )
                    continue; // this component names no material - nothing to do, tree untouched

                // Rebuilt rather than assigned into: rfl::Object is an ordered vector of pairs, and copying
                // every key while replacing one value is what every migration above this one does. Order is
                // preserved, so a scene that changes nothing round-trips byte-identically.
                rfl::Generic::Object kept;
                bool                 rewroteHere = false;

                for ( const auto& [key, value] : fields.value() )
                {
                    if ( key != site.Key )
                    {
                        kept[key] = value;
                        continue;
                    }

                    if ( !site.IsList )
                    {
                        const auto text = value.to_string();
                        if ( !text.has_value() )
                        {
                            LOG_WARN( "[SceneMigration] entity '{0}': {1}.{2} is {3}, not a string - it is "
                                      "left exactly as it is and still names no material relative to the "
                                      "assets root",
                                      tag, site.Component, site.Key, Describe( value ) );
                            kept[key] = value;
                            continue;
                        }

                        if ( const auto rel = RelativeToAssetsRoot( text.value(), assetsRoot ) )
                        {
                            kept[key] = *rel;
                            report.Paths += 1;
                            rewroteHere = true;
                        }
                        else
                        {
                            if ( LooksRootedOnAnyPlatform( text.value() ) )
                                report.OutsideNames.push_back( tag + " > " + site.Component + "." + site.Key +
                                                               " = " + text.value() );
                            kept[key] = value;
                        }
                        continue;
                    }

                    const auto rows = value.to_array();
                    if ( !rows.has_value() )
                    {
                        LOG_WARN( "[SceneMigration] entity '{0}': {1}.{2} is {3}, not an array - the slot "
                                  "paths in it are left exactly as they are",
                                  tag, site.Component, site.Key, Describe( value ) );
                        kept[key] = value;
                        continue;
                    }

                    rfl::Generic::Array slots;
                    for ( const auto& row : rows.value() )
                    {
                        const auto text = row.to_string();
                        if ( !text.has_value() )
                        {
                            LOG_WARN( "[SceneMigration] entity '{0}': a slot of {1}.{2} is {3}, not a "
                                      "string - it is left exactly as it is",
                                      tag, site.Component, site.Key, Describe( row ) );
                            slots.push_back( row );
                            continue;
                        }

                        if ( const auto rel = RelativeToAssetsRoot( text.value(), assetsRoot ) )
                        {
                            slots.push_back( rfl::Generic( *rel ) );
                            report.Paths += 1;
                            rewroteHere = true;
                            continue;
                        }

                        if ( LooksRootedOnAnyPlatform( text.value() ) )
                            report.OutsideNames.push_back( tag + " > " + site.Component + "." + site.Key + " = " +
                                                           text.value() );
                        slots.push_back( row );
                    }
                    kept[key] = std::move( slots );
                }

                if ( !rewroteHere )
                    continue; // already relative, or nothing usable - leave the tree byte-identical

                entity.Components[site.Component] = rfl::Generic( std::move( kept ) );
                touched                           = true;
            }

            if ( touched )
                report.Entities += 1;
        }

        return report;
    }

    UIVisibilityMigrationReport MigrateUIVisibilityV9ToV10( std::vector<Assets::EntityData>& entities )
    {
        static constexpr const char* kInteractableKey  = "Interactable";
        static constexpr const char* kRaycastTargetKey = "RaycastTarget";
        static constexpr const char* kHitTestKey       = "HitTest";

        // The enumerators of ECS::UIHitTest, which is what the reflected serializer reads this key back
        // as. Stated as named constants rather than as bare 1 and 2 at the assignment, because an integer
        // in a migration is exactly the thing nobody can check against the enum six months later.
        constexpr int kHitTestChildrenOnly = 1; // the old RaycastTarget = false
        constexpr int kHitTestBlocking     = 2; // the old Interactable  = false

        UIVisibilityMigrationReport report;

        for ( auto& entity : entities )
        {
            const auto layout = entity.Components.get( "UILayout" );
            if ( !layout.has_value() )
                continue;

            const auto fields = layout.value().to_object();
            if ( !fields.has_value() )
            {
                LOG_WARN( "[SceneMigration] entity '{0}': the UILayout payload is {1}, not an object - its "
                          "interaction flags could not be folded into Hit Test",
                          entity.Tag.value_or( "Entity" ), Describe( layout.value() ) );
                continue;
            }

            // Rebuilt rather than edited in place, like every step above it: rfl::Object is an ordered
            // vector of pairs with no erase and no rename, and copying every key except the two states the
            // intent more plainly than an index dance. The new key is appended once, after the loop, so
            // that two old keys can never produce two of it.
            rfl::Generic::Object kept;
            int                  dropped = 0;
            std::optional<int>   hitTest;

            for ( const auto& [key, value] : fields.value() )
            {
                const bool isRaycast = ( key == kRaycastTargetKey );
                if ( key != kInteractableKey && !isRaycast )
                {
                    kept[key] = value;
                    continue;
                }

                ++dropped;

                const auto flag = value.to_bool();
                if ( !flag.has_value() )
                {
                    // Neither true nor false: there is no behaviour to carry, and inventing one would be
                    // the silent substitution DC 1.4 forbids. The key goes (the field it named is gone
                    // either way) and the element keeps UIHitTest::All.
                    report.BrokenNames.push_back( entity.Tag.value_or( "Entity" ) + "." + key );
                    LOG_WARN( "[SceneMigration] entity '{0}': UILayout.{1} is {2}, not a boolean - the "
                              "element keeps the default Hit Test (All)",
                              entity.Tag.value_or( "Entity" ), key, Describe( value ) );
                    continue;
                }
                if ( flag.value() )
                    continue; // the default on both flags; an absent key IS UIHitTest::All

                // A transparent element cannot be pressed anyway, so RaycastTarget's answer subsumes
                // Interactable's whenever both are off. Writing Blocking there would ADD blocking that the
                // file never asked for.
                if ( isRaycast )
                    hitTest = kHitTestChildrenOnly;
                else if ( !hitTest.has_value() )
                    hitTest = kHitTestBlocking;
            }

            if ( dropped == 0 )
                continue; // already raised, or authored after the move - leave the tree byte-identical

            if ( hitTest.has_value() )
            {
                kept[kHitTestKey] = rfl::Generic( hitTest.value() );
                report.HitTestSet += 1;
            }

            entity.Components["UILayout"] = rfl::Generic( std::move( kept ) );
            report.Entities += 1;
            report.FlagsDropped += dropped;
        }

        return report;
    }

    SSRUnitsMigrationReport MigrateSSRUnitsV10ToV11( std::optional<rfl::Generic>& settings )
    {
        SSRUnitsMigrationReport report;

        if ( !settings.has_value() )
            return report; // a scene with no Settings block states no SSR distance; nothing to restate

        const auto fields = settings.value().to_object();
        if ( !fields.has_value() )
        {
            LOG_WARN( "[SceneMigration] the Settings block is {0}, not an object - the scene's SSR max "
                      "distance could not be restated in centimetres and stays as it is",
                      Describe( settings.value() ) );
            return report;
        }

        rfl::Generic::Object kept;
        for ( const auto& [key, value] : fields.value() )
        {
            if ( key != "SSRMaxDistance" )
            {
                kept[key] = value;
                continue;
            }

            // AsFiniteNumber, not to_double: a hand-edited scene writes "SSRMaxDistance":40, which
            // reflect-cpp parses as int64 and to_double() then refuses - the same trap the sky and
            // gravity migrations document.
            const auto number = AsFiniteNumber( value );
            if ( !number.has_value() )
            {
                LOG_WARN( "[SceneMigration] Settings.SSRMaxDistance is {0}, not a finite number - it is "
                          "left exactly as it is and the scene still states no usable SSR distance",
                          Describe( value ) );
                kept[key] = value;
                continue;
            }

            // Every value a v10 file can carry was authored under the metre-scale slider, so inside the
            // version gate x100 is a restatement of the author's magnitude, not a guess (see the header
            // on why this step scales where the gravity step recognises).
            report.Found  = true;
            report.Scaled = true;
            report.Before = static_cast<float>( number.value() );
            report.After  = static_cast<float>( number.value() * 100.0 );
            kept[key]     = number.value() * 100.0;
        }

        settings = rfl::Generic( kept );
        return report;
    }

    DebugViewMigrationReport MigrateDebugViewV12ToV13( std::optional<rfl::Generic>& settings )
    {
        DebugViewMigrationReport report;

        if ( !settings.has_value() )
            return report; // no Settings block states no debug flags; nothing to remove

        const auto fields = settings.value().to_object();
        if ( !fields.has_value() )
        {
            LOG_WARN( "[SceneMigration] the Settings block is {0}, not an object - the viewport debug "
                      "flags could not be removed and stay in the file",
                      Describe( settings.value() ) );
            return report;
        }

        rfl::Generic::Object kept;
        for ( const auto& [key, value] : fields.value() )
        {
            const bool isDebugKey = std::any_of( std::begin( kDebugViewKeys ), std::end( kDebugViewKeys ),
                                                 [&key]( const char* name ) { return key == name; } );
            if ( !isDebugKey )
            {
                kept[key] = value;
                continue;
            }

            // NAMED WITH ITS VALUE, not counted. `ShowColliders=true` in the log is what tells the person
            // running this why their green wireframes are gone; "10 keys removed" does not.
            ++report.KeysRemoved;
            report.RemovedNames.push_back( key + "=" + Describe( value ) );
        }

        settings = rfl::Generic( kept );
        return report;
    }

    ScriptRootMigrationReport MigrateScriptRootV15ToV16( std::vector<Assets::EntityData>& entities )
    {
        static constexpr const char* kComponentKey = "Script";
        static constexpr const char* kSlotsKey     = "Scripts";
        static constexpr const char* kOldKey       = "Path";      // the rooted spelling, up to v15
        static constexpr const char* kNewKey       = "ScriptKey"; // the root-tagged key, from v16

        ScriptRootMigrationReport report;

        // Composed ONCE, from the runtime's own table, so this function and
        // Common::AssetHandle::StableKeyForPath cannot come to spell the prefix differently — the whole
        // point of the migration is that what it writes is what the engine reads back.
        const std::string prefix = std::string( Common::AssetHandle::AssetsTag() ) + ':';

        for ( auto& entity : entities )
        {
            const std::string tag     = entity.Tag.value_or( "Entity" );
            const auto        payload = entity.Components.get( kComponentKey );
            if ( !payload.has_value() )
                continue;

            const auto fields = payload.value().to_object();
            if ( !fields.has_value() )
            {
                LOG_WARN( "[SceneMigration] entity '{0}': the Script payload is {1}, not an object - its "
                          "script references could not be root-tagged and stay as they are",
                          tag, Describe( payload.value() ) );
                continue;
            }

            const auto named = fields.value().get( kSlotsKey );
            if ( !named.has_value() )
                continue; // a Script component with no slot list - nothing to rewrite

            const auto rows = named.value().to_array();
            if ( !rows.has_value() )
            {
                LOG_WARN( "[SceneMigration] entity '{0}': Script.Scripts is {1}, not an array - the script "
                          "references in it are left exactly as they are",
                          tag, Describe( named.value() ) );
                continue;
            }

            rfl::Generic::Array slots;
            int                 rewrittenHere = 0; // slots whose reference was re-spelled as a key
            int                 carriedHere   = 0; // slots the census could not place, carried verbatim

            for ( const auto& row : rows.value() )
            {
                const auto slotFields = row.to_object();
                if ( !slotFields.has_value() )
                {
                    LOG_WARN( "[SceneMigration] entity '{0}': a Script slot is {1}, not an object - it is "
                              "left exactly as it is",
                              tag, Describe( row ) );
                    slots.push_back( row );
                    continue;
                }

                // Rebuilt rather than edited in place, like every step above: rfl::Object is an ordered
                // vector of pairs with no erase and no rename, so the rename IS a rebuild. Order is
                // otherwise preserved, which keeps an already-raised file byte-identical.
                rfl::Generic::Object kept;
                bool                 sawOldKey = false;

                for ( const auto& [key, value] : slotFields.value() )
                {
                    if ( key != kOldKey )
                    {
                        kept[key] = value;
                        continue;
                    }

                    sawOldKey = true;

                    const auto text = value.to_string();
                    if ( !text.has_value() )
                    {
                        LOG_WARN( "[SceneMigration] entity '{0}': a Script slot's {1} is {2}, not a string "
                                  "- it is carried over under {3} unchanged and still names no script",
                                  tag, kOldKey, Describe( value ), kNewKey );
                        report.UnrootedNames.push_back( tag + " > Script." + kOldKey + " = " + Describe( value ) );
                        ++carriedHere;
                        kept[kNewKey] = value;
                        continue;
                    }

                    if ( text.value().empty() )
                    {
                        // An empty slot names nothing. It must stay EMPTY rather than become a bare
                        // "assets:" — ScriptSystem tests the reference for emptiness to decide whether
                        // the slot runs at all, and a tag with nothing after it would make every unfilled
                        // slot start trying to load the assets root.
                        kept[kNewKey] = value;
                        report.Empty += 1;
                        rewrittenHere += 1;
                        continue;
                    }

                    // The root is read out of the STORED PATH, never out of a root this function was
                    // handed — see the header for why the v7 -> v8 step's parameter cannot work here.
                    const auto root = Common::Constants::Path::RootForContentPath(
                         Common::Constants::Path::ContentDir::Script, text.value() );
                    const std::string relative = root ? StripRoot( text.value(), *root ) : std::string();
                    if ( relative.empty() )
                    {
                        // Not under a `Scripts/` folder, so the census has nothing to say about it.
                        // Carried across verbatim: PathForStableKey returns an untagged string unchanged,
                        // so the slot behaves exactly as it did — and it is NAMED, because "exactly as it
                        // did" includes not resolving in a packaged game.
                        report.UnrootedNames.push_back( tag + " > Script." + kOldKey + " = " + text.value() );
                        ++carriedHere;
                        LOG_WARN( "[SceneMigration] entity '{0}': the script '{1}' does not lie under a "
                                  "'{2}' folder, so there is no content root to tag it with - it is "
                                  "carried over unchanged and will not resolve in a packaged game",
                                  tag, text.value(), "Scripts" );
                        kept[kNewKey] = value;
                        continue;
                    }

                    kept[kNewKey] = rfl::Generic( prefix + relative );
                    rewrittenHere += 1;
                }

                if ( !sawOldKey )
                {
                    slots.push_back( row ); // already raised, or a slot that never named a script
                    continue;
                }
                slots.push_back( rfl::Generic( std::move( kept ) ) );
            }

            // Counted PER ENTITY and not off the report's own list, which accumulates across the whole
            // scene: a single unplaceable slot early in the file would otherwise make every later entity
            // look touched and get rewritten, and a rewrite that changes nothing is exactly the thing a
            // second run must not do.
            if ( rewrittenHere == 0 && carriedHere == 0 )
                continue; // already raised, or no slot named a script - leave the tree byte-identical

            rfl::Generic::Object component;
            for ( const auto& [key, value] : fields.value() )
                component[key] = ( key == kSlotsKey ) ? rfl::Generic( slots ) : value;

            entity.Components[kComponentKey] = rfl::Generic( std::move( component ) );
            report.Entities += 1;
            report.Slots += rewrittenHere;
        }

        return report;
    }

    ServiceAssetRootMigrationReport MigrateServiceAssetRootV16ToV17( std::vector<Assets::EntityData>& entities,
                                                                     const std::filesystem::path&     assetsRoot )
    {
        // WHERE a reference to a service-registry asset can sit in a .desce. Four rows because the same
        // kind of value reaches the file by two routes - one manual serializer and three reflected
        // AssetHandle slots - and a step that knew only one of them would leave the other behind, which
        // is this project's most repeated defect shape.
        struct Site
        {
            const char* Component;
            const char* OldKey; // the key a v16 file states
            const char* NewKey; // what it is called from v17 on; equal to OldKey where nothing renames
        };
        static constexpr auto kSites = std::to_array<Site>( {
             { "Text", "FontPath", "Font" }, // renamed WITH the value: it is not a path any more
             { "UIText", "Font", "Font" },
             { "UIIcon", "Icon", "Icon" },
             { "UIPanel", "Video", "Video" },
        } );

        ServiceAssetRootMigrationReport report;

        const std::string assetsPrefix = std::string( Common::AssetHandle::AssetsTag() ) + ':';
        const std::string enginePrefix = std::string( Common::AssetHandle::EngineTag() ) + ':';

        const std::vector<std::string> assetsParts = PathComponents( assetsRoot );
        // The engine tree is a compile-time constant and never remapped, so it is read straight from the
        // census rather than passed in - there is no project state in it to disagree with.
        const std::vector<std::string> engineParts = PathComponents( Common::Constants::Path::RESOURCE_PATH );

        for ( auto& entity : entities )
        {
            const std::string tag        = entity.Tag.value_or( "Entity" );
            bool              touchedAny = false;

            for ( const Site& site : kSites )
            {
                const auto payload = entity.Components.get( site.Component );
                if ( !payload.has_value() )
                    continue;

                const auto fields = payload.value().to_object();
                if ( !fields.has_value() )
                {
                    LOG_WARN( "[SceneMigration] entity '{0}': the {1} payload is {2}, not an object - the "
                              "{3} reference in it could not be root-tagged and stays as it is",
                              tag, site.Component, Describe( payload.value() ), site.OldKey );
                    continue;
                }

                const auto named = fields.value().get( site.OldKey );
                if ( !named.has_value() )
                    continue; // this component names no such reference - tree untouched

                // THE OUTCOME IS DECIDED BEFORE ANYTHING IS WRITTEN, which is what makes the step
                // idempotent on the three sites whose key name does NOT change. A rename always has to
                // happen; a re-spelling only happens when a root can actually place the value. Everything
                // else - a value some earlier pass already tagged, an empty slot, a value that is not a
                // string, a file under neither root - leaves the tree byte-identical, so a second pass
                // over the same tree writes nothing at all.
                const bool renames = std::string_view( site.OldKey ) != site.NewKey;
                const auto text    = named.value().to_string();

                std::optional<std::string> respelled; // set only when a root could place the value
                // `bool unplaceable` stood here, assigned nowhere and read nowhere. It compiled unnoticed
                // because this TU was built only by the tool and by the migration suites, whose warning
                // settings let it pass; the Editor links the file since Г26 (CrashRecovery::
                // MigrateAutosaves) and the Editor's build reported it on the first compile. The
                // unplaceable case is already carried by report.UnrootedNames, which is what the report
                // actually reads.

                if ( !text.has_value() )
                {
                    LOG_WARN( "[SceneMigration] entity '{0}': {1}.{2} is {3}, not a string - it names "
                              "nothing and is left exactly as it is",
                              tag, site.Component, site.OldKey, Describe( named.value() ) );
                    report.UnrootedNames.push_back( std::string( tag ) + " > " + site.Component + "." +
                                                    site.OldKey + " = " + Describe( named.value() ) );
                }
                else if ( text.value().empty() )
                {
                    // An empty slot names nothing and must stay empty rather than become a bare tag: the
                    // read side turns any non-empty string into a registration attempt, so "engine:"
                    // would make every unfilled slot try to register the resource root itself.
                }
                else if ( Common::AssetHandle::IsProjectRelativeKey( text.value() ) )
                {
                    // Already a key - an earlier pass reached this tree.
                }
                else
                {
                    const std::vector<std::string> parts    = PathComponents( text.value() );
                    const RootMatch                inAssets = MatchRoot( parts, assetsParts );
                    const RootMatch                inEngine = MatchRoot( parts, engineParts );

                    if ( inAssets.Run == 0 && inEngine.Run == 0 )
                    {
                        // Under neither root, so there is nothing to tag it with. Carried unchanged -
                        // PathForStableKey hands an untagged string back verbatim, so the slot keeps
                        // exactly the behaviour it had - and NAMED, because "exactly the behaviour it
                        // had" includes not resolving in a packaged game (DC 1.4).
                        LOG_WARN( "[SceneMigration] entity '{0}': '{1}' lies under neither the assets root "
                                  "nor the engine resource tree, so there is no content root to tag it "
                                  "with - it is carried over unchanged and will not resolve in a packaged "
                                  "game",
                                  tag, text.value() );
                        report.UnrootedNames.push_back( std::string( tag ) + " > " + site.Component + "." +
                                                        site.OldKey + " = " + text.value() );
                    }
                    else
                    {
                        // The root that matched MORE OF ITSELF wins. On a tie the project's own root
                        // wins: it is the more specific answer about a file lying under both, and the
                        // two can only tie at equal specificity.
                        const bool        assetsWins = inAssets.Run >= inEngine.Run;
                        const std::size_t matched    = assetsWins ? inAssets.End : inEngine.End;

                        std::string relative;
                        for ( std::size_t i = matched; i < parts.size(); ++i )
                        {
                            if ( !relative.empty() )
                                relative += '/';
                            relative += parts[i];
                        }
                        respelled = ( assetsWins ? assetsPrefix : enginePrefix ) + relative;
                    }
                }

                if ( !renames && !respelled.has_value() )
                    continue; // nothing to write - leave the payload byte-identical

                // Rebuilt rather than edited in place, like every step above: rfl::Object is an ordered
                // vector of pairs with no erase and no rename, so a rename IS a rebuild.
                // The replacement is resolved ONCE, before the rebuild: inside the loop the guard beside
                // the dereference is the same statement, but a reader (and any analyser) has to carry that
                // across a back edge to see it, and the value does not depend on the iteration.
                std::optional<rfl::Generic> renamed;
                if ( respelled.has_value() )
                {
                    renamed = rfl::Generic( *respelled );
                }

                rfl::Generic::Object kept;
                for ( const auto& [key, value] : fields.value() )
                {
                    if ( key != site.OldKey )
                        kept[key] = value;
                    else
                        kept[site.NewKey] = renamed.value_or( value );
                }

                if ( respelled.has_value() )
                    report.Refs += 1;
                else if ( text.has_value() && text.value().empty() )
                    report.Empty += 1;

                entity.Components[site.Component] = rfl::Generic( std::move( kept ) );
                touchedAny                        = true;
            }

            if ( touchedAny )
                report.Entities += 1;
        }

        return report;
    }

    TextKeySigilMigrationReport MigrateTextKeySigilV18ToV19( std::vector<Assets::EntityData>& entities )
    {
        // WHERE AN AUTHORED, READER-FACING STRING CAN SIT IN A .desce. Four rows, and the set is the
        // argument: these are exactly the strings the canvas and the world-text system now put through
        // Localization::Resolve, so they are exactly the strings whose leading '#' changed meaning.
        // `UIInputField.Text` is absent on purpose — it is the player's own text, nothing resolves it,
        // and escaping it would put a hash into somebody's words.
        struct Site
        {
            const char* Component;
            const char* Key;
            bool        SemicolonList; // UIDropdown.Options is many strings in one field
        };
        static constexpr auto kSites = std::to_array<Site>( {
             { "UIText", "Text", false },
             { "UIInputField", "Placeholder", false },
             { "UIDropdown", "Options", true },
             { "Text", "Text", false },
        } );

        // Only a LEADING hash. A hash anywhere else is just a hash — `[color=#FF7A33]` is a rich-text
        // colour and this repository's main menu is full of them.
        //
        // A v18 STRING BEGINNING "##" IS ESCAPED TOO, to "###", and that is not an oversight: the resolver
        // strips exactly ONE leading hash, so "###" is the only spelling that still draws "##". This is
        // what makes the step non-idempotent and therefore gated — see the header.
        const auto escape = []( const std::string& text )
        { return ( !text.empty() && text.front() == '#' ) ? "#" + text : text; };

        TextKeySigilMigrationReport report;

        for ( auto& entity : entities )
        {
            const std::string tag        = entity.Tag.value_or( "Entity" );
            bool              touchedAny = false;

            for ( const Site& site : kSites )
            {
                const auto payload = entity.Components.get( site.Component );
                if ( !payload.has_value() )
                    continue;

                const auto fields = payload.value().to_object();
                if ( !fields.has_value() )
                {
                    LOG_WARN( "[SceneMigration] entity '{0}': the {1} payload is {2}, not an object - the "
                              "{3} string in it could not be escaped and stays as it is",
                              tag, site.Component, Describe( payload.value() ), site.Key );
                    continue;
                }

                const auto named = fields.value().get( site.Key );
                if ( !named.has_value() )
                    continue; // this component states no such string - tree untouched

                const auto text = named.value().to_string();
                if ( !text.has_value() )
                {
                    LOG_WARN( "[SceneMigration] entity '{0}': {1}.{2} is {3}, not a string - it is left "
                              "exactly as it is",
                              tag, site.Component, site.Key, Describe( named.value() ) );
                    continue;
                }

                // Decided before anything is written, which is what makes the step idempotent: a payload
                // with nothing to escape leaves the tree byte-identical and a second run finds nothing.
                std::string rewritten;
                if ( site.SemicolonList )
                {
                    // Each item on its own, because each item is resolved on its own. The separator is not
                    // part of any of them, so an empty item stays empty and the shape of the list is kept
                    // exactly — including a trailing separator, which SplitOptions ignores and a rewrite
                    // that "tidied" it would change.
                    std::string item;
                    for ( const char c : text.value() )
                    {
                        if ( c == ';' )
                        {
                            rewritten += escape( item ) + ';';
                            item.clear();
                        }
                        else
                        {
                            item += c;
                        }
                    }
                    rewritten += escape( item );
                }
                else
                {
                    rewritten = escape( text.value() );
                }

                if ( rewritten == text.value() )
                    continue;

                // Rebuilt rather than edited in place, like every step above: rfl::Object is an ordered
                // vector of pairs with no in-place assignment through its iterators.
                rfl::Generic::Object kept;
                for ( const auto& [key, value] : fields.value() )
                    kept[key] = ( key == site.Key ) ? rfl::Generic( rewritten ) : value;

                entity.Components[site.Component] = rfl::Generic( std::move( kept ) );
                report.Escaped += 1;
                report.EscapedNames.push_back( std::string( tag ) + " > " + site.Component + "." + site.Key +
                                               " = " + rewritten );
                touchedAny = true;
            }

            if ( touchedAny )
                report.Entities += 1;
        }

        return report;
    }

    AnimGraphMigrationReport MigrateAnimGraphV20ToV21( std::vector<Assets::EntityData>& entities )
    {
        // A GRAPH NAME IS NOT A FILENAME until this says so. An artist types anything into the graph's
        // Name field, and a '/' in it would make the migration write outside AnimGraphs/ — which is the
        // one way a pure-looking step can reach a file nobody asked it to touch. Everything that is not a
        // letter, a digit, '_' or '-' becomes '_'; the mapping is many-to-one, and the CALLER is what
        // notices two different graphs landing on one name (see the header).
        const auto asFilename = []( const std::string& name )
        {
            std::string out;
            out.reserve( name.size() );
            for ( const char c : name )
            {
                const bool safe = ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) ||
                                  ( c >= '0' && c <= '9' ) || c == '_' || c == '-';
                out.push_back( safe ? c : '_' );
            }
            return out;
        };

        AnimGraphMigrationReport report;

        for ( auto& entity : entities )
        {
            const std::string tag = entity.Tag.value_or( "Entity" );

            const auto payload = entity.Components.get( "Animation" );
            if ( !payload.has_value() )
                continue;

            const auto fields = payload.value().to_object();
            if ( !fields.has_value() )
            {
                LOG_WARN( "[SceneMigration] entity '{0}': the Animation payload is {1}, not an object - its "
                          "state machine could not be moved and stays as it is",
                          tag, Describe( payload.value() ) );
                continue;
            }

            const auto blob = fields.value().get( "GraphJson" );
            if ( !blob.has_value() )
                continue; // already converted, or never had a graph - tree untouched, so this is idempotent

            const auto json = blob.value().to_string();
            if ( !json.has_value() )
            {
                // Present and not a string. LEFT IN PLACE, named, and counted as a refusal: dropping a key
                // whose content we could not read is dropping an artist's work to make a number go up.
                report.Rejected += 1;
                report.RejectedNames.push_back( tag + " > Animation.GraphJson is " + Describe( blob.value() ) +
                                                ", not a string" );
                continue;
            }

            // Rebuilt rather than edited in place, like every step above: rfl::Object is an ordered vector
            // of pairs with no in-place assignment through its iterators.
            const auto withoutBlob = [&fields]( std::optional<std::string> graphPath )
            {
                rfl::Generic::Object kept;
                for ( const auto& [key, value] : fields.value() )
                {
                    if ( key == "GraphJson" )
                        continue;
                    kept[key] = value;
                }
                if ( graphPath.has_value() )
                    kept["Graph"] = rfl::Generic( *graphPath );
                return kept;
            };

            if ( json->empty() )
            {
                // The common case by count: an entity with an Animation component and no state machine.
                // The key simply goes; no file, no reference, and the payload says "no graph" by absence.
                entity.Components["Animation"] = rfl::Generic( withoutBlob( std::nullopt ) );
                report.Empty += 1;
                continue;
            }

            auto parsed = Animation::Graph::Deserialize( *json );
            if ( !parsed )
            {
                // LEFT IN PLACE. A blob that will not parse is a state machine somebody authored and this
                // tool cannot read; writing it to a file unread would produce a `.danimgraph` the engine
                // then refuses, and dropping it would lose the work outright. Named, so a fixed tool can
                // have another go at the same file.
                report.Rejected += 1;
                report.RejectedNames.push_back( tag +
                                                " > Animation.GraphJson is not a graph: " + parsed.GetError() );
                continue;
            }

            const Animation::Graph::AnimGraph graph = parsed.ExtractValue();

            // NAMED AFTER THE GRAPH, which is what makes two characters that carried byte-identical blobs
            // - exactly what copying a character produced - converge on ONE file and genuinely share it.
            // A graph with no Name of its own falls back to the ENTITY's tag, because "" is not a filename
            // and inventing one constant for every such blob would collide them all into a single file.
            std::string stem = asFilename( graph.Name );
            if ( stem.empty() )
                stem = asFilename( tag );
            if ( stem.empty() )
                stem = "Graph";

            // THE CENSUS ROW AND THE FORMAT'S OWN CONSTANT, never a literal: the folder this tool writes
            // into and the folder the content scan reads from are one statement (Constants.hpp), and a
            // second spelling of either here is how a migrated scene comes to name a file nothing loads.
            static constexpr auto kAnimGraphDir = Common::Constants::Path::CONTENT_DIRS[static_cast<std::size_t>(
                 Common::Constants::Path::ContentDir::AnimGraph )];
            const std::string     relative =
                 std::string( kAnimGraphDir.Rel ) + stem + std::string( Animation::Graph::kAnimGraphExtension );

            // RE-SERIALIZED, not copied through. The blob was written by whatever build saved the scene;
            // writing the canonical form means two entities whose graphs differ only in key order or in a
            // field one build omitted produce the SAME bytes, which is what lets the caller collapse them
            // onto one file instead of discovering a spurious conflict.
            report.Graphs.push_back( AnimGraphFile{ relative, Animation::Graph::Serialize( graph ) } );
            entity.Components["Animation"] = rfl::Generic( withoutBlob( relative ) );
            report.Entities += 1;
        }

        return report;
    }

    namespace
    {
        // THE v21 SHAPE OF A STATIC MESH'S EDITED GEOMETRY, known here and nowhere else (DEV_CONTRACT §4.3:
        // the runtime knows nothing about the old format). Exactly what ComponentRegistry wrote before v22.
        struct LegacyVertexV21
        {
            glm::vec3 Position;
            glm::vec3 Normal;
            glm::vec2 TexCoord;
        };
        struct LegacyStaticMeshGeometryV21
        {
            std::optional<std::vector<LegacyVertexV21>> CustomVertices;
            std::optional<std::vector<uint32_t>>        CustomIndices;
        };
    } // namespace

    EditMeshMigrationReport MigrateEditMeshV21ToV22( std::vector<Assets::EntityData>& entities )
    {
        EditMeshMigrationReport report;

        for ( auto& entity : entities )
        {
            const std::string tag = entity.Tag.value_or( "Entity" );

            const auto payload = entity.Components.get( "StaticMesh" );
            if ( !payload.has_value() )
                continue;
            const auto fields = payload.value().to_object();
            if ( !fields.has_value() )
            {
                LOG_WARN( "[SceneMigration] entity '{0}': the StaticMesh payload is {1}, not an object - an "
                          "edited mesh in it could not be converted and stays as it is",
                          tag, Describe( payload.value() ) );
                continue;
            }
            const bool hasVertices = fields.value().get( "CustomVertices" ).has_value();
            const bool hasIndices  = fields.value().get( "CustomIndices" ).has_value();
            if ( !hasVertices && !hasIndices )
                continue; // no edited mesh, or already converted - tree untouched, so this is idempotent

            const auto reject = [&]( const std::string& why )
            {
                report.Rejected += 1;
                report.RejectedNames.push_back( tag + " > StaticMesh: " + why );
            };

            if ( hasVertices != hasIndices )
            {
                reject( std::string( "states " ) + ( hasVertices ? "CustomVertices" : "CustomIndices" ) +
                        " without the other" );
                continue;
            }

            const auto legacy = rfl::json::read<LegacyStaticMeshGeometryV21, rfl::DefaultIfMissing>(
                 rfl::json::write( payload.value() ) );
            if ( !legacy.has_value() || !legacy.value().CustomVertices || !legacy.value().CustomIndices )
            {
                reject( std::string( "CustomVertices/CustomIndices are not the v21 arrays: " ) +
                        ( legacy.has_value() ? "missing after the read" : legacy.error().what() ) );
                continue;
            }
            const auto& vertices = *legacy.value().CustomVertices;
            const auto& indices  = *legacy.value().CustomIndices;
            if ( indices.size() % 3 != 0 )
            {
                reject( std::to_string( indices.size() ) + " CustomIndices is not a whole number of triangles" );
                continue;
            }

            Geometry::RenderMeshData render;
            render.Vertices.reserve( vertices.size() );
            for ( const auto& v : vertices )
            {
                Vertex vertex{};
                vertex.Position = v.Position;
                vertex.Normal   = v.Normal;
                vertex.TexCoord = v.TexCoord;
                render.Vertices.push_back( vertex );
            }
            for ( size_t i = 0; i < indices.size(); i += 3 )
                render.Indices.push_back( { indices[i], indices[i + 1], indices[i + 2] } );

            auto imported = Geometry::FromRenderMesh( render );
            if ( !imported.IsSuccess() )
            {
                reject( imported.GetError() );
                continue;
            }
            Geometry::ImportedEditMesh result = imported.ExtractValue();
            result.Mesh.Attributes().DisableTangents();

            rfl::Generic::Object kept;
            for ( const auto& [key, value] : fields.value() )
            {
                if ( key == "CustomVertices" || key == "CustomIndices" )
                    continue;
                kept[key] = value;
            }
            const auto saved =
                 rfl::json::read<rfl::Generic>( rfl::json::write( Geometry::ToSerialized( result.Mesh ) ) );
            if ( !saved.has_value() )
            {
                reject( std::string( "the converted mesh could not be written: " ) + saved.error().what() );
                continue;
            }
            kept["EditMesh"]                = saved.value();
            entity.Components["StaticMesh"] = rfl::Generic( kept );

            report.Entities += 1;
            std::string line = tag + ": " + std::to_string( vertices.size() ) + " render vertices -> " +
                               std::to_string( result.Mesh.VertexCount() ) + " vertices / " +
                               std::to_string( result.Mesh.TriangleCount() ) + " triangles";
            if ( result.DroppedDegenerate > 0 )
                line += ", " + std::to_string( result.DroppedDegenerate ) + " degenerate dropped";
            if ( result.DroppedDuplicate > 0 )
                line += ", " + std::to_string( result.DroppedDuplicate ) + " duplicate dropped";
            if ( result.DetachedTriangles > 0 )
                line += ", " + std::to_string( result.DetachedTriangles ) + " detached (non-manifold edge)";
            report.ConvertedNames.push_back( std::move( line ) );
        }

        return report;
    }

    RetiredKeysMigrationReport MigrateRetiredKeys( std::optional<rfl::Generic>&     settings,
                                                   std::vector<Assets::EntityData>& entities )
    {
        RetiredKeysMigrationReport report;

        // One block, one pass: rebuild it without the rows that name it. Rebuilt rather than erased in
        // place because rfl::Object is an ordered vector of pairs with no erase(), and rebuilding also
        // keeps the order of everything else — which matters now that a scene read and written back
        // unchanged is expected to be byte-identical.
        const auto strip = [&report]( rfl::Generic::Object fields, const char* block ) -> rfl::Generic::Object
        {
            rfl::Generic::Object kept;
            for ( const auto& [key, value] : fields )
            {
                const RetiredKey* row = nullptr;
                for ( const RetiredKey& candidate : kRetiredKeys )
                    if ( key == candidate.Key && std::string( block ) == candidate.Block )
                    {
                        row = &candidate;
                        break;
                    }

                if ( row == nullptr )
                {
                    kept[key] = value;
                    continue;
                }

                ++report.KeysRemoved;
                report.RemovedNames.push_back( std::string( block ) + "." + key + "=" + Describe( value ) + " (" +
                                               row->Why + ")" );
            }
            return kept;
        };

        if ( settings.has_value() )
        {
            const auto fields = settings.value().to_object();
            if ( !fields.has_value() )
            {
                LOG_WARN( "[SceneMigration] the Settings block is {0}, not an object - the retired keys "
                          "could not be removed and stay in the file",
                          Describe( settings.value() ) );
            }
            else
            {
                settings = rfl::Generic( strip( fields.value(), "Settings" ) );
            }
        }

        // Component payloads. A row whose Block is not "Settings" names a field inside that component on
        // every entity carrying one; with today's single row this loop finds nothing, and it exists so
        // that the NEXT retirement does not have to invent a second mechanism.
        for ( Assets::EntityData& entity : entities )
        {
            for ( const auto& [componentKey, payload] : entity.Components )
            {
                const bool anyRowNamesThisBlock =
                     std::any_of( std::begin( kRetiredKeys ), std::end( kRetiredKeys ),
                                  [&componentKey]( const RetiredKey& row ) { return componentKey == row.Block; } );
                if ( !anyRowNamesThisBlock )
                    continue;

                const auto fields = payload.to_object();
                if ( !fields.has_value() )
                    continue;
                entity.Components[componentKey] = rfl::Generic( strip( fields.value(), componentKey.c_str() ) );
            }
        }

        return report;
    }

    GrassGenerationMigrationReport MigrateGrassGenerationV17ToV18( std::vector<Assets::EntityData>& entities )
    {
        GrassGenerationMigrationReport report;

        // The six keys the procedural generator owned, spelled once. `EnableGrass` is FIRST because it is
        // the one that is read before it is dropped.
        static constexpr std::array kGeneratorKeys = {
             "EnableGrass", "GrassDensity", "GrassHeight", "GrassBladesPerClump", "GrassWidth", "GrassBrightness",
        };

        // The values TerrainLayerMode serializes as. An enum is written by the reflection serializer as
        // its INTEGER (ReflectionSerializer.cpp, FieldType::Enum), so these are the numbers a `.desce`
        // carries and not the names. Stated here rather than reached for through the reflection registry,
        // which is a global this function must not touch.
        constexpr int64_t kLayerModeAuto = 0;
        constexpr int64_t kLayerModeOff  = 2;

        for ( Assets::EntityData& entity : entities )
        {
            const auto terrain = entity.Components.get( "Terrain" );
            if ( !terrain.has_value() )
                continue;

            const auto fields = terrain.value().to_object();
            if ( !fields.has_value() )
            {
                LOG_WARN( "[SceneMigration] entity '{0}': the Terrain payload is {1}, not an object - the "
                          "grass generator's keys could not be removed and stay in the file",
                          entity.Tag.value_or( "Entity" ), Describe( terrain.value() ) );
                continue;
            }

            const auto& in = fields.value();

            // ── 1. Read the carry BEFORE anything is dropped ────────────────────────────────────────
            //
            // A file that already states GrassMode keeps what it states: the authored value wins over one
            // derived from a switch that is going away. A file that states NEITHER key gets no GrassMode
            // written, so the component's own default applies - which is what "this scene never expressed
            // an opinion" has always meant and is not something a migration may decide for it.
            const bool statesMode  = in.get( "GrassMode" ).has_value();
            const auto enableValue = in.get( "EnableGrass" );

            // TWO PLAIN VALUES, NOT AN OPTIONAL. "Is there a mode to carry" and "which mode" are asked in
            // four separate places below; an optional makes the second question a dereference that has to
            // stay in step with the first across a loop body, and the two are cheaper to keep honest apart.
            int64_t carriedMode = kLayerModeOff;
            bool    haveCarried = false;
            if ( !statesMode && enableValue.has_value() )
            {
                // The mode is decided as a VALUE and only then carried, so the line that reports it reads
                // the same object the branches wrote. It used to dereference an optional instead, which is
                // the same number reached through a second question nothing here answers.
                int64_t    mode = kLayerModeOff;
                const auto flag = enableValue.value().to_bool();
                if ( !flag.has_value() )
                {
                    // Present but not a boolean. "Off" is the only reading that cannot invent grass a
                    // scene never asked for, and it is said out loud rather than chosen quietly (DC 1.4).
                    LOG_WARN( "[SceneMigration] entity '{0}': Terrain.EnableGrass is {1}, expected a "
                              "boolean - the grass ground layer is set to Off",
                              entity.Tag.value_or( "Entity" ), Describe( enableValue.value() ) );
                }
                else
                {
                    mode = flag.value() ? kLayerModeAuto : kLayerModeOff;
                }
                carriedMode = mode;
                haveCarried = true;
                report.CarriedNames.push_back( std::string( "Terrain.GrassMode=" ) +
                                               ( mode == kLayerModeAuto ? "Auto" : "Off" ) +
                                               " (was EnableGrass=" + Describe( enableValue.value() ) + ")" );
            }

            // ── 2. Rebuild the payload without the six ───────────────────────────────────────────────
            //
            // Rebuilt rather than erased in place for the same reason MigrateRetiredKeys rebuilds:
            // rfl::Object is an ordered vector of pairs with no erase(), and rebuilding preserves the
            // order of everything else, which is what makes a file read and written back unchanged come
            // out byte-identical.
            rfl::Generic::Object out;
            int                  removedHere = 0;
            for ( const auto& [key, value] : in )
            {
                const bool isGeneratorKey =
                     std::find( kGeneratorKeys.begin(), kGeneratorKeys.end(), key ) != kGeneratorKeys.end();
                if ( !isGeneratorKey )
                {
                    out[key] = value;
                    continue;
                }
                ++removedHere;
                report.RemovedNames.push_back( "Terrain." + key + "=" + Describe( value ) );
            }
            if ( haveCarried )
            {
                out["GrassMode"] = carriedMode;
            }

            if ( removedHere == 0 && !haveCarried )
            {
                continue;
            }

            entity.Components["Terrain"] = rfl::Generic( out );
            ++report.Entities;
            report.KeysRemoved += removedHere;
        }

        return report;
    }

    CloudMaterialMigrationReport MigrateCloudMaterialV11ToV12( std::vector<Assets::EntityData>& entities,
                                                               const std::string&               sceneName )
    {
        // The thirty-three moved keys, spelled ONCE. Components: how many numbers the value carries
        // (1 scalar, 2/3 vector); the material stores every value as a vec4 with the tail zeroed, which
        // is the `.demat` format's own convention for scalars.
        struct MovedValue
        {
            const char* Key;
            int         Components;
        };
        static constexpr auto       kValues     = std::to_array<MovedValue>( {
             { "Coverage", 1 },
             { "CoverageContrast", 1 },
             { "WeatherTileSize", 1 },
             { "Seed", 1 },
             { "PlacementDensity", 1 },
             { "PlacementScatter", 1 },
             { "PlacementSizeVariety", 1 },
             { "PatchTileSize", 1 },
             { "PatchStrength", 1 },
             { "LayoutPatternStrength", 1 },
             { "LayoutMaskStrength", 1 },
             { "LayoutRepeats", 1 },
             { "LayoutRotation", 1 },
             { "LayoutOffset", 2 },
             { "DetailTileSize", 1 },
             { "DetailStrength", 1 },
             { "DensityScale", 1 },
             { "ExtinctionScale", 1 },
             { "ScatteringAlbedo", 1 },
             { "PhaseG", 1 },
             { "PhaseGBackward", 1 },
             { "PhaseBlend", 1 },
             { "AmbientOcclusionStrength", 1 },
             { "MultiScatterOctaves", 1 },
             { "MultiScatterContribution", 1 },
             { "MultiScatterOcclusion", 1 },
             { "MultiScatterEccentricity", 1 },
             { "AmbientScale", 3 },
        } );
        static constexpr std::array kAssets     = { "CloudType1", "CloudType2", "CloudType3", "CloudType4",
                                                    "CloudLayout" };
        static constexpr size_t     kMovedCount = std::size( kValues ) + std::size( kAssets );

        const auto isMovedValue = []( const std::string& key ) -> const MovedValue*
        {
            for ( const MovedValue& v : kValues )
                if ( key == v.Key )
                    return &v;
            return nullptr;
        };
        const auto isMovedAsset = []( const std::string& key )
        {
            for ( const char* a : kAssets )
                if ( key == a )
                    return true;
            return false;
        };

        // A number in whichever of the two spellings JSON parsing gave it. reflect-cpp keeps 1 as an int
        // and 1.0 as a double, and a migration that accepted only one spelling would reject half the
        // hand-edited scenes in the repository for stating "Seed": 1.
        const auto asNumber = []( const rfl::Generic& g ) -> std::optional<double>
        {
            if ( const auto d = g.to_double(); d )
                return *d;
            if ( const auto i = g.to_int(); i )
                return static_cast<double>( *i );
            return std::nullopt;
        };

        CloudMaterialMigrationReport report;

        // The material's file name comes from the SCENE, sanitized exactly as the editor sanitizes an
        // entity name into "M_<...>": the file lands in the shared Materials/ directory and has to say
        // whose sky it is. A second cloud entity in one scene (nothing renders it - the collector takes
        // the lowest id - but a file may carry one) gets a numbered sibling rather than clobbering.
        std::string base;
        for ( const char c : sceneName )
            base += ( std::isalnum( static_cast<unsigned char>( c ) ) || c == '_' || c == '-' ) ? c : '_';
        if ( base.empty() )
            base = "Scene";

        int cloudEntityIndex = 0;

        for ( auto& entity : entities )
        {
            const auto clouds = entity.Components.get( "VolumetricCloud" );
            if ( !clouds.has_value() )
                continue;

            const auto fields = clouds.value().to_object();
            if ( !fields.has_value() )
            {
                LOG_WARN( "[SceneMigration] entity '{0}': the VolumetricCloud payload is {1}, not an "
                          "object - its look could not be moved into a material",
                          entity.Tag.value_or( "Entity" ), Describe( clouds.value() ) );
                continue;
            }

            ++cloudEntityIndex;

            Assets::MaterialData material;
            material.ShaderName = "CloudRaymarch"; // = Graphic::kCloudMaterialShaderName; the migration
                                                   // suite pins the two spellings together

            rfl::Generic::Object kept;
            int                  movedHere = 0;

            for ( const auto& [key, value] : fields.value() )
            {
                if ( const MovedValue* moved = isMovedValue( key ) )
                {
                    glm::vec4 packed( 0.0f );
                    bool      usable = true;

                    if ( moved->Components == 1 )
                    {
                        if ( const auto n = asNumber( value ); n )
                            packed.x = static_cast<float>( *n );
                        else
                            usable = false;
                    }
                    else
                    {
                        const auto arr = value.to_array();
                        usable         = arr.has_value() && arr.value().size() >= size_t( moved->Components );
                        if ( usable )
                        {
                            for ( int i = 0; i < moved->Components; ++i )
                            {
                                if ( const auto n = asNumber( arr.value()[size_t( i )] ); n )
                                    packed[i] = static_cast<float>( *n );
                                else
                                    usable = false;
                            }
                        }
                    }

                    if ( !usable )
                    {
                        // The key still LEAVES the payload - the runtime knows nothing about the old
                        // format (§4.3) - but the value it carried is named, out loud, because a number
                        // somebody authored did not reach the material and will read as the default.
                        report.Rejected += 1;
                        report.RejectedNames.push_back( key );
                        LOG_WARN( "[SceneMigration] entity '{0}': VolumetricCloud.{1} is {2} and could "
                                  "not be carried into the cloud material - the schema default stands",
                                  entity.Tag.value_or( "Entity" ), key, Describe( value ) );
                        ++movedHere; // the payload still changed shape
                        continue;
                    }

                    material.Params.push_back( { key, packed } );
                    report.ValuesMoved += 1;
                    ++movedHere;
                    continue;
                }

                if ( isMovedAsset( key ) )
                {
                    const auto text = value.to_string();
                    if ( !text.has_value() )
                    {
                        report.Rejected += 1;
                        report.RejectedNames.push_back( key );
                        LOG_WARN( "[SceneMigration] entity '{0}': VolumetricCloud.{1} is {2}, not a "
                                  "path - the slot arrives empty in the material",
                                  entity.Tag.value_or( "Entity" ), key, Describe( value ) );
                        ++movedHere;
                        continue;
                    }
                    const std::string& path = text.value();
                    if ( path.empty() )
                    {
                        // An empty slot is the shipped state of most scenes; it needs no entry - an
                        // absent name resolves to the null handle exactly as an empty string did.
                        ++movedHere;
                        continue;
                    }
                    if ( path.front() == '/' || path.find( ':' ) != std::string::npos )
                    {
                        // A pure function cannot resolve an absolute path against a root it must not
                        // read; and no scene in the repository carries one here (the cloud branches
                        // were relative from the day they existed). Named rather than guessed at.
                        report.Rejected += 1;
                        report.RejectedNames.push_back( key );
                        LOG_WARN( "[SceneMigration] entity '{0}': VolumetricCloud.{1} names a "
                                  "non-relative path '{2}' - the slot arrives empty in the material",
                                  entity.Tag.value_or( "Entity" ), key, path );
                        ++movedHere;
                        continue;
                    }

                    // THE SAME HANDLE THE RUNTIME MINTS for this file: FNV over "assets:<relative>",
                    // which is AssetHandle::FromCookedPath's own key for a content asset
                    // (Common/Core/AssetHandle.hpp, StableKeyForPath). Deriving it here rather than
                    // resolving through a filesystem is what keeps this function pure - and the
                    // migration suite pins the two derivations together.
                    const auto handle = ::Common::AssetHandle::FromKey( "assets:" + path );
                    material.Textures.push_back( { key, static_cast<uint64_t>( handle ) } );
                    report.AssetsMoved += 1;
                    ++movedHere;
                    continue;
                }

                kept[key] = value;
            }

            if ( movedHere == 0 )
            {
                // Either already raised (a bespoke or shared "Material" is already present - untouched,
                // idempotent), or a layer that never authored a single look field. D-37: the second case
                // is NOT left byte-identical any more - it is pointed at the shared default so that every
                // migrated layer names some material and "the look lives in the material" holds without
                // an empty-slot exception. `kept` is an exact copy of the original payload here (nothing
                // matched a moved key), so checking IT for "Material" is checking the payload as it was.
                bool alreadyNamed = false;
                for ( const auto& [key, value] : kept )
                    alreadyNamed = alreadyNamed || key == "Material";

                if ( !alreadyNamed )
                {
                    kept["Material"] = rfl::Generic( std::string( kDefaultCloudMaterialRelativePath ) );
                    entity.Components["VolumetricCloud"] = rfl::Generic( std::move( kept ) );
                    report.Entities += 1;
                    report.DefaultsAssigned += 1;
                    report.Defaulted += static_cast<int>( kMovedCount );
                }
                continue;
            }

            const std::string suffix  = cloudEntityIndex > 1 ? "_" + std::to_string( cloudEntityIndex ) : "";
            const std::string relPath = "Materials/M_" + base + "_Clouds" + suffix + ".demat";

            // DERIVED, NOT GENERATED: the MaterialId is the FNV of the file's own relative path, so two
            // runs of this migration produce byte-identical files and byte-identical scenes - which is
            // what lets the suite pin the output and the repository diff show only real change.
            material.MaterialId = ::Common::UUID(
                 static_cast<uint64_t>( ::Common::AssetHandle::FromKey( "cloudmat:" + relPath ) ) );

            kept["Material"] = rfl::Generic( relPath );

            // RAISED THE REST OF THE WAY BEFORE IT IS WRITTEN. This step carries the v11 key `CloudLayout`
            // verbatim, because that is what a v11 scene states; O-4 split that input in two, and a file
            // produced here naming a slot the shader no longer declares would be born needing a second
            // pass. One statement of the split, called from both entry points.
            report.AssetsMoved += MigrateCloudMaterialLayoutInputs( material ).Split;

            // AND THE ALBEDO, because this function has just WRITTEN a scalar one: the scene field it moved
            // was a float, so the value it produced is (x, 0, 0, 0) — exactly the shape a `.demat` authored
            // before the colour landed carries. Raising it here rather than leaving it to the material pass
            // is what makes a v11 scene come out of ONE run with a sky it can render, instead of needing the
            // tool twice.
            MigrateCloudMaterialAlbedoToColour( material );

            report.Materials.push_back( { relPath, rfl::json::write( material ) } );

            entity.Components["VolumetricCloud"] = rfl::Generic( std::move( kept ) );
            report.Entities += 1;
            report.Defaulted += static_cast<int>( kMovedCount ) - movedHere;
        }

        return report;
    }

    CloudMaterialLayoutReport MigrateCloudMaterialLayoutInputs( Assets::MaterialData& material )
    {
        CloudMaterialLayoutReport report;

        // THE NAMES, ONCE. `CloudLayout` is what every `.demat` written before O-4 states; the two below
        // are what CloudRaymarch.shader declares now, and CloudMaterialSchema pins that census.
        constexpr const char* kOld     = "CloudLayout";
        constexpr const char* kPattern = "LayoutPattern";
        constexpr const char* kMask    = "LayoutMask";

        const auto names = [&material]( const char* wanted )
        {
            for ( const auto& texture : material.Textures )
                if ( texture.Name == wanted )
                    return true;
            return false;
        };

        for ( size_t i = 0; i < material.Textures.size(); )
        {
            if ( material.Textures[i].Name != kOld )
            {
                ++i;
                continue;
            }

            const uint64_t handle = material.Textures[i].TextureHandle;
            material.Textures.erase( material.Textures.begin() + static_cast<ptrdiff_t>( i ) );

            // BOTH SLOTS TAKE THE SAME FILE, and that is what makes the migration a rename rather than a
            // change of sky. One `.dclayout` carries both tables and the bake read both out of it, so
            // pointing the two new inputs at it reproduces the old frame exactly. Guarded against a
            // material that somehow already states one of them, so a second run cannot duplicate a name.
            if ( !names( kPattern ) )
                material.Textures.push_back( { kPattern, handle } );
            if ( !names( kMask ) )
                material.Textures.push_back( { kMask, handle } );

            report.Split += 1;
        }

        return report;
    }

    CloudMaterialAlbedoReport MigrateCloudMaterialAlbedoToColour( Assets::MaterialData& material )
    {
        CloudMaterialAlbedoReport report;

        // THE NAME, ONCE. It did not change — only how many of its four stored components the shader reads.
        constexpr const char* kAlbedo = "ScatteringAlbedo";

        for ( auto& param : material.Params )
        {
            if ( param.Name != kAlbedo )
                continue;

            // THE TRIGGER IS THE SHAPE OF THE STORED VALUE and nothing else. A scalar was written as
            // (x, 0, 0, 0); a colour is written with all three set. So a non-zero green or blue means this
            // file has already been raised — or was authored as a colour — and is left exactly alone.
            if ( param.Value.y != 0.0f || param.Value.z != 0.0f )
                continue;

            // BROADCAST, which reproduces the old sky EXACTLY: the scalar albedo multiplied a radiance that
            // was already three-channel, so a neutral colour of the same magnitude is the same arithmetic.
            const glm::vec4 raised( param.Value.x, param.Value.x, param.Value.x, param.Value.w );

            // A VALUE ALREADY EQUAL TO ITS OWN RAISE IS NOT A CHANGE, and reporting it as one is not a
            // cosmetic slip. The degenerate input (0, 0, 0, 0) is the case — black is black whether it is
            // one number or three — and while this function counted it, `SceneMigrator --check` reported
            // such a file as "would change" on every run, for ever, and the tool's own exit code said the
            // corpus was never converted. That is the "the action happened and nobody knew" defect with
            // its sign flipped: a report of work that was not done. A suite caught it, which is the whole
            // reason the degenerate case has a test of its own.
            if ( raised == param.Value )
                continue;

            param.Value = raised;
            report.Broadcast += 1;
        }

        return report;
    }

    namespace
    {
        // THE v22 TERRAIN BLOCK, known here and nowhere else: what ComponentRegistry wrote for the reflected
        // ECS::TerrainData before v23, with that struct's own defaults for a key the file did not state.
        struct LegacyTerrainV22
        {
            float Size           = 5000.0f;
            int   Resolution     = 64;
            float HeightScale    = 500.0f;
            float NoiseFrequency = 0.08f;
            int   Seed           = 1337;
        };

        // The v22 layer modes, by number (TerrainLayerMode: Auto, Manual, Off) ...
        constexpr int64_t kLegacyLayerManual = 1;
        constexpr int64_t kLegacyLayerOff    = 2;
        // ... and v23's (ECS::LandscapeLayerMode: Auto, Off). Stated as numbers because the scene stores them so.
        constexpr int64_t kLandscapeLayerAuto = 0;
        constexpr int64_t kLandscapeLayerOff  = 1;

        // What TerrainRenderer drew a v22 terrain with: at most 64 patches per side (kMaxGridDim), each split
        // at most 16 times by the TCS (kTessLevel). The bake reproduces that finest vertex spacing.
        constexpr uint32_t kLegacyMaxGridDim   = 64u;
        constexpr uint32_t kLegacyMaxTessLevel = 16u;
        // The most tiles a side the bake will make: past it, a smaller section size buys a few percent of
        // samples with many more entities and files.
        constexpr uint32_t kMaxTilesPerSide = 16u;

        float Fract( float v )
        {
            return v - std::floor( v );
        }

        // EVERY PRODUCT AND SUM IS ITS OWN STATEMENT, on purpose. The lattice hash takes fract() of numbers
        // in the hundreds of thousands, so one fused multiply-add (clang contracts `a * b + c` inside ONE
        // expression by default) moves a hash value and with it a whole noise cell by centimetres. Split,
        // the port rounds after every operation exactly as the GLSL text states them, and the suite's
        // vector transcription agrees with it to the 16-bit step.
        float Mix( float a, float b, float t )
        {
            const float wa = a * ( 1.0f - t );
            const float wb = b * t;
            return wa + wb;
        }

        // Hash / ValueNoise / FBm of the v22 TerrainTessEval.glslh, component by component.
        float LatticeHash( float px, float pz )
        {
            px             = Fract( px * 123.34f );
            pz             = Fract( pz * 456.21f );
            const float dx = px * ( px + 45.32f );
            const float dz = pz * ( pz + 45.32f );
            const float d  = dx + dz;
            px += d;
            pz += d;
            return Fract( px * pz );
        }

        float ValueNoise( float px, float pz )
        {
            const float ix   = std::floor( px );
            const float iz   = std::floor( pz );
            const float fx   = px - ix;
            const float fz   = pz - iz;
            const float twoX = 2.0f * fx;
            const float twoZ = 2.0f * fz;
            const float ux   = fx * fx * ( 3.0f - twoX );
            const float uz   = fz * fz * ( 3.0f - twoZ );

            const float a = LatticeHash( ix, iz );
            const float b = LatticeHash( ix + 1.0f, iz );
            const float c = LatticeHash( ix, iz + 1.0f );
            const float d = LatticeHash( ix + 1.0f, iz + 1.0f );
            return Mix( Mix( a, b, ux ), Mix( c, d, ux ), uz );
        }

        float FBm( float px, float pz )
        {
            float sum  = 0.0f;
            float amp  = 0.5f;
            float freq = 1.0f;
            for ( int octave = 0; octave < 5; ++octave )
            {
                const float layer = amp * ValueNoise( px * freq, pz * freq );
                sum += layer;
                freq *= 2.0f;
                amp *= 0.5f;
            }
            return sum;
        }

        // A tile's id: the entity's id and the tile coordinate through splitmix64, so the same v22 file always
        // bakes to the same entity ids and file names. Zero is "no id" and is stepped over.
        uint64_t TileId( uint64_t rootId, uint32_t tileX, uint32_t tileZ )
        {
            uint64_t z =
                 rootId + 0x9E3779B97F4A7C15ull * ( 1ull + ( static_cast<uint64_t>( tileX ) << 32u ) + tileZ );
            z = ( z ^ ( z >> 30u ) ) * 0xBF58476D1CE4E5B9ull;
            z = ( z ^ ( z >> 27u ) ) * 0x94D049BB133111EBull;
            z = z ^ ( z >> 31u );
            return z == 0u ? 1u : z;
        }

    } // namespace

    float ProceduralTerrainHeightV22( float x, float z, float noiseFrequency, int seed, float heightScale )
    {
        const float freq  = std::max( noiseFrequency, 0.0001f );
        const float seedF = static_cast<float>( seed );
        const float sx    = seedF * 0.137f;
        const float sz    = seedF * 0.911f;
        const float px    = x * freq;
        const float pz    = z * freq;
        const float h     = FBm( px + sx, pz + sz );
        return ( h - 0.5f ) * 2.0f * heightScale;
    }

    ProceduralTerrainGrid ProceduralTerrainGridFor( float sizeCm, int resolution )
    {
        const uint32_t patches =
             std::clamp<uint32_t>( static_cast<uint32_t>( std::max( resolution, 1 ) ), 1u, kLegacyMaxGridDim );
        const uint32_t target = patches * kLegacyMaxTessLevel;

        // Over UE's section sizes (the only tile sizes a landscape root accepts, LandscapeLayout.hpp), with at
        // most kMaxTilesPerSide tiles: the LARGEST section whose side is within 1/8 of the smallest side that
        // covers `target`. Fewer, bigger tiles for a few percent more samples; not 60 % more.
        uint32_t fewest = 0u;
        for ( const uint32_t quads : World::Landscape::kLandscapeTileQuadsValues )
        {
            const uint32_t tiles = ( target + quads - 1u ) / quads;
            if ( tiles <= kMaxTilesPerSide && ( fewest == 0u || tiles * quads < fewest ) )
                fewest = tiles * quads;
        }
        ProceduralTerrainGrid grid;
        for ( const uint32_t quads : World::Landscape::kLandscapeTileQuadsValues )
        {
            const uint32_t tiles = ( target + quads - 1u ) / quads;
            if ( tiles <= kMaxTilesPerSide && tiles * quads * 8u <= fewest * 9u )
            {
                grid.TilesPerSide = tiles; // ascending sizes: the last one kept is the largest
                grid.QuadsPerTile = quads;
            }
        }
        grid.SpacingCm = sizeCm / static_cast<float>( grid.TilesPerSide * grid.QuadsPerTile );
        return grid;
    }

    ProceduralTerrainMigrationReport MigrateProceduralTerrainV22ToV23( std::vector<Assets::EntityData>& entities,
                                                                       const std::filesystem::path&     sourceFile,
                                                                       const std::filesystem::path& assetsRoot )
    {
        ProceduralTerrainMigrationReport report;
        std::vector<Assets::EntityData>  created;

        for ( auto& entity : entities )
        {
            const auto payload = entity.Components.get( "Terrain" );
            if ( !payload.has_value() )
                continue;

            const std::string tag    = entity.Tag.value_or( "Entity" );
            const auto        reject = [&]( const std::string& why )
            {
                report.Rejected += 1;
                report.RejectedNames.push_back( tag + " > Terrain: " + why );
            };

            const auto fields = payload.value().to_object();
            if ( !fields.has_value() )
            {
                reject( "the payload is " + Describe( payload.value() ) + ", not an object" );
                continue;
            }
            const auto& in = fields.value();

            LegacyTerrainV22 terrain;
            bool             readable = true;
            const auto       number   = [&]( const char* key, auto& out )
            {
                const auto value = in.get( key );
                if ( !value.has_value() )
                    return;
                const auto n = AsFiniteNumber( value.value() );
                if ( !n.has_value() )
                {
                    reject( std::string( key ) + " is " + Describe( value.value() ) + ", not a finite number" );
                    readable = false;
                    return;
                }
                out = static_cast<std::remove_reference_t<decltype( out )>>( n.value() );
            };
            number( "Size", terrain.Size );
            number( "Resolution", terrain.Resolution );
            number( "HeightScale", terrain.HeightScale );
            number( "NoiseFrequency", terrain.NoiseFrequency );
            number( "Seed", terrain.Seed );
            if ( !readable )
                continue;
            if ( terrain.Size <= 0.0f )
            {
                reject( "Size is " + std::to_string( terrain.Size ) + " cm; a landscape needs a positive extent" );
                continue;
            }

            const glm::vec3 rotation = entity.Rotation.value_or( glm::vec3( 0.0f ) );
            const glm::vec3 scale    = entity.Scale.value_or( glm::vec3( 1.0f ) );
            if ( rotation != glm::vec3( 0.0f ) )
            {
                reject( "the entity is rotated; a landscape frame has no rotation (LandscapeComponent)" );
                continue;
            }
            if ( scale.x != scale.z || scale.x <= 0.0f || scale.y < 0.0f )
            {
                reject( "the entity's scale (" + std::to_string( scale.x ) + ", " + std::to_string( scale.y ) +
                        ", " + std::to_string( scale.z ) +
                        ") is not one positive spacing in x and z; a landscape has one sample spacing" );
                continue;
            }
            if ( !entity.id.has_value() || static_cast<uint64_t>( entity.id.value() ) == 0u )
            {
                reject( "the entity has no id for its tiles to name" );
                continue;
            }
            if ( sourceFile.empty() )
            {
                reject( "no source file was given, and the tiles' height files are named after it" );
                continue;
            }

            const ProceduralTerrainGrid grid     = ProceduralTerrainGridFor( terrain.Size, terrain.Resolution );
            const uint32_t              quads    = grid.TilesPerSide * grid.QuadsPerTile;
            const float                 cell     = terrain.Size / static_cast<float>( quads );
            const float                 half     = terrain.Size * 0.5f;
            const float                 heightCm = terrain.HeightScale * scale.y;
            const float zScale = heightCm > 0.0f ? heightCm / 256.0f : World::Landscape::kLandscapeDefaultZScale;
            const uint32_t rowSamples = quads + 1u;

            // The whole landscape's samples once; the tiles copy their window (edges are shared samples).
            std::vector<uint16_t> all( static_cast<size_t>( rowSamples ) * rowSamples );
            float                 maxAbs = 0.0f;
            for ( uint32_t z = 0; z < rowSamples; ++z )
                for ( uint32_t x = 0; x < rowSamples; ++x )
                {
                    // The v22 vertex stage's own arithmetic for a grid corner: index * cell - size / 2.
                    const float ax = static_cast<float>( x ) * cell;
                    const float az = static_cast<float>( z ) * cell;
                    const float lx = ax - half;
                    const float lz = az - half;
                    const float h  = ProceduralTerrainHeightV22( lx, lz, terrain.NoiseFrequency, terrain.Seed,
                                                                 terrain.HeightScale ) *
                                    scale.y;
                    maxAbs = std::max( maxAbs, std::abs( h ) );
                    all[static_cast<size_t>( z ) * rowSamples + x] =
                         World::Landscape::LandscapeSampleFromLocal( h / zScale );
                }

            const uint64_t                  rootId = static_cast<uint64_t>( entity.id.value() );
            bool                            failed = false;
            std::vector<Assets::EntityData> tiles;
            std::vector<LandscapeTileFile>  files;
            for ( uint32_t tz = 0; tz < grid.TilesPerSide && !failed; ++tz )
                for ( uint32_t tx = 0; tx < grid.TilesPerSide && !failed; ++tx )
                {
                    const uint32_t        side = grid.QuadsPerTile + 1u;
                    std::vector<uint16_t> samples( static_cast<size_t>( side ) * side );
                    for ( uint32_t z = 0; z < side; ++z )
                        for ( uint32_t x = 0; x < side; ++x )
                            samples[static_cast<size_t>( z ) * side + x] =
                                 all[static_cast<size_t>( tz * grid.QuadsPerTile + z ) * rowSamples +
                                     tx * grid.QuadsPerTile + x];
                    auto tile =
                         World::Landscape::LandscapeTileData::FromSamples( side, side, std::move( samples ) );
                    if ( !tile )
                    {
                        reject( "tile (" + std::to_string( tx ) + ", " + std::to_string( tz ) +
                                "): " + tile.GetError() );
                        failed = true;
                        break;
                    }

                    const uint64_t              id    = TileId( rootId, tx, tz );
                    const std::filesystem::path disk  = World::Landscape::LandscapeTileBlobPath( sourceFile, id );
                    const std::filesystem::path under = disk.lexically_relative( assetsRoot );
                    if ( under.empty() || *under.begin() == ".." )
                    {
                        reject( "the tile file " + disk.generic_string() + " is not under the assets root " +
                                assetsRoot.generic_string() + ", so the scene could not name it" );
                        failed = true;
                        break;
                    }

                    // Written by the loader's own writer (AuthoredComponentIO), so the block is spelled as
                    // a save spells it - the root id as a decimal string, not a JSON number.
                    ECS::LandscapeTileComponent block;
                    block.Landscape  = Common::UUID( rootId );
                    block.TileX      = static_cast<int32_t>( tx );
                    block.TileZ      = static_cast<int32_t>( tz );
                    block.HeightFile = ( Common::Constants::Path::ASSETS_PATH / under ).generic_string();

                    Assets::EntityData tileEntity;
                    tileEntity.id  = Common::UUID( id );
                    tileEntity.Tag = tag + " Tile " + std::to_string( tx ) + "_" + std::to_string( tz );
                    tileEntity.Components["LandscapeTile"] =
                         rfl::Generic( Core::Serialize::WriteComponent( block ) );
                    tiles.push_back( std::move( tileEntity ) );
                    files.push_back( { disk, World::Landscape::EncodeLandscapeTile( tile.GetValue() ) } );
                }
            if ( failed )
                continue;

            // The layer modes and the material leave with the relief, onto the root.
            rfl::Object<rfl::Generic> material;
            if ( const auto handle = in.get( "Material" ); handle.has_value() )
                material["Material"] = handle.value();
            std::vector<std::string> notes;
            for ( const char* key : { "GrassMode", "RockMode", "SnowMode" } )
            {
                const auto value = in.get( key );
                if ( !value.has_value() )
                    continue; // unstated: v22's default was Auto, and so is v23's
                const auto mode = value.value().to_int64();
                if ( mode.has_value() && mode.value() == kLegacyLayerManual )
                    notes.push_back( std::string( key ) + " Manual -> Auto (the splat map was never saved)" );
                material[key] =
                     rfl::Generic( mode.has_value() && mode.value() == kLegacyLayerOff ? kLandscapeLayerOff
                                                                                       : kLandscapeLayerAuto );
            }

            ECS::LandscapeComponent root;
            root.QuadsPerTile = grid.QuadsPerTile;
            root.SpacingCm    = cell * scale.x;
            root.ZScale       = zScale;
            const rfl::Generic rootGeneric( Core::Serialize::WriteComponent( root ) );

            // Sample (0, 0) is the grid's corner: the entity's origin minus half the (scaled) side.
            const glm::vec3 origin = entity.Translation.value_or( glm::vec3( 0.0f ) );
            entity.Translation     = glm::vec3( origin.x - half * scale.x, origin.y, origin.z - half * scale.z );
            entity.Scale           = glm::vec3( 1.0f );
            // Rebuilt, as every step here drops a key: rfl::Object has no erase. The two new blocks take the
            // retired one's place in the order.
            rfl::ExtraFields<rfl::Generic> kept;
            for ( const auto& [key, value] : entity.Components )
            {
                if ( key != "Terrain" )
                {
                    kept[key] = value;
                    continue;
                }
                kept["Landscape"]         = rootGeneric;
                kept["LandscapeMaterial"] = rfl::Generic( material );
            }
            entity.Components = std::move( kept );

            report.Entities += 1;
            report.Tiles += static_cast<int>( tiles.size() );
            std::string line =
                 tag + ": " + std::to_string( quads ) + " x " + std::to_string( quads ) + " quads in " +
                 std::to_string( grid.TilesPerSide ) + " x " + std::to_string( grid.TilesPerSide ) + " tiles, " +
                 std::to_string( root.SpacingCm ) + " cm spacing, max |h| " + std::to_string( maxAbs ) + " cm";
            for ( const auto& note : notes )
                line += "; " + note;
            report.ConvertedNames.push_back( std::move( line ) );
            for ( auto& t : tiles )
                created.push_back( std::move( t ) );
            for ( auto& f : files )
                report.Files.push_back( std::move( f ) );
        }

        for ( auto& t : created )
            entities.push_back( std::move( t ) );
        return report;
    }

    // И11 свёл цепочку шагов в один RunSteps, и функция выше — материальная, а не сценная: она
    // живёт своим проходом по .demat и в эту цепочку не входит. Обе стороны нужны целиком.
    namespace
    {
        // THE STEP CHAIN, AND THERE IS EXACTLY ONE OF IT IN THIS REPOSITORY (И11).
        //
        // `settings` is the file's scene-wide Settings block, or NULL for a `.deprefab`, which has no
        // such block at all. That single pointer is what lets one chain serve both file classes without
        // a flag anywhere: the four settings-only steps are skipped because the block does not exist,
        // and the two that take entities AND settings (units, retired keys) are handed a stand-in that
        // stays empty - which is precisely what those two already see for a scene whose file states no
        // settings. A prefab therefore gets every ENTITY-level step, forever, including the ones added
        // after this was written, because there is no second list to remember to update.
        //
        // The caller owns the version gating decision that comes BEFORE this (refusals, the prefab's
        // stamp-only case) and the stamping that comes after; this function only runs steps.
        void RunSteps( std::vector<Assets::EntityData>& entities, std::optional<rfl::Generic>* settings,
                       const std::string& name, int statedSceneVersion, int statedUnitVersion,
                       const std::filesystem::path& assetsRoot, const std::filesystem::path& sourceFile,
                       FileMigrationReport& report )
        {
            // Stands in for the block a prefab does not have. Never read back by the caller: the two
            // steps below that take it are both guarded on has_value(), so an absent block is a no-op
            // for them, and no step may be added here that would WRITE into it - the settings-only ones
            // are gated on the pointer instead, exactly so a created block cannot be silently dropped.
            std::optional<rfl::Generic>  absentSettings;
            std::optional<rfl::Generic>& settingsRef = settings != nullptr ? *settings : absentSettings;
            const bool                   hasSettings = settings != nullptr;

            if ( statedSceneVersion < kSceneVersionSky )
            {
                report.SkyRaised = true;
                report.Sky       = MigrateSkyV0ToV1( entities );
            }

            if ( statedUnitVersion < kUnitVersion )
            {
                report.UnitsRaised = true;
                report.Units       = MigrateMetresToUnits( entities, settingsRef );
            }

            if ( hasSettings && statedSceneVersion < kSceneVersionTonemap )
            {
                report.TonemapperRaised = true;
                report.Tonemap          = MigrateTonemapperV1ToV2( settingsRef );
            }

            if ( statedSceneVersion < kSceneVersionCloudNoise )
            {
                report.CloudNoiseRaised = true;
                report.CloudNoise       = MigrateCloudNoiseV2ToV3( entities );
            }

            if ( statedSceneVersion < kSceneVersionCloudSpecies )
            {
                report.CloudSpeciesRaised = true;
                report.CloudSpecies       = MigrateCloudSpeciesV3ToV4( entities );
            }

            // AFTER the step above and not beside it: v3 -> v4 WRITES the "Species" key that this one
            // reads. The two are the only pair in this function with an order that matters, and it is
            // stated here rather than left to the sequence they happen to be written in.
            if ( statedSceneVersion < kSceneVersionCloudType )
            {
                report.CloudTypeRaised = true;
                report.CloudType       = MigrateCloudTypeV4ToV5( entities );
            }

            // AFTER the step above for the same reason that one follows its own: v4 -> v5 WRITES the
            // "CloudType" key that this one renames. Three of the steps in this function now form one
            // chain — Species integer, then CloudType path, then CloudType1 slot — and each is gated on
            // its own version integer precisely so that a file entering at any point along it comes out
            // at the end.
            if ( statedSceneVersion < kSceneVersionCloudSet )
            {
                report.CloudSetRaised = true;
                report.CloudSet       = MigrateCloudSetV5ToV6( entities );
            }

            // Independent of the cloud chain above it and of everything else in this function: no terrain
            // field is a length the unit migration scales, and no sky or cloud step reads either of the
            // two keys this one pairs.
            if ( statedSceneVersion < kSceneVersionTerrainMaterial )
            {
                report.TerrainMaterialRaised = true;
                report.TerrainMaterial       = MigrateTerrainMaterialV6ToV7( entities );
            }

            // AFTER the step above, and this pair's order does matter: v6 -> v7 REMOVES the inline
            // Material component from terrain entities, and this step reads `Terrain.Material` - a
            // different key on a different payload, but running it first would rewrite paths inside a
            // component that is about to be deleted and report work that did not survive.
            if ( statedSceneVersion < kSceneVersionMaterialPath )
            {
                report.MaterialPathRaised = true;
                report.MaterialPath       = MigrateMaterialPathV7ToV8( entities, assetsRoot );
            }

            // Independent of every step above: it touches the scene-wide Settings block and no entity
            // payload, and no earlier step reads or writes Gravity.
            if ( hasSettings && statedSceneVersion < kSceneVersionGravityUnits )
            {
                report.GravityUnitsRaised = true;
                report.GravityUnits       = MigrateGravityUnitsV8ToV9( settingsRef );
            }

            // Independent of every step above: no earlier step reads or writes a "UILayout" payload, and
            // none of the three keys this one touches is a length the unit migration scales.
            if ( statedSceneVersion < kSceneVersionUIVisibility )
            {
                report.UIVisibilityRaised = true;
                report.UIVisibility       = MigrateUIVisibilityV9ToV10( entities );
            }

            // Independent of every step above: it touches one key of the scene-wide Settings block and no
            // entity payload, and no earlier step reads or writes SSRMaxDistance. The gravity step scales
            // the same block but a different key, so their order is irrelevant.
            if ( hasSettings && statedSceneVersion < kSceneVersionSSRUnits )
            {
                report.SSRUnitsRaised = true;
                report.SSRUnits       = MigrateSSRUnitsV10ToV11( settingsRef );
            }

            // AFTER the cloud chain above (v3 -> v6 write and rename the very keys this one moves out)
            // and independent of everything since: no other step reads a VolumetricCloud payload. The
            // material files it returns are the TOOL's to write - this function stays pure.
            if ( statedSceneVersion < kSceneVersionCloudMaterial )
            {
                report.CloudMaterialRaised = true;
                report.CloudMaterial       = MigrateCloudMaterialV11ToV12( entities, name );
            }

            // Touches only the Settings block, so it is independent of every step above and of the two
            // that also edit Settings (gravity v8->v9, SSR v10->v11) - those rewrite one key each and
            // this one removes ten others.
            if ( hasSettings && statedSceneVersion < kSceneVersionDebugView )
            {
                report.DebugViewRaised = true;
                report.DebugView       = MigrateDebugViewV12ToV13( settingsRef );
            }

            // Touches only "Script" payloads, which no step above reads or writes, so it is independent
            // of all of them.
            if ( statedSceneVersion < kSceneVersionScriptRoot )
            {
                report.ScriptRootRaised = true;
                report.ScriptRoot       = MigrateScriptRootV15ToV16( entities );
            }

            // Touches four component payloads that no step above reads or writes, so it is independent of
            // all of them. It takes the FILE'S OWN assets root, like the v7 -> v8 material step, because
            // the project's assets-root NAME is the only per-project fact the tagging needs and it cannot
            // be read from a global in a pure function.
            if ( statedSceneVersion < kSceneVersionServiceAssetRoot )
            {
                report.ServiceAssetRootRaised = true;
                report.ServiceAssetRoot       = MigrateServiceAssetRootV16ToV17( entities, assetsRoot );
            }

            // LAST, and it has to be: every step above may WRITE keys, and this one is the statement of
            // which keys must not be in the finished file. Running it earlier would let a later step
            // reintroduce a retired name and leave the tool reporting a removal that did not survive its
            // own run.
            //
            // AND GATED ON THE HEAD, not on a step number of its own. It used to read
            // `< kSceneVersionRetiredKeys` (14), which was right for the day it landed and wrong for the
            // day a row was ADDED: K3's five rows would then never have fired on a corpus already stamped
            // 14, and the tool would have reported every file up to date while five dead keys sat in each
            // one. The pass reads only the table, never the values, so re-running it on a file it has
            // already cleaned removes nothing and leaves the tree byte-identical — which is what makes
            // this gate safe here and unsafe for every step above. See the note over MigrateRetiredKeys
            // in the header.
            //
            // It runs for a prefab too, on the entity half of the table: a retired COMPONENT key is dead
            // in a `.deprefab` for exactly the reasons it is dead in a `.desce`, and the saver preserves
            // it in both.
            // Touches only "Terrain" payloads, which no step above reads or writes except the v6 -> v7
            // material step - and that one moves a MaterialComponent off the entity rather than editing
            // the Terrain payload's keys, so the two are independent. Before the retirement pass, like
            // every other step: that pass is the statement of what must not be in the finished file.
            if ( statedSceneVersion < kSceneVersionGrassGeneration )
            {
                report.GrassGenerationRaised = true;
                report.GrassGeneration       = MigrateGrassGenerationV17ToV18( entities );
            }

            // Touches four authored STRINGS that no step above reads or writes — the service-asset step
            // next door edits `UIText.Font`, a different key of the same payload, so the two are
            // independent. Before the retirement pass, like every other step.
            if ( statedSceneVersion < kSceneVersionTextKeySigil )
            {
                report.TextKeySigilRaised = true;
                report.TextKeySigil       = MigrateTextKeySigilV18ToV19( entities );
            }

            // The state machine leaves the entity for a file of its own. After the sigil step and before
            // the retirement pass, like every other step; it reads and writes one key no step above
            // touches, so nothing depends on where in this chain it sits.
            if ( statedSceneVersion < kSceneVersionAnimGraphAsset )
            {
                report.AnimGraphRaised = true;
                report.AnimGraph       = MigrateAnimGraphV20ToV21( entities );
            }

            if ( statedSceneVersion < kSceneVersionEditMesh )
            {
                report.EditMeshRaised = true;
                report.EditMesh       = MigrateEditMeshV21ToV22( entities );
            }

            // The procedural terrain leaves for a landscape. Reads and writes the `Terrain` key only, which
            // no step after v18 touches; before the retirement pass, like every other step.
            if ( statedSceneVersion < kSceneVersionProceduralTerrain )
            {
                report.ProceduralTerrainRaised = true;
                report.ProceduralTerrain = MigrateProceduralTerrainV22ToV23( entities, sourceFile, assetsRoot );
            }

            if ( statedSceneVersion < kSceneVersion )
            {
                report.RetiredKeysRaised = true;
                report.RetiredKeys       = MigrateRetiredKeys( settingsRef, entities );
            }
        }

        // The refusal a file gets when its stated pair is one this tool has no route from: the numbers it
        // states, the numbers this tool knows, and what to do instead. Built here rather than at each
        // site so a scene and a prefab are refused in the same words.
        std::string RefuseGeneration( const char* what, int statedSceneVersion, int statedUnitVersion,
                                      const char* why )
        {
            return std::string( "states " ) + what + " schema v" + std::to_string( statedSceneVersion ) +
                   " / world units v" + std::to_string( statedUnitVersion ) + ", and this tool raises files to v" +
                   std::to_string( kSceneVersion ) + "/v" + std::to_string( kUnitVersion ) + ". " + why;
        }
    } // namespace

    FileMigrationReport MigrateScene( SceneSerialized& scene, const std::filesystem::path& assetsRoot,
                                      const std::filesystem::path& sourceFile )
    {
        FileMigrationReport report;

        const int statedSceneVersion = scene.SceneVersion.value_or( 0 );
        const int statedUnitVersion  = scene.UnitVersion.value_or( 0 );

        // A file from a LATER build is refused, not stamped down. Every gate in RunSteps is
        // `stated < step`, so a v18 tree matched no step and fell through to the unconditional stamp
        // below: this function wrote v17 over a v18 number while the payloads stayed v18, and the tool
        // then printed "already at scene v17" about a file it had just mis-labelled — a successful-looking
        // answer over a substitution (§1.4). The prefab migrator refused this case from the day it was
        // written; scenes now get the same guard from the same place.
        if ( statedSceneVersion > kSceneVersion || statedUnitVersion > kUnitVersion )
        {
            report.Refused = RefuseGeneration( "scene", statedSceneVersion, statedUnitVersion,
                                               "A file at a LATER generation was written by a build this "
                                               "tool predates - convert it with THAT build's SceneMigrator." );
            return report;
        }

        RunSteps( scene.Entities, &scene.Settings, scene.SceneName, statedSceneVersion, statedUnitVersion,
                  assetsRoot, sourceFile, report );

        // Stamped whether or not anything moved: an empty scene at version 0 is still a scene at version 0,
        // and leaving it unstamped is how every load ends up re-running a migration that already happened.
        scene.SceneVersion = kSceneVersion;
        scene.UnitVersion  = kUnitVersion;

        return report;
    }

    PrefabMigrationOutcome MigratePrefab( Assets::PrefabData& prefab, const std::filesystem::path& assetsRoot,
                                          const std::filesystem::path& sourceFile )
    {
        PrefabMigrationOutcome outcome;
        outcome.FoundSceneVersion = prefab.SceneVersion.value_or( 0 );
        outcome.FoundUnitVersion  = prefab.UnitVersion.value_or( 0 );

        if ( Assets::PrefabIsAtCurrentVersion( prefab ) )
        {
            outcome.AlreadyCurrent = true;
            return outcome;
        }

        if ( outcome.FoundSceneVersion > kSceneVersion || outcome.FoundUnitVersion > kUnitVersion )
        {
            outcome.Refused = RefuseGeneration( "prefab", outcome.FoundSceneVersion, outcome.FoundUnitVersion,
                                                "A file at a LATER generation was written by a build this "
                                                "tool predates - convert it with THAT build's SceneMigrator." );
            return outcome;
        }

        // The unstamped pre-Д28 file: STAMP ONLY. The header says at length why the chain must not run on
        // it — a `.deprefab` at v0 could be from any generation up to the one Д28 landed in, and the sky
        // and unit steps are not safe to re-run.
        if ( outcome.FoundSceneVersion == 0 && outcome.FoundUnitVersion == 0 )
        {
            prefab.SceneVersion = kSceneVersion;
            prefab.UnitVersion  = kUnitVersion;
            outcome.StampOnly   = true;
            return outcome;
        }

        // Half-stamped: one integer stated and the other not, or a unit version that is neither 0 nor the
        // head. Assets::WritePrefabJson stamps both together, so no build produces this, and the version
        // the chain would be entered at is unknowable. Refused rather than guessed at.
        if ( outcome.FoundSceneVersion == 0 || outcome.FoundUnitVersion != kUnitVersion )
        {
            outcome.Refused = RefuseGeneration(
                 "prefab", outcome.FoundSceneVersion, outcome.FoundUnitVersion,
                 "Both integers are stamped together by the one writer of prefab text, so a file stating "
                 "one and not the other was hand-edited and the generation its payloads are at cannot be "
                 "known. Nothing was changed." );
            return outcome;
        }

        RunSteps( prefab.Entities, nullptr, prefab.Name, outcome.FoundSceneVersion, outcome.FoundUnitVersion,
                  assetsRoot, sourceFile, outcome.Steps );

        prefab.SceneVersion = kSceneVersion;
        prefab.UnitVersion  = kUnitVersion;

        return outcome;
    }

} // namespace Desert::Migration
