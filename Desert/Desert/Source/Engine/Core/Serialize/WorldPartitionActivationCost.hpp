#pragma once

// WHAT MAKING A CELL'S ENTITIES COSTS, MEASURED AS IT HAPPENS (WP-ACTIVATE-SLICED).
//
// Pattern from UE 5.5 Engine/Source/Runtime/Engine/Private/Level.cpp (ULevel::IncrementalRegisterComponents,
// s.LevelStreamingComponentsRegistrationGranularity) and World.cpp (UWorld::AddToWorld, IsTimeLimitExceeded with
// s.LevelStreamingActorsUpdateTimeLimit, s.PriorityLevelStreamingActorsUpdateExtraTime), adapted.
//
// A unit's activation is cut into SLICES (WorldPartitionResidencyExecutor.hpp, THE SLICES): runs of whole
// composites made by one InstantiateRecords, timed on the clock, until the frame's ActivationBudgetMs is spent.
// UE re-reads the clock every N components (the granularity, 10 by default) and so overshoots its limit by up to N
// components of unknown cost — a skinned actor and a decal cost the same to its check. Here each slice is sized
// BEFORE it is made, from what the records of each kind cost when they were last made, so the slice ends inside
// the budget; and the clock is still read after every slice, so a wrong estimate costs one slice, never the frame.
//
// THE ESTIMATE IS A MEASUREMENT, NOT A CONSTANT. Each kind's milliseconds per record is a rolling average of the
// slices that held it (the newest slice weighs kRecentWeight). A kind that was never made this session has NO
// estimate: its first composite is made in a slice of its own, which measures it, and the slices after it are
// sized from that. Before WP-ACTIVATE-SLICED one coefficient (0.02 ms, measured on cubes in WP5b) priced every
// record, so a cell of prefab instances or skinned meshes was promised a frame it did not get.
//
// PURE: no clock, no scene. The executor times the slices and hands the measurements in.

