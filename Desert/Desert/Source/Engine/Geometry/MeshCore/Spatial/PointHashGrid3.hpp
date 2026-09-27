// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Spatial/PointHashGrid3.h:216-229
// (FindNearestInRadius), 256-300 (FindPointsInBall), 365-431 (FindInRadiusHelper, nearest only),
// InsertPointUnsafe, adapted: glm, namespace Desert::Geometry, int32_t values at double precision only
// (TPointHashGrid3<int32_t, double> with FScaleGridIndexer3 at the origin), std::map of cells (sorted, so a query
// visits values in a fixed order) in place of TMultiMap, no lock (the Unsafe inserts are the only ones anybody
// calls), invalid value -1. Lifted out of MergeCoincidentMeshEdges.cpp, where it was private, for MeshBoolean's
// vertex matching.
#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace Desert::Geometry
{
    class PointHashGrid3
    {
    public:
        static constexpr int32_t InvalidValue = -1;

        explicit PointHashGrid3( double CellSize ) : m_CellSize( CellSize )
        {
        }

        void InsertPointUnsafe( int32_t Value, const glm::dvec3& Pos )
        {
            m_Hash[ToGrid( Pos )].push_back( Value );
        }

        template <typename DistanceSqFn>
        void FindPointsInBall( const glm::dvec3& QueryPoint, double Radius, DistanceSqFn&& DistanceSqFunc,
                               std::vector<int32_t>& ResultsOut ) const
        {
            const double RadiusSquared = Radius * Radius;
            IterateAcrossBounds( QueryPoint - glm::dvec3( Radius ), QueryPoint + glm::dvec3( Radius ),
                                 [&]( const Key& Idx )
                                 {
                                     const auto It = m_Hash.find( Idx );
                                     if ( It == m_Hash.end() )
                                         return;
                                     for ( int32_t const Value : It->second )
                                         if ( DistanceSqFunc( Value ) < RadiusSquared )
                                             ResultsOut.push_back( Value );
                                 } );
        }

        // The value nearest QueryPoint strictly within Radius, with its squared distance; {InvalidValue, max}
        // when there is none. The centre cell is searched first and shrinks the ball the rest is searched in.
        template <typename DistanceSqFn>
        std::pair<int32_t, double> FindNearestInRadius( const glm::dvec3& QueryPoint, double Radius,
                                                        DistanceSqFn&& DistanceSqFunc ) const
        {
            if ( m_Hash.empty() )
                return { InvalidValue, std::numeric_limits<double>::max() };

            double  MinDistSq  = Radius * Radius;
            int32_t Nearest    = InvalidValue;
            auto    SearchCell = [&]( const Key& CellIdx )
            {
                bool       bFound = false;
                const auto It     = m_Hash.find( CellIdx );
                if ( It == m_Hash.end() )
                    return false;
                for ( int32_t const Value : It->second )
                {
                    const double DistSq = DistanceSqFunc( Value );
                    if ( DistSq < MinDistSq )
                    {
                        Nearest   = Value;
                        MinDistSq = DistSq;
                        bFound    = true;
                    }
                }
                return bFound;
            };

            const Key CenterIdx    = ToGrid( QueryPoint );
            double    SearchRadius = Radius;
            if ( SearchCell( CenterIdx ) )
                SearchRadius = std::sqrt( MinDistSq );
            IterateAcrossBounds( QueryPoint - glm::dvec3( SearchRadius ), QueryPoint + glm::dvec3( SearchRadius ),
                                 [&]( const Key& Idx )
                                 {
                                     if ( Idx != CenterIdx )
                                         SearchCell( Idx );
                                 } );
            if ( Nearest == InvalidValue )
                MinDistSq = std::numeric_limits<double>::max();
            return { Nearest, MinDistSq };
        }

    private:
        using Key = std::array<int64_t, 3>;

        [[nodiscard]] Key ToGrid( const glm::dvec3& P ) const
        {
            return { static_cast<int64_t>( std::floor( P.x / m_CellSize ) ),
                     static_cast<int64_t>( std::floor( P.y / m_CellSize ) ),
                     static_cast<int64_t>( std::floor( P.z / m_CellSize ) ) };
        }

        template <typename CellFn>
        void IterateAcrossBounds( const glm::dvec3& Lo, const glm::dvec3& Hi, CellFn&& Fn ) const
        {
            const Key MinIdx = ToGrid( Lo );
            const Key MaxIdx = ToGrid( Hi );
            for ( int64_t zi = MinIdx[2]; zi <= MaxIdx[2]; zi++ )
                for ( int64_t yi = MinIdx[1]; yi <= MaxIdx[1]; yi++ )
                    for ( int64_t xi = MinIdx[0]; xi <= MaxIdx[0]; xi++ )
                        Fn( Key{ xi, yi, zi } );
        }

        double                              m_CellSize;
        std::map<Key, std::vector<int32_t>> m_Hash;
    };
} // namespace Desert::Geometry
