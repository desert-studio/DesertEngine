// Ported from UE 5.8 .../GeometryCore/Private/CompGeom/ExactPredicates.cpp + ThirdParty Shewchuk predicates (see
// the header for what was adapted).
#include "Engine/Geometry/MeshCore/CompGeom/ExactPredicates.hpp"

#include <cmath>
#include <limits>
#include <vector>

using namespace Desert::Geometry;

namespace
{
    // Shewchuk: epsilon = 2^-53 (half an ulp of 1), the bound on the relative error of one rounded operation.
    constexpr double Epsilon      = std::numeric_limits<double>::epsilon() * 0.5;
    constexpr double CcwErrBoundA = ( 3.0 + 16.0 * Epsilon ) * Epsilon;
    constexpr double IccErrBoundA = ( 10.0 + 96.0 * Epsilon ) * Epsilon;

    // A nonoverlapping expansion: components sorted by increasing magnitude, their exact sum is the value.
    using Expansion = std::vector<double>;

    // Knuth's TwoSum: X + Y == Hi + Lo exactly.
    void TwoSum( double X, double Y, double& Hi, double& Lo )
    {
        Hi                 = X + Y;
        const double BVirt = Hi - X;
        const double AVirt = Hi - BVirt;
        Lo                 = ( X - AVirt ) + ( Y - BVirt );
    }

    // X * Y == Hi + Lo exactly (fma gives the rounding error of the product).
    void TwoProduct( double X, double Y, double& Hi, double& Lo )
    {
        Hi = X * Y;
        Lo = std::fma( X, Y, -Hi );
    }

    Expansion FromDiff( double X, double Y )
    {
        double Hi;
        double Lo;
        TwoSum( X, -Y, Hi, Lo );
        Expansion Out;
        if ( Lo != 0.0 )
            Out.push_back( Lo );
        if ( Hi != 0.0 )
            Out.push_back( Hi );
        return Out;
    }

    // Shewchuk's grow_expansion_zeroelim (Theorem 10): E + B exactly, E nonoverlapping in increasing magnitude.
    Expansion Grow( const Expansion& E, double B )
    {
        Expansion Out;
        Out.reserve( E.size() + 1 );
        double Q = B;
        for ( const double Component : E )
        {
            double Hi;
            double Lo;
            TwoSum( Q, Component, Hi, Lo );
            if ( Lo != 0.0 )
                Out.push_back( Lo );
            Q = Hi;
        }
        if ( Q != 0.0 )
            Out.push_back( Q );
        return Out;
    }

    // Shewchuk's expansion_sum_zeroelim (Theorem 12), as one Grow per component of F.
    Expansion Sum( const Expansion& E, const Expansion& F )
    {
        Expansion Out = E;
        for ( const double Component : F )
            Out = Grow( Out, Component );
        return Out;
    }

    Expansion Negate( Expansion E )
    {
        for ( double& C : E )
            C = -C;
        return E;
    }

    // Shewchuk's scale_expansion_zeroelim: E * B exactly.
    Expansion Scale( const Expansion& E, double B )
    {
        Expansion Out;
        for ( size_t K = 0; K < E.size(); ++K )
        {
            double ProdHi;
            double ProdLo;
            TwoProduct( E[K], B, ProdHi, ProdLo );
            Expansion Term{ ProdLo, ProdHi };
            Out = Sum( Out, Term );
        }
        return Out;
    }

    Expansion Mul( const Expansion& E, const Expansion& F )
    {
        Expansion Out;
        for ( const double C : F )
            Out = Sum( Out, Scale( E, C ) );
        return Out;
    }

    // The exact sign of an expansion = the sign of the component with the largest magnitude.
    double Sign( const Expansion& E )
    {
        double Largest = 0.0;
        for ( const double C : E )
            if ( std::abs( C ) > std::abs( Largest ) )
                Largest = C;
        return Largest > 0.0 ? 1.0 : ( Largest < 0.0 ? -1.0 : 0.0 );
    }

    double Orient2Exact( const glm::dvec2& A, const glm::dvec2& B, const glm::dvec2& C )
    {
        const Expansion ACX = FromDiff( A.x, C.x );
        const Expansion ACY = FromDiff( A.y, C.y );
        const Expansion BCX = FromDiff( B.x, C.x );
        const Expansion BCY = FromDiff( B.y, C.y );
        return Sign( Sum( Mul( ACX, BCY ), Negate( Mul( ACY, BCX ) ) ) );
    }

