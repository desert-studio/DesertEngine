#include <Engine/Core/Serialize/SceneUpgrade.hpp>

#include <Engine/Assets/TextAssetHeaderIdentity.hpp>
#include <Engine/Geometry/LightTravel.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Settings/RenderMethods.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <rflcpp/rfl/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <unordered_map>
#include <span>
#include <utility>

namespace Desert::Core
{
    namespace
    {
        // Applies @p edit to the object block @p component of @p components, if it is stated as an object.
        template <typename Edit>
        void EditBlock( rfl::ExtraFields<rfl::Generic>& components, const char* component, Edit&& edit )
        {
            const auto payload = components.get( component );
            if ( !payload.has_value() )
                return;
            auto fields = payload.value().to_object();
            if ( !fields.has_value() )
                return;
            rfl::Generic::Object block = std::move( fields.value() );
            if ( edit( block ) )
                components[component] = rfl::Generic( std::move( block ) );
        }

        // @p block without @p key; true when the key was there.
        bool DropKey( rfl::Generic::Object& block, const std::string& key )
        {
            if ( !block.get( key ).has_value() )
                return false;
            rfl::Generic::Object kept;
            for ( const auto& [name, value] : block )
                if ( name != key )
                    kept[name] = value;
            block = std::move( kept );
            return true;
        }

        // A file refused over a step's list, in one line naming the file and every refusal.
        std::string RefusedWhole( std::string_view name, const std::vector<std::string>& refusals )
        {
            std::string joined;
            for ( const std::string& refusal : refusals )
                joined += ( joined.empty() ? std::string() : std::string( "; " ) ) + refusal;
            return std::format( "'{}': {}", name, joined );
        }

        std::string GenerationsText( const SceneGenerations& g )
        {
            return std::format( "scene v{} / world units v{} / world partition v{} / asset references v{} / "
                                "lights v{} / post process v{}",
                                g.Scene, g.Unit, g.WorldPartition, g.AssetRef, g.Light, g.PostProcess );
        }

        // The step numbers, frozen: a step restates the numbers of ITS generation, so a later renumbering of an
        // enum cannot change what an old file becomes.
        constexpr int kTonemapACES                 = 0; // Core::TonemapOperator::ACES at v74
        constexpr int kTonemapFilmic               = 2; // Core::TonemapOperator::Filmic at v74
        constexpr int kGrassRasterScalability      = 0; // Core::GrassRasterPath::Scalability at v75
        constexpr int kGrassRasterVisibilityBuffer = 2; // Core::GrassRasterPath::VisibilityBuffer at v75
        // The post-process v0 PostProcessSettings defaults a record that did not state a key rendered with (v1 states
        // them).
        constexpr double kPostProcessV0BloomIntensity    = 0.8;
        constexpr double kPostProcessV0BloomThreshold    = 2.0;
        constexpr double kPostProcessV0AutoExposureSpeed = 1.5;
        // The histogram window every post-process v0 file was graded under (AutoExposureRenderer's constants then).
        constexpr double kPostProcessV0LowPercent  = 0.0;
        constexpr double kPostProcessV0HighPercent = 95.0;

        // A JSON number, whichever of int or double it was read as.
        std::optional<double> NumberOf( const rfl::Generic& value )
        {
            if ( const auto d = value.to_double(); d.has_value() )
                return d.value();
            if ( const auto i = value.to_int64(); i.has_value() )
                return static_cast<double>( i.value() );
            return std::nullopt;
        }
    } // namespace


    TracedRequestReport MigrateTracedRequestV71ToV72( std::vector<Assets::EntityData>& entities )
    {
        using Common::Scalability::GIRequest;
        using Common::Scalability::ReflectionRequest;
        TracedRequestReport report;

        // The v64..v71 ladders by their numbers, as today's requests: both tracers are Traced.
        constexpr std::array<int, 7> kGIFromOld{
             static_cast<int>( GIRequest::Off ),         static_cast<int>( GIRequest::Lightmaps ),
             static_cast<int>( GIRequest::ScreenSpace ), static_cast<int>( GIRequest::ProbeVolume ),
             static_cast<int>( GIRequest::Traced ),      static_cast<int>( GIRequest::Traced ),
             static_cast<int>( GIRequest::PathTraced ) };
        constexpr std::array<int, 6> kReflectionsFromOld{
             static_cast<int>( ReflectionRequest::Off ),         static_cast<int>( ReflectionRequest::Captures ),
             static_cast<int>( ReflectionRequest::ScreenSpace ), static_cast<int>( ReflectionRequest::Traced ),
             static_cast<int>( ReflectionRequest::Traced ),
             static_cast<int>( ReflectionRequest::PathTraced ) };

        // One key of one Settings object; false when it is absent or already on the new number.
        const auto restateKey = [&]( rfl::Generic::Object& settings, const char* key, std::span<const int> table,
                                     const std::string& who, bool& refused )
        {
            const auto stated = settings.get( key );
            if ( !stated.has_value() )
                return false;
            const auto old = stated.value().to_int();
            if ( !old.has_value() || old.value() < 0 || old.value() >= static_cast<int>( table.size() ) )
            {
                report.Refused.push_back( std::format(
                     "{}: PostProcessVolume.Settings.{} {} is not on the v71 ladder (0 .. {} PathTraced)", who,
                     key, rfl::json::write( stated.value() ), table.size() - 1 ) );
                refused = true;
                return false;
            }
            const int now = table[static_cast<std::size_t>( old.value() )];
            if ( now == old.value() )
                return false;
            DropKey( settings, key );
            settings[key] = rfl::Generic( now );
            ++report.Restated;
            return true;
        };

        const auto restate = [&]( rfl::ExtraFields<rfl::Generic>& components, const std::string& who )
        {
            EditBlock( components, "PostProcessVolume",
                       [&]( rfl::Generic::Object& block )
                       {
                           const auto stated = block.get( "Settings" );
                           if ( !stated.has_value() )
                               return false;
                           auto fields = stated.value().to_object();
                           if ( !fields.has_value() )
                           {
                               report.Refused.push_back(
                                    std::format( "{}: PostProcessVolume.Settings {} is not an object", who,
                                                 rfl::json::write( stated.value() ) ) );
                               return false;
                           }
                           rfl::Generic::Object settings = std::move( fields.value() );
                           bool                 refused  = false;
                           const bool           gi       = restateKey( settings, "GlobalIlluminationMethod",
                                                                       std::span<const int>( kGIFromOld ), who,
                                                                       refused );
                           const bool reflections =
                                restateKey( settings, "ReflectionsMethod",
                                            std::span<const int>( kReflectionsFromOld ), who, refused );
                           if ( refused || ( !gi && !reflections ) )
                               return false;
                           DropKey( block, "Settings" );
                           block["Settings"] = rfl::Generic( std::move( settings ) );
                           return true;
                       } );
        };

        for ( auto& entity : entities )
        {
            const std::string who = entity.id ? std::format( "entity {}", entity.id->ToString() )
                                              : std::string( "<record without id>" );
            restate( entity.Components, who );
            if ( entity.PrefabOverrides )
                for ( auto& override_ : *entity.PrefabOverrides )
                    restate( override_.Components, who );
        }
        return report;
    }

