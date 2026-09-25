// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/CompGeom/PolygonTriangulation.cpp:175-371,
// adapted: namespace Desert::Geometry::PolygonTriangulation; UE's file-level FVector VectorsOnSameSide/
// PointInTriangle duplicates are dropped (the templated Local3 copies are what the 3D path uses); SMALL_NUMBER is
// spelled out; UE's check(OutTriangles.Num() > 0) is implied by the loop (it always emits the last three).
#include "Engine/Geometry/UECore/CompGeom/PolygonTriangulation.hpp"

#include "Engine/Geometry/UECore/VectorUtil.hpp"

namespace Desert::Geometry::PolygonTriangulation
{
    template <typename T>
    T ComputePolygonPlane( const TArray<TVector<T>>& VertexPositions, TVector<T>& PlaneNormalOut,
                           TVector<T>& PlanePointOut )
    {
        PlaneNormalOut          = TVector<T>::Zero();
        PlanePointOut           = TVector<T>::Zero();
        const int32_t NumVertices = VertexPositions.Num();
        // Newell's method: the sum over edges gives twice the projected area on each axis plane.
        for ( int32_t VertexNumberI = NumVertices - 1, VertexNumberJ = 0; VertexNumberJ < NumVertices;
              VertexNumberI = VertexNumberJ++ )
        {
            const TVector<T>& PositionI = VertexPositions[VertexNumberI];
            const TVector<T>& PositionJ = VertexPositions[VertexNumberJ];
            PlanePointOut += PositionJ;
            PlaneNormalOut.X += ( PositionJ.Y - PositionI.Y ) * ( PositionI.Z + PositionJ.Z );
            PlaneNormalOut.Y += ( PositionJ.Z - PositionI.Z ) * ( PositionI.X + PositionJ.X );
            PlaneNormalOut.Z += ( PositionJ.X - PositionI.X ) * ( PositionI.Y + PositionJ.Y );
        }
        PlanePointOut /= static_cast<T>( NumVertices );
        return static_cast<T>( 0.5 ) * Normalize( PlaneNormalOut );
    }