#include <Engine/Assets/Prefab/PrefabData.hpp>
#include <Engine/Core/Serialize/WorldPartitionHLODRules.hpp>
#include <Engine/Core/Serialize/WorldPartitionRules.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Core::Rules
{
    // The costliest thing a record makes when it becomes an entity. One per record, decided from its payload.
    enum class ActivationRecordKind : std::uint8_t
    {
        Plain,          // a transform and whatever is cheap (lights, volumes, scripts, sources)
        StaticMesh,     // a mesh primitive
        Physics,        // a rigid body, collider or character: its Jolt body is made in the slice
        InstancedMesh,  // an instance list (InstancedStaticMesh)
        SkinnedMesh,    // a skeleton and its pose
        Foliage,        // a foliage field
        LandscapeTile,  // a heightfield tile
        PrefabInstance, // a whole prefab file, instantiated
        Count,
    };

    inline constexpr std::size_t kActivationKindCount = static_cast<std::size_t>( ActivationRecordKind::Count );

    [[nodiscard]] inline std::string_view ActivationKindName( ActivationRecordKind kind )
    {
        switch ( kind )
        {
            case ActivationRecordKind::Plain:
                return "plain";
            case ActivationRecordKind::StaticMesh:
                return "static mesh";
            case ActivationRecordKind::Physics:
                return "physics";
            case ActivationRecordKind::InstancedMesh:
                return "instanced mesh";
            case ActivationRecordKind::SkinnedMesh:
                return "skinned mesh";
            case ActivationRecordKind::Foliage:
                return "foliage";
            case ActivationRecordKind::LandscapeTile:
                return "landscape tile";
            case ActivationRecordKind::PrefabInstance:
                return "prefab instance";
            case ActivationRecordKind::Count:
                break;
        }
        return "?";
    }

    // The component keys whose presence makes a record a Physics record (ComponentRegistry keys, Components.hpp).
    inline constexpr std::string_view kRigidBodyComponent           = "RigidBody";
    inline constexpr std::string_view kColliderComponent            = "Collider";
    inline constexpr std::string_view kCharacterControllerComponent = "CharacterController";

    // A COMPONENT WHOSE MODULE STATES ITS RECORD'S KIND (UE: the actor class decides what it costs to stream in),
    // registered at the module's StartupModule (the terrain module: a tile is a heightfield tile); a second row
    // for a component is ignored. Read before this file's own kinds, in registration order.
    struct ActivationKindComponent
    {
        std::string_view     Component;
        ActivationRecordKind Kind = ActivationRecordKind::Plain;
    };

    namespace Detail
    {
        inline std::vector<ActivationKindComponent>& ActivationKindComponentList()
        {
            static std::vector<ActivationKindComponent> rows;
            return rows;
        }
    } // namespace Detail

    inline void RegisterActivationKind( const ActivationKindComponent& row )
    {
        for ( const ActivationKindComponent& known : Detail::ActivationKindComponentList() )
            if ( known.Component == row.Component )
                return;
        if ( !row.Component.empty() )
            Detail::ActivationKindComponentList().push_back( row );
    }

    // The kind of @p record, the costliest first: a prefab instance makes a whole file; a registered kind (a
    // heightfield tile), a foliage field, a skinned mesh and an instance list each build more than a primitive; a
    // body is a primitive plus a Jolt shape.
    [[nodiscard]] inline ActivationRecordKind ActivationKindOf( const Assets::EntityData& record )
    {
        const auto has = [&record]( std::string_view key )
        { return Detail::PayloadOf( record, key ).has_value(); };
        if ( record.PrefabPath.has_value() )
            return ActivationRecordKind::PrefabInstance;
        for ( const ActivationKindComponent& row : Detail::ActivationKindComponentList() )
            if ( has( row.Component ) )
                return row.Kind;
        if ( has( kFoliageComponent ) )
            return ActivationRecordKind::Foliage;
        if ( has( kSkinnedMeshComponent ) )
            return ActivationRecordKind::SkinnedMesh;
        if ( has( kInstancePointsComponent ) )
            return ActivationRecordKind::InstancedMesh;
        if ( has( kRigidBodyComponent ) || has( kColliderComponent ) || has( kCharacterControllerComponent ) )
            return ActivationRecordKind::Physics;
        if ( has( kPrimitiveComponent ) )
            return ActivationRecordKind::StaticMesh;
        return ActivationRecordKind::Plain;
    }

    class ActivationCostModel
    {
    public:
        // The newest slice's share of a kind's estimate: a cell of heavier records moves the estimate within a
        // few slices, and one slice that hit a cold cache does not swing it.
        static constexpr double kRecentWeight = 0.25;

        [[nodiscard]] bool IsMeasured( ActivationRecordKind kind ) const
        {
            return m_Measured[Index( kind )];
        }

        [[nodiscard]] bool AllMeasured( std::span<const ActivationRecordKind> kinds ) const
        {
            return std::all_of( kinds.begin(), kinds.end(),
                                [this]( ActivationRecordKind kind ) { return IsMeasured( kind ); } );
        }

        // Milliseconds to make @p kinds, every one of them measured (AllMeasured).
        [[nodiscard]] double Estimate( std::span<const ActivationRecordKind> kinds ) const
        {
            double ms = 0.0;
            for ( const ActivationRecordKind kind : kinds )
                ms += m_MsPerRecord[Index( kind )];
            return ms;
        }

        // A slice of @p kinds took @p ms. The time is shared out by what each kind was expected to cost; the
        // kinds never measured share what the measured ones leave, per record.
        void Observe( std::span<const ActivationRecordKind> kinds, double ms )
        {
            if ( kinds.empty() )
                return;
            ms = std::max( ms, 0.0 );
            std::array<std::size_t, kActivationKindCount> count{};
            for ( const ActivationRecordKind kind : kinds )
                ++count[Index( kind )];
            double      known      = 0.0;
            std::size_t unmeasured = 0;
            for ( std::size_t kind = 0; kind < kActivationKindCount; ++kind )
            {
                if ( count[kind] == 0 )
                    continue;
                if ( m_Measured[kind] )
                    known += static_cast<double>( count[kind] ) * m_MsPerRecord[kind];
                else
                    unmeasured += count[kind];
            }
            // What one record of each kind took in this slice.
            const double knownScale = unmeasured == 0 ? ( known > 0.0 ? ms / known : 0.0 )
                                                      : ( known > 0.0 ? std::min( 1.0, ms / known ) : 0.0 );
            const double perUnmeasured =
                 unmeasured == 0 ? 0.0
                                 : std::max( ms - known * knownScale, 0.0 ) / static_cast<double>( unmeasured );
            const double evenShare = ms / static_cast<double>( kinds.size() );
            for ( std::size_t kind = 0; kind < kActivationKindCount; ++kind )
            {
                if ( count[kind] == 0 )
                    continue;
                if ( !m_Measured[kind] )
                {
                    m_MsPerRecord[kind] = perUnmeasured;
                    m_Measured[kind]    = true;
                    continue;
                }
                // Every kind measured at zero (a clock too coarse for the slice): the time is shared evenly.
                const double seen = known > 0.0 ? m_MsPerRecord[kind] * knownScale : evenShare;
                m_MsPerRecord[kind] += kRecentWeight * ( seen - m_MsPerRecord[kind] );
            }
            m_TotalMs += ms;
            m_TotalRecords += kinds.size();
        }

        // Milliseconds per record over every slice made so far; nullopt before the first.
        [[nodiscard]] std::optional<double> MeanMsPerRecord() const
        {
            if ( m_TotalRecords == 0 )
                return std::nullopt;
            return m_TotalMs / static_cast<double>( m_TotalRecords );
        }

        // "static mesh 0.0150 ms, prefab instance 0.2100 ms" — the measured kinds, for the end-of-Play line.
        [[nodiscard]] std::string Describe() const
        {
            std::string out;
            for ( std::size_t kind = 0; kind < kActivationKindCount; ++kind )
            {
                if ( !m_Measured[kind] )
                    continue;
                out += std::format( "{}{} {:.4f} ms", out.empty() ? "" : ", ",
                                    ActivationKindName( static_cast<ActivationRecordKind>( kind ) ),
                                    m_MsPerRecord[kind] );
            }
            return out.empty() ? std::string( "nothing measured" ) : out;
        }

    private:
        [[nodiscard]] static std::size_t Index( ActivationRecordKind kind )
        {
            return static_cast<std::size_t>( kind );
        }

        std::array<double, kActivationKindCount> m_MsPerRecord{};
        std::array<bool, kActivationKindCount>   m_Measured{};
        double                                   m_TotalMs      = 0.0;
        std::size_t                              m_TotalRecords = 0;
    };

    // THE NEXT SLICE of a unit whose members [@p cursor, end) are still to be made: the member index (exclusive)
    // it ends at, always on a composite boundary — @p compositeEnds are the unit's composites' ends in member
    // order, ascending, the last one the unit's size — because a parent and its child meet only in the same
    // InstantiateRecords (a composite is never divided, WorldPartitionRules.hpp). At least one composite.
    //  * A composite holding a kind never measured is a slice of its own: it is the measurement.
    //  * Otherwise composites are taken while their estimate fits @p remainingMs, and the slice stops before
    //    a composite holding a kind never measured.
    [[nodiscard]] inline std::size_t NextActivationSlice( std::span<const std::size_t>          compositeEnds,
                                                          std::span<const ActivationRecordKind> kinds,
                                                          std::size_t cursor, const ActivationCostModel& cost,
                                                          double remainingMs )
    {
        const auto first = std::upper_bound( compositeEnds.begin(), compositeEnds.end(), cursor );
        if ( first == compositeEnds.end() )
            return cursor;
        if ( !cost.AllMeasured( kinds.subspan( cursor, *first - cursor ) ) )
            return *first;
        double      spent = cost.Estimate( kinds.subspan( cursor, *first - cursor ) );
        std::size_t end   = *first;
        for ( auto next = std::next( first ); next != compositeEnds.end(); ++next )
        {
            const auto composite = kinds.subspan( end, *next - end );
            if ( !cost.AllMeasured( composite ) )
                break;
            const double ms = cost.Estimate( composite );
            if ( spent + ms > remainingMs )
                break;
            spent += ms;
            end = *next;
        }
        return end;
    }
} // namespace Desert::Core::Rules