    ComposedGIReport MigrateComposedGIV72ToV73( std::vector<Assets::EntityData>& entities )
    {
        using Common::Scalability::GIRequest;
        constexpr int    kScreenSpace = static_cast<int>( GIRequest::ScreenSpace );
        constexpr int    kOff         = static_cast<int>( GIRequest::Off );
        ComposedGIReport report;

        const auto restate = [&]( rfl::ExtraFields<rfl::Generic>& components, const std::string& who )
        {
            EditBlock( components, "PostProcessVolume",
                       [&]( rfl::Generic::Object& block )
                       {
                           const auto stated = block.get( "Settings" );
                           if ( !stated.has_value() )
                               return false;
                           auto fields = stated.value().to_object();
                           if ( !fields.has_value() )
                           {
                               report.Refused.push_back(
                                    std::format( "{}: PostProcessVolume.Settings {} is not an object", who,
                                                 rfl::json::write( stated.value() ) ) );
                               return false;
                           }
                           rfl::Generic::Object settings = std::move( fields.value() );
                           bool                 changed  = false;
                           if ( const auto gi = settings.get( "GlobalIlluminationMethod" ); gi.has_value() )
                           {
                               const auto number = gi.value().to_int();
                               if ( number.has_value() && number.value() == kScreenSpace )
                               {
                                   DropKey( settings, "GlobalIlluminationMethod" );
                                   settings["GlobalIlluminationMethod"] = rfl::Generic( kOff );
                                   ++report.Restated;
                                   changed = true;
                               }
                           }
                           if ( settings.get( "GIIntensity" ).has_value() )
                           {
                               DropKey( settings, "GIIntensity" );
                               ++report.Dropped;
                               changed = true;
                           }
                           if ( !changed )
                               return false;
                           DropKey( block, "Settings" );
                           block["Settings"] = rfl::Generic( std::move( settings ) );
                           return true;
                       } );
        };

        for ( auto& entity : entities )
        {
            const std::string who = entity.id ? std::format( "entity {}", entity.id->ToString() )
                                              : std::string( "<record without id>" );
            restate( entity.Components, who );
            if ( entity.PrefabOverrides )
                for ( auto& override_ : *entity.PrefabOverrides )
                    restate( override_.Components, who );
        }
        return report;
    }

    FilmicDefaultReport MigrateFilmicDefaultV73ToV74( std::vector<Assets::EntityData>& entities )
    {
        constexpr int       kACES   = kTonemapACES;
        constexpr int       kFilmic = kTonemapFilmic;
        FilmicDefaultReport report;

        const auto restate = [&]( rfl::ExtraFields<rfl::Generic>& components, const std::string& who )
        {
            EditBlock( components, "PostProcessVolume",
                       [&]( rfl::Generic::Object& block )
                       {
                           const auto stated = block.get( "Settings" );
                           if ( !stated.has_value() )
                               return false;
                           auto fields = stated.value().to_object();
                           if ( !fields.has_value() )
                           {
                               report.Refused.push_back(
                                    std::format( "{}: PostProcessVolume.Settings {} is not an object", who,
                                                 rfl::json::write( stated.value() ) ) );
                               return false;
                           }
                           rfl::Generic::Object settings = std::move( fields.value() );
                           bool changed = false;
                           if ( const auto op = settings.get( "Tonemapper" ); op.has_value() )
                           {
                               const auto number = op.value().to_int();
                               if ( number.has_value() && number.value() == kACES )
                               {
                                   DropKey( settings, "Tonemapper" );
                                   settings["Tonemapper"] = rfl::Generic( kFilmic );
                                   ++report.Restated;
                                   changed = true;
                               }
                           }
                           if ( !changed )
                               return false;
                           DropKey( block, "Settings" );
                           block["Settings"] = rfl::Generic( std::move( settings ) );
                           return true;
                       } );
        };

        for ( auto& entity : entities )
        {
            const std::string who = entity.id ? std::format( "entity {}", entity.id->ToString() )
                                              : std::string( "<record without id>" );
            restate( entity.Components, who );
            if ( entity.PrefabOverrides )
                for ( auto& override_ : *entity.PrefabOverrides )
                    restate( override_.Components, who );
        }
        return report;
    }

