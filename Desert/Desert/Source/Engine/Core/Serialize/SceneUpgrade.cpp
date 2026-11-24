#include <Engine/Core/Serialize/SceneUpgrade.hpp>

#include <Common/Settings/RenderMethods.hpp>

#include <rflcpp/rfl/json.hpp>

#include <array>
#include <cstdint>
#include <format>
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
                                "lights v{}",
                                g.Scene, g.Unit, g.WorldPartition, g.AssetRef, g.Light );
        }

        // The step numbers, frozen: a step restates the numbers of ITS generation, so a later renumbering of an
        // enum cannot change what an old file becomes.
        constexpr int kTonemapACES                 = 0; // Core::TonemapOperator::ACES at v74
        constexpr int kTonemapFilmic               = 2; // Core::TonemapOperator::Filmic at v74
        constexpr int kGrassRasterScalability      = 0; // Core::GrassRasterPath::Scalability at v75
        constexpr int kGrassRasterVisibilityBuffer = 2; // Core::GrassRasterPath::VisibilityBuffer at v75
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


    SceneGenerations StatedGenerations( const std::optional<Common::Content::TextAssetHeaderSerialized>& header )
    {
        return SceneGenerations{ Assets::StatedVersion( header, Assets::kSceneSchemaTag ),
                                 Assets::StatedVersion( header, Assets::kUnitSchemaTag ),
                                 Assets::StatedVersion( header, Assets::kWorldPartitionSchemaTag ),
                                 Assets::StatedVersion( header, Assets::kAssetRefSchemaTag ),
                                 Assets::StatedVersion( header, Assets::kLightSchemaTag ) };
    }

    SceneGenerations CurrentGenerations()
    {
        return SceneGenerations{ kSceneVersion, kUnitVersion, kWorldPartitionVersion, kAssetRefVersion,
                                 kLightVersion };
    }

    SceneUpgradeReport UpgradeSceneRecords( std::vector<Assets::EntityData>& entities, const SceneGenerations& from,
                                            const UpgradeTarget target, const std::string_view name )
    {
        SceneUpgradeReport report;
        report.From                 = from;
        const SceneGenerations head = CurrentGenerations();

        // NEWER than this engine: written by a later build. Nothing is read "as best we can".
        if ( from.Scene > head.Scene || from.Unit > head.Unit || from.WorldPartition > head.WorldPartition ||
             from.AssetRef > head.AssetRef || from.Light > head.Light )
        {
            report.Refused = std::format(
                 "'{}' is at {}, NEWER than this engine ({}): it was written by a later build. Open it with that "
                 "build or a newer one.",
                 name, GenerationsText( from ), GenerationsText( head ) );
            return report;
        }
        // OLDER than the oldest generation raised in memory: the steps below the floors move data between files.
        const SceneGenerations floor{ kSceneUpgradeFloor, kUnitVersion, kWorldPartitionUpgradeFloor,
                                      kAssetRefUpgradeFloor, kLightUpgradeFloor };
        if ( from.Scene < floor.Scene || from.Unit != head.Unit || from.WorldPartition < floor.WorldPartition ||
             from.AssetRef < floor.AssetRef || from.Light < floor.Light )
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
        const SceneGenerations from = StatedGenerations( scene.Header );
        if ( from == CurrentGenerations() )
        {
            SceneUpgradeReport current;
            current.From = from;
            return current;
        }
        SceneUpgradeReport report =
             UpgradeSceneRecords( scene.Entities, from, UpgradeTarget::Scene, scene.SceneName );
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
        return std::format( "'{}': upgraded {} in memory, save to keep", source, scene );
    }
} // namespace Desert::Core