    template <typename T>
    void TriangulateSimplePolygon( const TArray<TVector<T>>& VertexPositions, TArray<FIndex3i>& OutTriangles,
                                   bool bOrientAsHoleFill )
    {
        struct Local3
        {
            static bool IsTriangleFlipped( const TVector<T>& ReferenceNormal, const TVector<T>& VertexPositionA,
                                           const TVector<T>& VertexPositionB, const TVector<T>& VertexPositionC )
            {
                const TVector<T> TriangleNormal =
                     VectorUtil::Normal( VertexPositionA, VertexPositionB, VertexPositionC );
                return TriangleNormal.Dot( ReferenceNormal ) <= static_cast<T>( 0 );
            }

            static bool VectorsOnSameSide( const TVector<T>& Vec, const TVector<T>& A, const TVector<T>& B,
                                           T SameSideDotProductEpsilon )
            {
                const TVector<T> CrossA         = Vec.Cross( A );
                const TVector<T> CrossB         = Vec.Cross( B );
                const T          DotWithEpsilon = SameSideDotProductEpsilon + CrossA.Dot( CrossB );
                return DotWithEpsilon >= 0;
            }

            static bool PointInTriangle( const TVector<T>& A, const TVector<T>& B, const TVector<T>& C,
                                         const TVector<T>& P, const T InsideTriangleDotProductEpsilon )
            {
                return ( VectorsOnSameSide( B - A, P - A, C - A, InsideTriangleDotProductEpsilon ) &&
                         VectorsOnSameSide( C - B, P - B, A - B, InsideTriangleDotProductEpsilon ) &&
                         VectorsOnSameSide( A - C, P - C, B - C, InsideTriangleDotProductEpsilon ) );
            }
        };

        // UE's SMALL_NUMBER.
        constexpr T InsideTriangleEpsilon = static_cast<T>( 1.e-8 );

        OutTriangles.Reset();

        const int32_t PolygonVertexCount = VertexPositions.Num();
        if ( PolygonVertexCount < 3 )
        {
            return;
        }
        if ( PolygonVertexCount == 3 )
        {
            OutTriangles.Add( bOrientAsHoleFill ? FIndex3i( 0, 2, 1 ) : FIndex3i( 0, 1, 2 ) );
            return;
        }

        // Polygon plane normal, the reference for "not flipped" ears.
        TVector<T> PolygonNormal;
        TVector<T> PolygonCentroid;
        ComputePolygonPlane( VertexPositions, PolygonNormal, PolygonCentroid );

        // A doubly-linked ring over the vertex numbers; clipping an ear unlinks its tip.
        TArray<int32_t> PrevVertexNumbers;
        TArray<int32_t> NextVertexNumbers;
        PrevVertexNumbers.SetNumUninitialized( PolygonVertexCount, EAllowShrinking::No );
        NextVertexNumbers.SetNumUninitialized( PolygonVertexCount, EAllowShrinking::No );
        for ( int32_t VertexNumber = 0; VertexNumber < PolygonVertexCount; ++VertexNumber )
        {
            PrevVertexNumbers[VertexNumber] = VertexNumber - 1;
            NextVertexNumbers[VertexNumber] = VertexNumber + 1;
        }
        PrevVertexNumbers[0]                      = PolygonVertexCount - 1;
        NextVertexNumbers[PolygonVertexCount - 1] = 0;

        int32_t EarVertexNumber = 0;
        int32_t EarTestCount    = 0;
        for ( int32_t RemainingVertexCount = PolygonVertexCount; RemainingVertexCount >= 3; )
        {
            bool bIsEar = true;

            // With more than three vertices left, the ear must be convex and contain no other remaining vertex.
            // After a full lap with no ear found (EarTestCount), the polygon is degenerate: clip anyway.
            if ( RemainingVertexCount > 3 && EarTestCount < RemainingVertexCount )
            {
                const TVector<T>& PrevVertexPosition = VertexPositions[PrevVertexNumbers[EarVertexNumber]];
                const TVector<T>& EarVertexPosition  = VertexPositions[EarVertexNumber];
                const TVector<T>& NextVertexPosition = VertexPositions[NextVertexNumbers[EarVertexNumber]];

                if ( !Local3::IsTriangleFlipped( PolygonNormal, PrevVertexPosition, EarVertexPosition,
                                                 NextVertexPosition ) )
                {
                    int32_t TestVertexNumber = NextVertexNumbers[NextVertexNumbers[EarVertexNumber]];
                    do
                    {
                        const TVector<T>& TestVertexPosition = VertexPositions[TestVertexNumber];
                        if ( Local3::PointInTriangle( PrevVertexPosition, EarVertexPosition, NextVertexPosition,
                                                      TestVertexPosition, InsideTriangleEpsilon ) )
                        {
                            bIsEar = false;
                            break;
                        }
                        TestVertexNumber = NextVertexNumbers[TestVertexNumber];
                    } while ( TestVertexNumber != PrevVertexNumbers[EarVertexNumber] );
                }
                else
                {
                    bIsEar = false;
                }
            }

            if ( bIsEar )
            {
                {
                    const int32_t A = PrevVertexNumbers[EarVertexNumber];
                    const int32_t B = EarVertexNumber;
                    const int32_t C = NextVertexNumbers[EarVertexNumber];
                    OutTriangles.Add( bOrientAsHoleFill ? FIndex3i( A, C, B ) : FIndex3i( A, B, C ) );
                }
                NextVertexNumbers[PrevVertexNumbers[EarVertexNumber]] = NextVertexNumbers[EarVertexNumber];
                PrevVertexNumbers[NextVertexNumbers[EarVertexNumber]] = PrevVertexNumbers[EarVertexNumber];
                --RemainingVertexCount;

                // Step back so the next test also re-checks the neighbour whose ear just changed.
                EarVertexNumber = PrevVertexNumbers[EarVertexNumber];
                EarTestCount    = 0;
            }
            else
            {
                EarVertexNumber = NextVertexNumbers[EarVertexNumber];
                ++EarTestCount;
            }
        }
    }

    template float  ComputePolygonPlane<float>( const TArray<TVector<float>>&, TVector<float>&, TVector<float>& );
    template double ComputePolygonPlane<double>( const TArray<TVector<double>>&, TVector<double>&,
                                                 TVector<double>& );
    template void   TriangulateSimplePolygon<float>( const TArray<TVector<float>>&, TArray<FIndex3i>&, bool );
    template void   TriangulateSimplePolygon<double>( const TArray<TVector<double>>&, TArray<FIndex3i>&, bool );
} // namespace Desert::Geometry::PolygonTriangulation