    GrassRasterPathReport MigrateGrassRasterPathV74ToV75( std::vector<Assets::EntityData>& entities )
    {
        constexpr int         kScalability      = kGrassRasterScalability;
        constexpr int         kVisibilityBuffer = kGrassRasterVisibilityBuffer;
        GrassRasterPathReport report;

        const auto restate = [&]( rfl::ExtraFields<rfl::Generic>& components, const std::string& who )
        {
            EditBlock( components, "PostProcessVolume",
                       [&]( rfl::Generic::Object& block )
                       {
                           const auto stated = block.get( "Settings" );
                           if ( !stated.has_value() )
                               return false;
                           auto fields = stated.value().to_object();
                           if ( !fields.has_value() )
                           {
                               report.Refused.push_back(
                                    std::format( "{}: PostProcessVolume.Settings {} is not an object", who,
                                                 rfl::json::write( stated.value() ) ) );
                               return false;
                           }
                           rfl::Generic::Object settings = std::move( fields.value() );
                           const auto           former   = settings.get( "GrassVisibilityBuffer" );
                           if ( !former.has_value() )
                               return false;
                           const auto flag = former.value().to_bool();
                           if ( !flag.has_value() )
                           {
                               report.Refused.push_back( std::format(
                                    "{}: PostProcessVolume.Settings.GrassVisibilityBuffer {} is not a bool", who,
                                    rfl::json::write( former.value() ) ) );
                               return false;
                           }
                           int path = kScalability;
                           if ( flag.value() )
                               path = kVisibilityBuffer;
                           DropKey( settings, "GrassVisibilityBuffer" );
                           settings["GrassRaster"] = rfl::Generic( path );
                           ++report.Restated;
                           DropKey( block, "Settings" );
                           block["Settings"] = rfl::Generic( std::move( settings ) );
                           return true;
                       } );
        };

        for ( auto& entity : entities )
        {
            const std::string who = entity.id ? std::format( "entity {}", entity.id->ToString() )
                                              : std::string( "<record without id>" );
            restate( entity.Components, who );
            if ( entity.PrefabOverrides )
                for ( auto& override_ : *entity.PrefabOverrides )
                    restate( override_.Components, who );
        }
        return report;
    }

    PostProcessBloomReport MigratePostProcessBloomV0ToV1( std::vector<Assets::EntityData>& entities )
    {
        PostProcessBloomReport report;

        // @p record: the entity's own block (absent keys meant the post-process v0 defaults, which this states);
        // an override is a delta, so only the keys it states are re-stated.
        const auto restate = [&]( rfl::ExtraFields<rfl::Generic>& components, const std::string& who, bool record )
        {
            EditBlock( components, "PostProcessVolume",
                       [&]( rfl::Generic::Object& block )
                       {
                           // A record without Settings had every post-process v0 default: it is restated from an empty object.
                           const auto           stated = block.get( "Settings" );
                           rfl::Generic::Object settings;
                           if ( stated.has_value() )
                           {
                               auto fields = stated.value().to_object();
                               if ( !fields.has_value() )
                               {
                                   report.Refused.push_back(
                                        std::format( "{}: PostProcessVolume.Settings {} is not an object", who,
                                                     rfl::json::write( stated.value() ) ) );
                                   return false;
                               }
                               settings = std::move( fields.value() );
                           }
                           else if ( !record )
                               return false;
                           const auto number = [&]( const char* key, std::optional<double> fallback )
                                -> std::optional<double>
                           {
                               const auto value = settings.get( key );
                               if ( !value.has_value() )
                                   return fallback;
                               const auto parsed = NumberOf( value.value() );
                               if ( !parsed.has_value() )
                                   report.Refused.push_back(
                                        std::format( "{}: PostProcessVolume.Settings.{} {} is not a number", who, key,
                                                     rfl::json::write( value.value() ) ) );
                               return parsed;
                           };
                           const auto put = [&]( const char* key, rfl::Generic value )
                           {
                               DropKey( settings, key );
                               settings[key] = std::move( value );
                           };
                           const std::size_t refusedBefore = report.Refused.size();
                           bool              changed       = false;

                           // EnableBloom false -> Intensity 0 (UE has no switch); true keeps the intensity.
                           const auto enable = settings.get( "EnableBloom" );
                           std::optional<bool> enabled;
                           if ( enable.has_value() )
                           {
                               const auto flag = enable.value().to_bool();
                               if ( !flag.has_value() )
                               {
                                   report.Refused.push_back( std::format(
                                        "{}: PostProcessVolume.Settings.EnableBloom {} is not a bool", who,
                                        rfl::json::write( enable.value() ) ) );
                                   return false;
                               }
                               enabled = flag.value();
                           }
                           else if ( record )
                               enabled = false; // the post-process v0 default
                           if ( enabled.has_value() )
                           {
                               if ( !enabled.value() )
                               {
                                   put( "BloomIntensity", rfl::Generic( 0.0 ) );
                                   ++report.BloomOff;
                               }
                               else
                               {
                                   // An override switching bloom on over a base this step set to 0 states the
                                   // intensity it meant: its own, or the post-process v0 default.
                                   const auto intensity = number( "BloomIntensity", kPostProcessV0BloomIntensity );
                                   if ( intensity.has_value() )
                                       put( "BloomIntensity", rfl::Generic( intensity.value() ) );
                               }
                               DropKey( settings, "EnableBloom" );
                               changed = true;
                           }
                           // Threshold <= 0 bloomed every pixel: UE's -1. Above 0 the number is kept.
                           const auto threshold =
                                number( "BloomThreshold", record ? std::optional<double>( kPostProcessV0BloomThreshold )
                                                                 : std::nullopt );
                           if ( threshold.has_value() )
                           {
                               put( "BloomThreshold", rfl::Generic( threshold.value() <= 0.0 ? -1.0
                                                                                             : threshold.value() ) );
                               changed = true;
                           }
                           // One speed -> both directions at it.
                           const auto speed =
                                number( "AutoExposureSpeed", record ? std::optional<double>( kPostProcessV0AutoExposureSpeed )
                                                                    : std::nullopt );
                           if ( speed.has_value() )
                           {
                               DropKey( settings, "AutoExposureSpeed" );
                               put( "AutoExposureSpeedUp", rfl::Generic( speed.value() ) );
                               put( "AutoExposureSpeedDown", rfl::Generic( speed.value() ) );
                               changed = true;
                           }
                           if ( record )
                           {
                               // The former defaults where the record did not state them: auto exposure off and
                               // the 0 / 95 histogram window the scene was graded under.
                               if ( !settings.get( "AutoExposure" ).has_value() )
                                   put( "AutoExposure", rfl::Generic( false ) );
                               put( "AutoExposureLowPercent", rfl::Generic( kPostProcessV0LowPercent ) );
                               put( "AutoExposureHighPercent", rfl::Generic( kPostProcessV0HighPercent ) );
                               changed = true;
                           }
                           if ( report.Refused.size() != refusedBefore || !changed )
                               return false;
                           ++report.Restated;
                           DropKey( block, "Settings" );
                           block["Settings"] = rfl::Generic( std::move( settings ) );
                           return true;
                       } );
        };

        for ( auto& entity : entities )
        {
            const std::string who = entity.id ? std::format( "entity {}", entity.id->ToString() )
                                              : std::string( "<record without id>" );
            restate( entity.Components, who, true );
            if ( entity.PrefabOverrides )
                for ( auto& override_ : *entity.PrefabOverrides )
                    restate( override_.Components, who, false );
        }
        return report;
    }