    double InCircle2Exact( const glm::dvec2& A, const glm::dvec2& B, const glm::dvec2& C, const glm::dvec2& D )
    {
        const Expansion ADX = FromDiff( A.x, D.x );
        const Expansion ADY = FromDiff( A.y, D.y );
        const Expansion BDX = FromDiff( B.x, D.x );
        const Expansion BDY = FromDiff( B.y, D.y );
        const Expansion CDX = FromDiff( C.x, D.x );
        const Expansion CDY = FromDiff( C.y, D.y );

        const Expansion ALift = Sum( Mul( ADX, ADX ), Mul( ADY, ADY ) );
        const Expansion BLift = Sum( Mul( BDX, BDX ), Mul( BDY, BDY ) );
        const Expansion CLift = Sum( Mul( CDX, CDX ), Mul( CDY, CDY ) );

        const Expansion BC = Sum( Mul( BDX, CDY ), Negate( Mul( BDY, CDX ) ) );
        const Expansion CA = Sum( Mul( CDX, ADY ), Negate( Mul( CDY, ADX ) ) );
        const Expansion AB = Sum( Mul( ADX, BDY ), Negate( Mul( ADY, BDX ) ) );

        return Sign( Sum( Sum( Mul( ALift, BC ), Mul( BLift, CA ) ), Mul( CLift, AB ) ) );
    }
} // namespace

double ExactPredicates::Orient2( const glm::dvec2& A, const glm::dvec2& B, const glm::dvec2& C )
{
    // Shewchuk orient2d, stage A filter.
    const double DetLeft  = ( A.x - C.x ) * ( B.y - C.y );
    const double DetRight = ( A.y - C.y ) * ( B.x - C.x );
    const double Det      = DetLeft - DetRight;
    double       DetSum;
    if ( DetLeft > 0.0 )
    {
        if ( DetRight <= 0.0 )
            return Det;
        DetSum = DetLeft + DetRight;
    }
    else if ( DetLeft < 0.0 )
    {
        if ( DetRight >= 0.0 )
            return Det;
        DetSum = -DetLeft - DetRight;
    }
    else
        return Det == 0.0 ? Orient2Exact( A, B, C ) : Det;
    const double ErrBound = CcwErrBoundA * DetSum;
    if ( Det >= ErrBound || -Det >= ErrBound )
        return Det;
    return Orient2Exact( A, B, C );
}

double ExactPredicates::InCircle2( const glm::dvec2& A, const glm::dvec2& B, const glm::dvec2& C,
                                   const glm::dvec2& D )
{
    // Shewchuk incircle, stage A filter.
    const double ADX = A.x - D.x;
    const double BDX = B.x - D.x;
    const double CDX = C.x - D.x;
    const double ADY = A.y - D.y;
    const double BDY = B.y - D.y;
    const double CDY = C.y - D.y;

    const double BDXCDY = BDX * CDY;
    const double CDXBDY = CDX * BDY;
    const double ALift  = ADX * ADX + ADY * ADY;
    const double CDXADY = CDX * ADY;
    const double ADXCDY = ADX * CDY;
    const double BLift  = BDX * BDX + BDY * BDY;
    const double ADXBDY = ADX * BDY;
    const double BDXADY = BDX * ADY;
    const double CLift  = CDX * CDX + CDY * CDY;

    const double Det = ALift * ( BDXCDY - CDXBDY ) + BLift * ( CDXADY - ADXCDY ) + CLift * ( ADXBDY - BDXADY );
    const double Permanent = ( std::abs( BDXCDY ) + std::abs( CDXBDY ) ) * ALift +
                             ( std::abs( CDXADY ) + std::abs( ADXCDY ) ) * BLift +
                             ( std::abs( ADXBDY ) + std::abs( BDXADY ) ) * CLift;
    const double ErrBound = IccErrBoundA * Permanent;
    if ( Det > ErrBound || -Det > ErrBound )
        return Det;
    return InCircle2Exact( A, B, C, D );
}
