#include <Engine/Core/SceneUpgrade/Steps/V76LandscapeSplineEntities.hpp>

#include <Engine/Core/SceneUpgrade/Steps/StepEdits.hpp>

#include <cstdint>
#include <utility>

#include <format>
#include <string>

namespace Desert::Core
{
    namespace
    {
        using UpgradeEdits::DropKey;
        using UpgradeEdits::EditBlock;
    } // namespace

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

    StepOutcome RunLandscapeSplineEntities( UpgradeContext& context )
    {
        LandscapeSplineEntitiesReport report =
             MigrateLandscapeSplineEntitiesV75ToV76( context.Entities, context.Target == UpgradeTarget::Scene );
        StepOutcome outcome;
        outcome.Changed = report.RootsCleared + report.SplinesCreated;
        outcome.Summary = std::format( "{} landscape spline entit(ies) split out of {} landscape root(s)",
                                       report.SplinesCreated, report.RootsCleared );
        outcome.Refused = std::move( report.Refused );
        return outcome;
    }
} // namespace Desert::Core