    LandscapeSplineEntitiesReport
    MigrateLandscapeSplineEntitiesV75ToV76( std::vector<Assets::EntityData>& entities, bool createEntities )
    {
        LandscapeSplineEntitiesReport   report;
        std::vector<Assets::EntityData> created;
        // A non-negative integer field of an object, or nullopt.
        const auto indexOf = []( const rfl::Generic::Object& object,
                                 const char*                 key ) -> std::optional<std::size_t>
        {
            const auto value = object.get( key );
            if ( !value.has_value() )
                return std::nullopt;
            const auto number = value.value().to_int();
            if ( !number.has_value() || number.value() < 0 )
                return std::nullopt;
            return static_cast<std::size_t>( number.value() );
        };
        // Takes the Splines key out of one Landscape block; @p root is the record's id (0: an override).
        const auto take = [&]( rfl::Generic::Object& block, const std::string& name, uint64_t root ) -> bool
        {
            const auto stated = block.get( "Splines" );
            if ( !stated.has_value() )
                return false;
            const rfl::Generic value = stated.value();
            DropKey( block, "Splines" );
            ++report.RootsCleared;
            const auto splines = value.to_object();
            if ( !splines.has_value() )
            {
                report.Refused.push_back( std::format( "'{}': Landscape.Splines is not an object", name ) );
                return true;
            }
            rfl::Generic::Array points;
            rfl::Generic::Array segments;
            if ( const auto listed = splines.value().get( "ControlPoints" ); listed.has_value() )
                if ( auto array = listed.value().to_array(); array.has_value() )
                    points = std::move( array.value() );
            if ( const auto listed = splines.value().get( "Segments" ); listed.has_value() )
                if ( auto array = listed.value().to_array(); array.has_value() )
                    segments = std::move( array.value() );
            if ( points.empty() )
                return true; // an empty set: nothing becomes an entity
            if ( !createEntities || root == 0 )
            {
                report.Refused.push_back( std::format(
                     "'{}': a prefab's Landscape states {} spline control point(s); a spline is its own entity "
                     "since scene v{} and cannot be part of a prefab",
                     name, points.size(), kSceneVersionLandscapeSplineEntities ) );
                return true;
            }
            // Connected groups (union-find over the segments' ends), numbered by their first point.
            std::vector<std::size_t> parent( points.size() );
            for ( std::size_t i = 0; i < parent.size(); ++i )
                parent[i] = i;
            const auto find = [&parent]( std::size_t i )
            {
                while ( parent[i] != i )
                    i = parent[i] = parent[parent[i]];
                return i;
            };
            std::vector<std::pair<std::size_t, std::size_t>> ends;
            for ( std::size_t s = 0; s < segments.size(); ++s )
            {
                const auto fields = segments[s].to_object();
                const auto start  = fields.has_value() ? indexOf( fields.value(), "Start" ) : std::nullopt;
                const auto end    = fields.has_value() ? indexOf( fields.value(), "End" ) : std::nullopt;
                if ( !start || !end || *start >= points.size() || *end >= points.size() )
                {
                    report.Refused.push_back(
                         std::format( "'{}': Landscape.Splines.Segments[{}] names no control point", name, s ) );
                    return true;
                }
                ends.emplace_back( *start, *end );
                parent[find( *start )] = find( *end );
            }
            std::vector<std::size_t> groupOf( points.size() );
            std::vector<std::size_t> rootGroup( points.size(), points.size() );
            std::size_t              groups = 0;
            for ( std::size_t i = 0; i < points.size(); ++i )
            {
                const std::size_t top = find( i );
                if ( rootGroup[top] == points.size() )
                    rootGroup[top] = groups++;
                groupOf[i] = rootGroup[top];
            }
            for ( std::size_t g = 0; g < groups; ++g )
            {
                rfl::Generic::Array      groupPoints;
                std::vector<std::size_t> newIndex( points.size(), 0 );
                for ( std::size_t i = 0; i < points.size(); ++i )
                    if ( groupOf[i] == g )
                    {
                        newIndex[i] = groupPoints.size();
                        groupPoints.push_back( points[i] );
                    }
                rfl::Generic::Array groupSegments;
                for ( std::size_t s = 0; s < segments.size(); ++s )
                {
                    if ( groupOf[ends[s].first] != g )
                        continue;
                    rfl::Generic::Object segment = segments[s].to_object().value();
                    segment["Start"]             = rfl::Generic( static_cast<int>( newIndex[ends[s].first] ) );
                    segment["End"]               = rfl::Generic( static_cast<int>( newIndex[ends[s].second] ) );
                    groupSegments.push_back( rfl::Generic( std::move( segment ) ) );
                }
                // FNV-1a over the root and the group: the same scene migrated twice names the same records.
                uint64_t id = 14695981039346656037ull;
                for ( const char c : std::format( "LandscapeSpline:{}:{}", root, g ) )
                    id = ( id ^ static_cast<uint8_t>( c ) ) * 1099511628211ull;
                rfl::Generic::Object splinesBlock;
                splinesBlock["ControlPoints"] = rfl::Generic( std::move( groupPoints ) );
                splinesBlock["Segments"]      = rfl::Generic( std::move( groupSegments ) );
                rfl::Generic::Object component;
                component["Landscape"] = rfl::Generic( std::to_string( root ) ); // a UUID is a decimal string
                component["Splines"]   = rfl::Generic( std::move( splinesBlock ) );
                Assets::EntityData spline;
                spline.id                            = Common::UUID( id == 0 ? 1 : id );
                spline.Tag                           = std::string( "Landscape Spline" );
                spline.Components["LandscapeSpline"] = rfl::Generic( std::move( component ) );
                created.push_back( std::move( spline ) );
                ++report.SplinesCreated;
            }
            return true;
        };
        for ( Assets::EntityData& entity : entities )
        {
            const std::string name = entity.Tag.value_or( std::string() );
            const uint64_t    root = entity.id.has_value() ? static_cast<uint64_t>( *entity.id ) : 0u;
            EditBlock( entity.Components, "Landscape",
                       [&]( rfl::Generic::Object& block ) { return take( block, name, root ); } );
            if ( entity.PrefabOverrides.has_value() )
                for ( auto& override_ : *entity.PrefabOverrides )
                    EditBlock( override_.Components, "Landscape",
                               [&]( rfl::Generic::Object& block ) { return take( block, name, 0u ); } );
        }
        for ( Assets::EntityData& spline : created )
            entities.push_back( std::move( spline ) );
        return report;
    }

    std::size_t MigrateTubeSourceLengthV76ToV77( std::vector<Assets::EntityData>& entities )
    {
        std::size_t stated = 0;
        for ( auto& entity : entities )
        {
            for ( const char* block : { "PointLight", "SpotLight" } )
            {
                const auto payload = entity.Components.get( block );
                if ( !payload.has_value() )
                    continue;
                auto fields = payload.value().to_object();
                if ( !fields.has_value() || fields.value().get( "SourceLength" ).has_value() )
                    continue;
                fields.value()["SourceLength"] = rfl::Generic( 0.0 );
                entity.Components[block]       = rfl::Generic( std::move( fields.value() ) );
                ++stated;
            }
        }
        return stated;
    }


    ReflectedAssetRefsReport MigrateReflectedAssetRefsV0ToV1( std::vector<Assets::EntityData>& entities,
                                                              const MaterialGuidLookup&        materialGuid )
    {
        ReflectedAssetRefsReport report;
        // The header GUID of the `.demat` an "assets:" key names, read once per key; nullopt = refused.
        std::map<std::string, std::optional<std::string>> guids;
        const auto guidOf = [&]( const std::string& key, const std::string& who ) -> std::optional<std::string>
        {
            if ( const auto found = guids.find( key ); found != guids.end() )
                return found->second;
            std::optional<std::string> guid = materialGuid ? materialGuid( key ) : std::nullopt;
            if ( guid && !Common::Content::AssetGuidFromText( *guid ) )
                guid.reset();
            if ( !guid )
                report.Refused.push_back( std::format( "{}: material '{}' states no header GUID", who, key ) );
            guids.emplace( key, guid );
            return guid;
        };
        const auto ref = []( const std::string& guid, const std::string& path )
        {
            rfl::Generic::Object one;
            one["Guid"] = rfl::Generic( guid );
            one["Path"] = rfl::Generic( path );
            return rfl::Generic( std::move( one ) );
        };
        const auto texts = [&]( const rfl::Generic::Object& block, const char* key, const std::string& who,
                                std::vector<std::string>& out ) -> bool
        {
            const auto stated = block.get( key );
            if ( !stated.has_value() )
                return true;
            const auto array = stated.value().to_array();
            if ( !array.has_value() )
            {
                report.Refused.push_back( std::format( "{}: StaticMesh.{} is not an array", who, key ) );
                return false;
            }
            for ( const rfl::Generic& item : array.value() )
                out.push_back( item.to_string().value_or( std::string() ) );
            return true;
        };
        const auto raise = [&]( rfl::ExtraFields<rfl::Generic>& components, const std::string& who )
        {
            EditBlock( components, "StaticMesh",
                       [&]( rfl::Generic::Object& block )
                       {
                           const auto text = [&]( const char* key )
                           {
                               const auto v = block.get( key );
                               return v.has_value() ? v.value().to_string().value_or( std::string() )
                                                    : std::string();
                           };
                           const bool statesMesh = block.get( "MeshGuid" ).has_value() ||
                                                   block.get( "MeshPath" ).has_value();
                           std::vector<std::string> slotGuids;
                           std::vector<std::string> slotPaths;
                           if ( !texts( block, "MaterialGuids", who, slotGuids ) ||
                                !texts( block, "MaterialPaths", who, slotPaths ) )
                               return false;
                           const bool statesSlots = block.get( "MaterialGuids" ).has_value() ||
                                                    block.get( "MaterialPaths" ).has_value();
                           if ( !statesMesh && !statesSlots && block.get( "MeshHandle" ).has_value() )
                               return false; // already v1
                           const std::string meshGuid = text( "MeshGuid" );
                           const std::string meshPath = text( "MeshPath" );
                           for ( const char* key : { "MeshGuid", "MeshPath", "MaterialGuids", "MaterialPaths" } )
                               DropKey( block, key );
                           // Both keys always, as the save writes them (reflection states every field): a
                           // primitive's MeshHandle is the empty {"", ""}, so a migrated scene and the same
                           // scene saved again are byte-equal.
                           block["MeshHandle"] = ref( meshGuid, meshPath );
                           rfl::Generic::Array slots;
                           const std::size_t   count = std::max( slotGuids.size(), slotPaths.size() );
                           for ( std::size_t i = 0; i < count; ++i )
                               slots.push_back( ref( i < slotGuids.size() ? slotGuids[i] : std::string(),
                                                     i < slotPaths.size() ? slotPaths[i] : std::string() ) );
                           block["MaterialSlots"] = rfl::Generic( std::move( slots ) );
                           ++report.MeshBlocks;
                           return true;
                       } );
            // The reflected Asset<MaterialAsset> fields, stated as a bare key until v1.
            constexpr std::array<std::pair<const char*, const char*>, 4> kMaterialFields = {
                 { { "LandscapeMaterial", "Material" },
                   { "VolumetricCloud", "Material" },
                   { "Destructible", "InteriorMaterial" },
                   { "UIPanel", "Material" } } };
            for ( const auto& [component, field] : kMaterialFields )
                EditBlock( components, component,
                           [&]( rfl::Generic::Object& block )
                           {
                               const auto stated = block.get( field );
                               if ( !stated.has_value() )
                                   return false;
                               const auto key = stated.value().to_string();
                               if ( !key.has_value() )
                                   return false; // already an object: {Guid, Path}
                               std::string guid;
                               if ( !key.value().empty() )
                               {
                                   const auto found =
                                        guidOf( key.value(), std::format( "{} / {}.{}", who, component, field ) );
                                   if ( !found )
                                       return false;
                                   guid = *found;
                               }
                               block[field] = ref( guid, key.value() );
                               ++report.MaterialFields;
                               return true;
                           } );
        };
        for ( auto& entity : entities )
        {
            const std::string who = entity.id.has_value() ? std::format( "entity {}", entity.id->ToString() )
                                                          : std::string( "<record without id>" );
            raise( entity.Components, who );
            if ( entity.PrefabOverrides )
                for ( auto& override_ : *entity.PrefabOverrides )
                    raise( override_.Components, who + " (prefab override)" );
        }
        return report;
    }

    SunRotationReport MigrateLightRotationV0ToV1( std::vector<Assets::EntityData>& entities )
    {
        // The old engine's gate (Rules::kSunDirectionEpsilon before light v1): below it a Translation was no
        // direction and the light emitted nothing.
        constexpr float kOldDirectionEpsilon = 1e-4f;

        std::unordered_map<Common::UUID, const Assets::EntityData*> byId;
        for ( const auto& entity : entities )
            if ( entity.id.has_value() )
                byId.emplace( *entity.id, &entity );

        // The world rotation of a record as the file states it: its own Rotation under its parents'. Bounded
        // by the record count, so a parent cycle in a damaged file ends instead of spinning.
        const auto worldRotation = [&]( std::optional<Common::UUID> id )
        {
            glm::quat world = glm::quat( 1.0f, glm::vec3( 0.0f ) );
            for ( std::size_t depth = 0; id.has_value() && depth <= entities.size(); ++depth )
            {
                const auto found = byId.find( *id );
                if ( found == byId.end() )
                    break;
                world = glm::quat( found->second->Rotation.value_or( glm::vec3( 0.0f ) ) ) * world;
                id    = found->second->parent;
            }
            return world;
        };

        const auto isLight = []( Common::Json::KeyedValues& components )
        { return components.get( "DirectionLight" ).has_value(); };

        SunRotationReport report;
        for ( auto& entity : entities )
        {
            if ( isLight( entity.Components ) )
            {
                const glm::vec3 travel = entity.Translation.value_or( glm::vec3( 0.0f ) );
                if ( glm::length( travel ) > kOldDirectionEpsilon )
                {
                    const glm::quat parent = worldRotation( entity.parent );
                    entity.Rotation =
                         glm::eulerAngles( glm::inverse( parent ) * Geometry::RotationForTravel( travel ) );
                    entity.Translation = glm::vec3( 0.0f );
                    ++report.Aimed;
                }
                else
                {
                    ++report.Undirected;
                }
            }

            if ( !entity.PrefabOverrides )
                continue;
            for ( auto& override_ : *entity.PrefabOverrides )
            {
                if ( !isLight( override_.Components ) || !override_.Translation.has_value() ||
                     glm::length( *override_.Translation ) <= kOldDirectionEpsilon )
                    continue;
                override_.Rotation = glm::eulerAngles( Geometry::RotationForTravel( *override_.Translation ) );
                override_.Translation = glm::vec3( 0.0f );
                ++report.Overrides;
            }
        }
        return report;
    }

    MaterialGuidLookup ContentMaterialGuidLookup()
    {
        return []( const std::string& key ) -> std::optional<std::string>
        {
            const std::filesystem::path file = Common::AssetHandle::PathForStableKey( key );
            if ( file.empty() )
                return std::nullopt;
            const Common::Content::AssetGuid guid = Assets::ReadTextHeaderGuid( file );
            if ( guid == Common::Content::AssetGuid{} )
                return std::nullopt;
            return Common::Content::AssetGuidToText( guid );
        };
    }

    SceneGenerations StatedGenerations( const std::optional<Common::Content::TextAssetHeaderSerialized>& header )
    {
        return SceneGenerations{ Assets::StatedVersion( header, Assets::kSceneSchemaTag ),
                                 Assets::StatedVersion( header, Assets::kUnitSchemaTag ),
                                 Assets::StatedVersion( header, Assets::kWorldPartitionSchemaTag ),
                                 Assets::StatedVersion( header, Assets::kAssetRefSchemaTag ),
                                 Assets::StatedVersion( header, Assets::kLightSchemaTag ),
                                 Assets::StatedVersion( header, Assets::kPostProcessSchemaTag ) };
    }

    SceneGenerations CurrentGenerations()
    {
        return SceneGenerations{ kSceneVersion,    kUnitVersion,  kWorldPartitionVersion,
                                 kAssetRefVersion, kLightVersion, kPostProcessVersion };
    }

    SceneUpgradeReport UpgradeSceneRecords( std::vector<Assets::EntityData>& entities, const SceneGenerations& from,
                                            const UpgradeTarget target, const std::string_view name,
                                            const MaterialGuidLookup& materialGuid )
    {
        SceneUpgradeReport report;
        report.From                 = from;
        const SceneGenerations head = CurrentGenerations();

        // NEWER than this engine: written by a later build. Nothing is read "as best we can".
        if ( from.Scene > head.Scene || from.Unit > head.Unit || from.WorldPartition > head.WorldPartition ||
             from.AssetRef > head.AssetRef || from.Light > head.Light || from.PostProcess > head.PostProcess )
        {
            report.Refused = std::format(
                 "'{}' is at {}, NEWER than this engine ({}): it was written by a later build. Open it with that "
                 "build or a newer one.",
                 name, GenerationsText( from ), GenerationsText( head ) );
            return report;
        }
        // OLDER than the oldest generation raised in memory: the steps below the floors move data between files.
        const SceneGenerations floor{ kSceneUpgradeFloor, kUnitVersion, kWorldPartitionUpgradeFloor,
                                      kAssetRefUpgradeFloor, kLightUpgradeFloor, kPostProcessUpgradeFloor };
        if ( from.Scene < floor.Scene || from.Unit != head.Unit || from.WorldPartition < floor.WorldPartition ||
             from.AssetRef < floor.AssetRef || from.Light < floor.Light || from.PostProcess < floor.PostProcess )
        {
            report.Refused = std::format(
                 "'{}' is at {}, older than the oldest generation this engine upgrades as it loads ({}). Its steps "
                 "write other files beside it, so it is converted once by Tools/SceneMigrator, which writes the "
                 "current generation.",
                 name, GenerationsText( from ), GenerationsText( floor ) );
            return report;
        }

        // Additive: the records are read with the declared defaults and the next save states them.
        report.VolumetricFogRaised = from.Scene < kSceneVersionVolumetricFog;

        if ( from.Scene < kSceneVersionTracedRequest )
        {
            report.TracedRequestRaised = true;
            report.TracedRequest       = MigrateTracedRequestV71ToV72( entities );
            if ( !report.TracedRequest.Refused.empty() )
            {
                report.Refused = RefusedWhole( name, report.TracedRequest.Refused );
                return report;
            }
        }
        // The same PostProcessVolume keys, after the v72 re-statement they follow.
        if ( from.Scene < kSceneVersionComposedGI )
        {
            report.ComposedGIRaised = true;
            report.ComposedGI       = MigrateComposedGIV72ToV73( entities );
            if ( !report.ComposedGI.Refused.empty() )
            {
                report.Refused = RefusedWhole( name, report.ComposedGI.Refused );
                return report;
            }
        }
        if ( from.Scene < kSceneVersionFilmicDefault )
        {
            report.FilmicDefaultRaised = true;
            report.FilmicDefault       = MigrateFilmicDefaultV73ToV74( entities );
            if ( !report.FilmicDefault.Refused.empty() )
            {
                report.Refused = RefusedWhole( name, report.FilmicDefault.Refused );
                return report;
            }
        }
        if ( from.Scene < kSceneVersionGrassRasterPath )
        {
            report.GrassRasterPathRaised = true;
            report.GrassRasterPath       = MigrateGrassRasterPathV74ToV75( entities );
            if ( !report.GrassRasterPath.Refused.empty() )
            {
                report.Refused = RefusedWhole( name, report.GrassRasterPath.Refused );
                return report;
            }
        }
        if ( from.Scene < kSceneVersionLandscapeSplineEntities )
        {
            report.LandscapeSplineEntitiesRaised = true;
            report.LandscapeSplineEntities =
                 MigrateLandscapeSplineEntitiesV75ToV76( entities, target == UpgradeTarget::Scene );
            if ( !report.LandscapeSplineEntities.Refused.empty() )
            {
                report.Refused = RefusedWhole( name, report.LandscapeSplineEntities.Refused );
                return report;
            }
        }
        if ( from.Scene < kSceneVersionTubeSourceLength )
        {
            report.TubeSourceLengthRaised  = true;
            report.TubeSourceLengthsStated = MigrateTubeSourceLengthV76ToV77( entities );
        }

        // World Partition's own chain above its floor: additive only.
        report.EntityHLODLayerRaised = from.WorldPartition < kWorldPartitionVersionEntityHLODLayer;
        report.ImpostorLayersRaised  = from.WorldPartition < kWorldPartitionVersionImpostorLayers;

        // The asset-reference chain, after the scene's and World Partition's: its numbers are its tag's.
        if ( from.AssetRef < kAssetRefVersionReflected )
        {
            report.ReflectedAssetRefsRaised = true;
            report.ReflectedAssetRefs       = MigrateReflectedAssetRefsV0ToV1( entities, materialGuid );
            if ( !report.ReflectedAssetRefs.Refused.empty() )
            {
                report.Refused = RefusedWhole( name, report.ReflectedAssetRefs.Refused );
                return report;
            }
        }
        // The light chain, after the asset-reference chain.
        if ( from.Light < kLightVersionRotation )
        {
            report.SunRotationRaised = true;
            report.SunRotation       = MigrateLightRotationV0ToV1( entities );
        }
        // The post-process chain, after the light chain: its numbers are its tag's.
        if ( from.PostProcess < kPostProcessVersionBloom )
        {
            report.PostProcessBloomRaised = true;
            report.PostProcessBloom       = MigratePostProcessBloomV0ToV1( entities );
            if ( !report.PostProcessBloom.Refused.empty() )
            {
                report.Refused = RefusedWhole( name, report.PostProcessBloom.Refused );
                return report;
            }
        }
        return report;
    }

    void RestateCurrentGenerations( Common::Content::TextAssetHeaderSerialized& header )
    {
        header.Versions = Common::Content::MakeTextHeader( Common::Content::ContentKind::Scene,
                                                           Common::Content::AssetGuid{}, SceneTextSubsystems() )
                               .Versions;
    }

    SceneUpgradeReport UpgradeScene( SceneSerialized& scene )
    {
        SceneGenerations from = StatedGenerations( scene.Header );
        // World Partition below its floor: every step under v12 (Tools/SceneMigrator, RunWorldPartitionSteps) reads
        // the scene's WorldPartition block and nothing else - grids, HLOD layers, partitions. A scene that states no
        // block (GI_Bistro_Night, WPAR 1) has nothing for them to move, so it starts its chain at the floor instead
        // of being refused for a world it does not have.
        if ( !scene.WorldPartition.has_value() && from.WorldPartition < kWorldPartitionUpgradeFloor )
            from.WorldPartition = kWorldPartitionUpgradeFloor;
        if ( from == CurrentGenerations() )
        {
            SceneUpgradeReport current;
            current.From = from;
            return current;
        }
        SceneUpgradeReport report =
             UpgradeSceneRecords( scene.Entities, from, UpgradeTarget::Scene, scene.SceneName,
                                  ContentMaterialGuidLookup() );
        if ( report.Refused.empty() && scene.Header.has_value() )
            RestateCurrentGenerations( *scene.Header );
        return report;
    }

    std::string DescribeUpgrade( const std::string_view source, const SceneUpgradeReport& report )
    {
        const SceneGenerations head = CurrentGenerations();
        // The scene number leads ("upgraded v71->v77"); a subsystem that moved too follows in brackets.
        std::string scene = std::format( "v{}->v{}", report.From.Scene, head.Scene );
        if ( report.From.WorldPartition != head.WorldPartition )
            scene += std::format( " (world partition v{}->v{})", report.From.WorldPartition, head.WorldPartition );
        if ( report.From.AssetRef != head.AssetRef )
            scene += std::format( " (asset references v{}->v{})", report.From.AssetRef, head.AssetRef );
        if ( report.From.Light != head.Light )
            scene += std::format( " (lights v{}->v{})", report.From.Light, head.Light );
        if ( report.From.PostProcess != head.PostProcess )
            scene += std::format( " (post process v{}->v{})", report.From.PostProcess, head.PostProcess );
        return std::format( "'{}': upgraded {} in memory, save to keep", source, scene );
    }
} // namespace Desert::Core
