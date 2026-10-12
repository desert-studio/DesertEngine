// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/Parameterization/UVPacking.cpp:24-946 and
// :1018-1075 (see UVPacking.hpp for what was adapted).
#include "Engine/Geometry/MeshCore/Parameterization/UVPacking.hpp"

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <unordered_set>

namespace Desert::Geometry
{
    namespace
    {
        // UE's FMatrix2d: row-major [[A, B], [C, D]].
        struct Mat2
        {
            double A = 1.0, B = 0.0, C = 0.0, D = 1.0;

            glm::dvec2 operator*( const glm::dvec2& V ) const
            {
                return { A * V.x + B * V.y, C * V.x + D * V.y };
            }
            Mat2 operator*( const Mat2& M ) const
            {
                return { A * M.A + B * M.C, A * M.B + B * M.D, C * M.A + D * M.C, C * M.B + D * M.D };
            }
            Mat2 operator*( double S ) const
            {
                return { A * S, B * S, C * S, D * S };
            }
            [[nodiscard]] Mat2 Inverse() const
            {
                const double Det = A * D - B * C;
                return { D / Det, -B / Det, -C / Det, A / Det };
            }
        };

        // A bit per texel, rows of 64-bit words: FUVSpaceAllocator's role (UsedSegments / FreeSegments) without
        // its run encoding.
        class UVRaster
        {
        public:
            UVRaster( uint32_t Width, uint32_t Height ) : m_Width( Width ), m_Height( Height )
            {
                m_Words = ( Width + 63u ) / 64u;
                m_Bits.assign( static_cast<size_t>( m_Words ) * Height, 0ull );
            }

            void Clear()
            {
                std::fill( m_Bits.begin(), m_Bits.end(), 0ull );
                m_RasterWidth  = 0;
                m_RasterHeight = 0;
            }

            void SetBit( uint32_t X, uint32_t Y )
            {
                m_Bits[static_cast<size_t>( Y ) * m_Words + ( X >> 6 )] |= 1ull << ( X & 63u );
                m_RasterWidth  = std::max( m_RasterWidth, X + 1 );
                m_RasterHeight = std::max( m_RasterHeight, Y + 1 );
            }

            [[nodiscard]] uint32_t GetRasterWidth() const
            {
                return m_RasterWidth;
            }
            [[nodiscard]] uint32_t GetRasterHeight() const
            {
                return m_RasterHeight;
            }

            [[nodiscard]] uint64_t Word( uint32_t Row, uint32_t W ) const
            {
                return m_Bits[static_cast<size_t>( Row ) * m_Words + W];
            }

            // The 64 bits of row Row starting at column X (zeros past the right edge).
            [[nodiscard]] uint64_t BitsAt( uint32_t Row, uint32_t X ) const
            {
                const uint32_t W  = X >> 6;
                const uint32_t Sh = X & 63u;
                if ( W >= m_Words )
                    return 0ull;
                uint64_t Bits = Word( Row, W ) >> Sh;
                if ( Sh != 0 && W + 1 < m_Words )
                    Bits |= Word( Row, W + 1 ) << ( 64u - Sh );
                return Bits;
            }

            // Does Chart, placed with its corner at (X, Y), touch a used texel?
            [[nodiscard]] bool Overlaps( const UVRaster& Chart, uint32_t X, uint32_t Y ) const
            {
                const uint32_t ChartWords = ( Chart.m_RasterWidth + 63u ) / 64u;
                for ( uint32_t R = 0; R < Chart.m_RasterHeight; ++R )
                    for ( uint32_t W = 0; W < ChartWords; ++W )
                    {
                        const uint64_t Bits = Chart.Word( R, W );
                        if ( Bits != 0 && ( Bits & BitsAt( Y + R, X + W * 64u ) ) != 0 )
                            return true;
                    }
                return false;
            }

            void Alloc( const UVRaster& Chart, uint32_t X, uint32_t Y )
            {
                for ( uint32_t R = 0; R < Chart.m_RasterHeight; ++R )
                    for ( uint32_t C = 0; C < Chart.m_RasterWidth; ++C )
                        if ( ( Chart.Word( R, C >> 6 ) >> ( C & 63u ) ) & 1ull )
                            SetBit( X + C, Y + R );
            }

            [[nodiscard]] uint64_t GetUsedTexels() const
            {
                uint64_t Count = 0;
                for ( const uint64_t Bits : m_Bits )
                    Count += static_cast<uint64_t>( std::popcount( Bits ) );
                return Count;
            }

        private:
            uint32_t              m_Width        = 0;
            uint32_t              m_Height       = 0;
            uint32_t              m_Words        = 0;
            uint32_t              m_RasterWidth  = 0;
            uint32_t              m_RasterHeight = 0;
            std::vector<uint64_t> m_Bits;
        };

        struct Rect
        {
            uint32_t X = 0, Y = 0, W = 0, H = 0;
        };

        struct UVIsland
        {
            int32_t              Id = 0;
            std::vector<int32_t> Triangles;
            glm::dvec2           MinUV{ 0.0 };
            glm::dvec2           MaxUV{ 0.0 };
            double               UVArea       = 0.0;
            double               ScaleToWorld = 1.0;
            glm::dvec2           UVScale{ 0.0 };
            Mat2                 UVRotation;
            Mat2                 UVPackingScale;
            glm::dvec2           OrientationBiasShift{ 0.0 };
            glm::dvec2           PackingBias{ 0.0 };

            [[nodiscard]] glm::dvec2 GetPackingBias() const
            {
                return ( UVRotation * ( UVPackingScale * OrientationBiasShift ) ) + glm::dvec2( 0.5 ) +
                       PackingBias;
            }
        };

        // Max of 2048x2048 due to precision. Dilate in 28.4 fixed point; half-pixel dilation is conservative
        // rasterization, the same as the Minkowski sum of triangle and square.
        template <int32_t Dilate>
        void InternalRasterizeTriangle( UVRaster& Shader, const glm::vec2 Points[3], int32_t ScissorWidth,
                                        int32_t ScissorHeight )
        {
            const glm::vec2 HalfPixel( 0.5f, 0.5f );
            glm::vec2       p0 = Points[0] - HalfPixel;
            glm::vec2       p1 = Points[1] - HalfPixel;
            glm::vec2       p2 = Points[2] - HalfPixel;

            // Correct winding
            const float Facing = ( p0.x - p1.x ) * ( p2.y - p0.y ) - ( p0.y - p1.y ) * ( p2.x - p0.x );
            if ( Facing < 0.0f )
                std::swap( p0, p2 );

            // 28.4 fixed point
            const int32_t X0 = static_cast<int32_t>( 16.0f * p0.x + 0.5f );
            const int32_t X1 = static_cast<int32_t>( 16.0f * p1.x + 0.5f );
            const int32_t X2 = static_cast<int32_t>( 16.0f * p2.x + 0.5f );
            const int32_t Y0 = static_cast<int32_t>( 16.0f * p0.y + 0.5f );
            const int32_t Y1 = static_cast<int32_t>( 16.0f * p1.y + 0.5f );
            const int32_t Y2 = static_cast<int32_t>( 16.0f * p2.y + 0.5f );

            // Bounding rect, clipped to the image
            int32_t MinX = ( std::min( { X0, X1, X2 } ) - Dilate + 15 ) / 16;
            int32_t MaxX = ( std::max( { X0, X1, X2 } ) + Dilate + 15 ) / 16;
            int32_t MinY = ( std::min( { Y0, Y1, Y2 } ) - Dilate + 15 ) / 16;
            int32_t MaxY = ( std::max( { Y0, Y1, Y2 } ) + Dilate + 15 ) / 16;
            MinX         = std::clamp( MinX, 0, ScissorWidth );
            MaxX         = std::clamp( MaxX, 0, ScissorWidth );
            MinY         = std::clamp( MinY, 0, ScissorHeight );
            MaxY         = std::clamp( MaxY, 0, ScissorHeight );

            const int32_t DX01 = X0 - X1;
            const int32_t DX12 = X1 - X2;
            const int32_t DX20 = X2 - X0;
            const int32_t DY01 = Y0 - Y1;
            const int32_t DY12 = Y1 - Y2;
            const int32_t DY20 = Y2 - Y0;

            // Half-edge constants, corrected for the fill convention, then dilated
            int32_t C0 = DY01 * X0 - DX01 * Y0;
            int32_t C1 = DY12 * X1 - DX12 * Y1;
            int32_t C2 = DY20 * X2 - DX20 * Y2;
            C0 += ( DY01 < 0 || ( DY01 == 0 && DX01 > 0 ) ) ? 0 : -1;
            C1 += ( DY12 < 0 || ( DY12 == 0 && DX12 > 0 ) ) ? 0 : -1;
            C2 += ( DY20 < 0 || ( DY20 == 0 && DX20 > 0 ) ) ? 0 : -1;
            C0 += ( std::abs( DX01 ) + std::abs( DY01 ) ) * Dilate;
            C1 += ( std::abs( DX12 ) + std::abs( DY12 ) ) * Dilate;
            C2 += ( std::abs( DX20 ) + std::abs( DY20 ) ) * Dilate;

            for ( int32_t y = MinY; y < MaxY; y++ )
                for ( int32_t x = MinX; x < MaxX; x++ )
                {
                    // same as Edge1 >= 0 && Edge2 >= 0 && Edge3 >= 0
                    int32_t IsInside = C0 + ( DX01 * y - DY01 * x ) * 16;
                    IsInside |= C1 + ( DX12 * y - DY12 * x ) * 16;
                    IsInside |= C2 + ( DX20 * y - DY20 * x ) * 16;
                    if ( IsInside >= 0 )
                        Shader.SetBit( static_cast<uint32_t>( x ), static_cast<uint32_t>( y ) );
                }
        }

        // Chart Packer for UV islands (UE: a port of FLayoutUV to FDynamicMesh3): flips are optional, input
        // islands are computed outside.
        class StandardChartPacker
        {
        public:
            UVPacker::IUVMeshView* Mesh              = nullptr;
            uint32_t               TextureResolution = 512;
            bool                   bPreserveRotation = false;
            bool                   bAllowFlips       = false;
            double                 TotalUVArea       = 0.0;

            bool FindBestPacking( std::vector<UVIsland>& Charts )
            {
                if ( static_cast<uint64_t>( Charts.size() ) >
                     static_cast<uint64_t>( TextureResolution ) * TextureResolution )
                    return false; // more charts than texels

                TotalUVArea = 0.0;
                for ( const UVIsland& Chart : Charts )
                    TotalUVArea += Chart.UVArea * Chart.ScaleToWorld * Chart.ScaleToWorld;
                if ( TotalUVArea <= 0.0 )
                    return false;

                for ( UVIsland& Chart : Charts )
                {
                    Chart.PackingBias          = glm::dvec2( 0.0 );
                    Chart.UVScale              = glm::dvec2( 0.0 );
                    Chart.UVPackingScale       = Mat2{};
                    Chart.UVRotation           = Mat2{};
                    Chart.OrientationBiasShift = glm::dvec2( 0.0 );
                }

                const double  LinearSearchStart = 0.5;
                const double  LinearSearchStep  = 0.5;
                const int32_t BinarySearchSteps = 6;

                double UVScaleFail = TextureResolution * std::sqrt( 1.0 / TotalUVArea );
                double UVScalePass = TextureResolution * std::sqrt( LinearSearchStart / TotalUVArea );

                std::vector<UVIsland> LastPassCharts;
                double                LastEfficiency = 0.0;

                // Linear search for the first fit. UE loops until one fits; a scale this small means every chart
                // is under a texel and still none fits, which only an island count near the texel count reaches.
                while ( true )
                {
                    if ( PackCharts( Charts, UVScalePass, LastEfficiency ) )
                    {
                        LastPassCharts = Charts;
                        break;
                    }
                    UVScaleFail = UVScalePass;
                    UVScalePass *= LinearSearchStep;
                    if ( UVScalePass < 1e-9 )
                        return false;
                }

                // Binary search for the best fit
                for ( int32_t i = 0; i < BinarySearchSteps; i++ )
                {
                    const double UVScale    = 0.5 * ( UVScaleFail + UVScalePass );
                    double       Efficiency = 0.0;
                    if ( PackCharts( Charts, UVScale, Efficiency ) )
                    {
                        LastPassCharts                     = Charts;
                        const double EfficiencyGainPercent = 100.0 * std::abs( Efficiency - LastEfficiency );
                        LastEfficiency                     = Efficiency;
                        // Early out when we're inside a 1% efficiency range
                        if ( EfficiencyGainPercent <= 1.0 )
                            break;
                        UVScalePass = UVScale;
                    }
                    else
                        UVScaleFail = UVScale;
                }

                // In case the last step was a failure, restore from the last known good computation
                Charts = LastPassCharts;
                return true;
            }

        private:
            void ScaleCharts( std::vector<UVIsland>& Charts, double UVScale ) const
            {
                for ( UVIsland& Chart : Charts )
                    Chart.UVScale = glm::dvec2( 1.0 ) * UVScale * Chart.ScaleToWorld;

                // Unsort the charts so ScaleCharts always returns the same ordering
                std::sort( Charts.begin(), Charts.end(),
                           []( const UVIsland& A, const UVIsland& B ) { return A.Id < B.Id; } );

                // Scale charts such that they all fit and roughly total the same area as before
                double UniformScale = 1.0;
                for ( int i = 0; i < 1000; i++ )
                {
                    uint32_t NumMaxedOut  = 0;
                    double   ScaledUVArea = 0.0;
                    for ( UVIsland& Chart : Charts )
                    {
                        const glm::dvec2 ChartSize        = Chart.MaxUV - Chart.MinUV;
                        const glm::dvec2 ChartSizeScaled  = ChartSize * Chart.UVScale * UniformScale;
                        const double     MaxChartEdge     = TextureResolution - 1.0;
                        const double     LongestChartEdge = std::max( ChartSizeScaled.x, ChartSizeScaled.y );
                        const double     Epsilon          = 0.01;
                        if ( LongestChartEdge + Epsilon > MaxChartEdge )
                        {
                            // Rescale oversized charts to fit
                            Chart.UVScale.x = MaxChartEdge / std::max( ChartSize.x, ChartSize.y );
                            Chart.UVScale.y = MaxChartEdge / std::max( ChartSize.x, ChartSize.y );
                            NumMaxedOut++;
                        }
                        else
                        {
                            Chart.UVScale.x *= UniformScale;
                            Chart.UVScale.y *= UniformScale;
                        }
                        ScaledUVArea += Chart.UVArea * Chart.UVScale.x * Chart.UVScale.y;
                    }
                    if ( NumMaxedOut == 0 || NumMaxedOut == Charts.size() )
                        break; // nothing to rebalance, or everything is maxed out

                    // Scale up smaller charts to keep the expected total area: ScaledUVArea == TotalUVArea *
                    // UVScale^2
                    const double RebalanceScale = UVScale * std::sqrt( TotalUVArea / ScaledUVArea );
                    if ( RebalanceScale < 1.01 )
                        break; // further rebalancing is minor
                    UniformScale = RebalanceScale;
                }

                // Largest chart rectangle first
                std::sort( Charts.begin(), Charts.end(),
                           []( const UVIsland& A, const UVIsland& B )
                           {
                               const glm::dvec2 RectA = ( A.MaxUV - A.MinUV ) * A.UVScale;
                               const glm::dvec2 RectB = ( B.MaxUV - B.MinUV ) * B.UVScale;
                               return RectA.x * RectA.y > RectB.x * RectB.y;
                           } );
            }

            // FUVSpaceAllocator::FindWithSegments's answer: the placement with the least IsBestRect cost
            // (X + W + Y + H) that beats BestRect; positions are visited by increasing X + Y, so the first free
            // one is that placement.
            bool FindBest( const UVRaster& Layout, const UVRaster& Chart, Rect& InOut, const Rect& Best ) const
            {
                const uint32_t RW = Chart.GetRasterWidth();
                const uint32_t RH = Chart.GetRasterHeight();
                if ( RW > TextureResolution || RH > TextureResolution )
                    return false;
                const uint32_t SpanX    = TextureResolution - RW;
                const uint32_t SpanY    = TextureResolution - RH;
                const uint64_t BestCost = static_cast<uint64_t>( Best.X ) + Best.W + Best.Y + Best.H;
                for ( uint32_t Diagonal = 0; Diagonal <= SpanX + SpanY; ++Diagonal )
                {
                    if ( static_cast<uint64_t>( Diagonal ) + InOut.W + InOut.H >= BestCost )
                        return false;
                    const uint32_t FirstX = Diagonal > SpanY ? Diagonal - SpanY : 0u;
                    const uint32_t LastX  = std::min( Diagonal, SpanX );
                    for ( uint32_t X = FirstX; X <= LastX; ++X )
                    {
                        const uint32_t Y = Diagonal - X;
                        if ( !Layout.Overlaps( Chart, X, Y ) )
                        {
                            InOut.X = X;
                            InOut.Y = Y;
                            return true;
                        }
                    }
                }
                return false;
            }

            bool PackCharts( std::vector<UVIsland>& Charts, double UVScale, double& OutEfficiency )
            {
                ScaleCharts( Charts, UVScale );

                UVRaster BestChartRaster( TextureResolution, TextureResolution );
                UVRaster ChartRaster( TextureResolution, TextureResolution );
                UVRaster LayoutRaster( TextureResolution, TextureResolution );
                OutEfficiency = 0.0;

                for ( UVIsland& Chart : Charts )
                {
                    // Try different orientations and pick the best
                    int32_t BestOrientation = -1;
                    Rect    BestRect{ ~0u >> 2, ~0u >> 2, ~0u >> 2, ~0u >> 2 };
                    auto    IsBestRect = [&BestRect]( const Rect& R )
                    {
                        return static_cast<uint64_t>( R.X ) + R.W + R.Y + R.H <
                               static_cast<uint64_t>( BestRect.X ) + BestRect.W + BestRect.Y + BestRect.H;
                    };

                    const int32_t MaxOrientation  = bPreserveRotation ? 1 : 8;
                    const int32_t OrientationStep = bAllowFlips ? 1 : 2;
                    for ( int32_t Orientation = 0; Orientation < MaxOrientation; Orientation += OrientationStep )
                    {
                        OrientChart( Chart, Orientation );
                        const glm::dvec2 ChartSize =
                             Chart.UVRotation * Chart.UVPackingScale * ( Chart.MaxUV - Chart.MinUV );

                        // Only need half pixel dilate for rects
                        Rect R;
                        R.W = static_cast<uint32_t>( std::ceil( std::abs( ChartSize.x ) + 1.0 ) );
                        R.H = static_cast<uint32_t>( std::ceil( std::abs( ChartSize.y ) + 1.0 ) );
                        // Just in case lack of precision pushes it over
                        R.W = std::min( TextureResolution, R.W );
                        R.H = std::min( TextureResolution, R.H );

                        RasterizeChart( Chart, R.W, R.H, ChartRaster );

                        bool found = false;
                        if ( ChartRaster.GetRasterWidth() == 0 && ChartRaster.GetRasterHeight() == 0 )
                        {
                            // Nothing rasterized: (0, 0), as UE answers
                            R.X   = 0;
                            R.Y   = 0;
                            found = true;
                        }
                        else
                            found = FindBest( LayoutRaster, ChartRaster, R, BestRect );

                        if ( found && IsBestRect( R ) )
                        {
                            BestChartRaster = ChartRaster;
                            BestOrientation = Orientation;
                            BestRect        = R;
                            if ( BestRect.X == 0 && BestRect.Y == 0 )
                                break; // can't be beaten
                        }
                    }

                    if ( BestOrientation < 0 )
                        return false; // found no orientation that fit

                    OrientChart( Chart, BestOrientation );
                    LayoutRaster.Alloc( BestChartRaster, BestRect.X, BestRect.Y );
                    Chart.PackingBias.x += BestRect.X;
                    Chart.PackingBias.y += BestRect.Y;
                }

                const double TotalTexels = static_cast<double>( TextureResolution ) * TextureResolution;
                OutEfficiency            = static_cast<double>( LayoutRaster.GetUsedTexels() ) / TotalTexels;
                return true;
            }

            // The packing bias puts the rotated box's corner back at (0, 0): rotate the box about its corner,
            // then shift the new corner onto the origin.
            static void OrientChart( UVIsland& Chart, int32_t Orientation )
            {
                Chart.PackingBias    = glm::dvec2( 0.0 );
                Chart.UVPackingScale = Mat2{ Chart.UVScale.x, 0.0, 0.0, Chart.UVScale.y };
                const glm::dvec2& Mn = Chart.MinUV;
                const glm::dvec2& Mx = Chart.MaxUV;
                switch ( Orientation )
                {
                    case 0: // 0 degrees
                        Chart.UVRotation           = { 1.0, 0.0, 0.0, 1.0 };
                        Chart.OrientationBiasShift = { -Mn.x, -Mn.y };
                        break;
                    case 1: // 0 degrees, flip x
                        Chart.UVRotation           = { -1.0, 0.0, 0.0, 1.0 };
                        Chart.OrientationBiasShift = { -Mx.x, -Mn.y };
                        break;
                    case 2: // 90 degrees
                        Chart.UVRotation           = { 0.0, 1.0, -1.0, 0.0 };
                        Chart.OrientationBiasShift = { -Mx.x, -Mn.y };
                        break;
                    case 3: // 90 degrees, flip x
                        Chart.UVRotation           = { 0.0, 1.0, 1.0, 0.0 };
                        Chart.OrientationBiasShift = { -Mn.x, -Mn.y };
                        break;
                    case 4: // 180 degrees
                        Chart.UVRotation           = { -1.0, 0.0, 0.0, -1.0 };
                        Chart.OrientationBiasShift = { -Mx.x, -Mx.y };
                        break;
                    case 5: // 180 degrees, flip x
                        Chart.UVRotation           = { 1.0, 0.0, 0.0, -1.0 };
                        Chart.OrientationBiasShift = { -Mn.x, -Mx.y };
                        break;
                    case 6: // 270 degrees
                        Chart.UVRotation           = { 0.0, -1.0, 1.0, 0.0 };
                        Chart.OrientationBiasShift = { -Mn.x, -Mx.y };
                        break;
                    case 7: // 270 degrees, flip x
                        Chart.UVRotation           = { 0.0, -1.0, -1.0, 0.0 };
                        Chart.OrientationBiasShift = { -Mx.x, -Mx.y };
                        break;
                    default:
                        break;
                }
            }

            // Bilinear footprint is -1 to 1 pixels: with pixel packing a full one-pixel dilation keeps charts
            // apart unless their edges align with pixel centres.
            void RasterizeChart( const UVIsland& Chart, uint32_t RectW, uint32_t RectH, UVRaster& Out ) const
            {
                Out.Clear();
                for ( const int32_t tid : Chart.Triangles )
                {
                    const Index3i UVTriangle = Mesh->GetUVTriangle( tid );
                    if ( UVTriangle.A < 0 || UVTriangle.B < 0 || UVTriangle.C < 0 )
                        continue;
                    glm::vec2 Points[3];
                    for ( int k = 0; k < 3; k++ )
                    {
                        const glm::dvec2 UV = glm::dvec2( Mesh->GetUV( UVTriangle[k] ) );
                        Points[k]           = glm::vec2( ( Chart.UVRotation * Chart.UVPackingScale * UV ) +
                                                         Chart.GetPackingBias() );
                    }
                    InternalRasterizeTriangle<16>( Out, Points, static_cast<int32_t>( RectW ),
                                                   static_cast<int32_t>( RectH ) );
                }
            }
        };
    } // namespace

    bool UVPacker::StandardPack( IUVMeshView* Mesh, int32_t NumIslands,
                                 const std::function<void( int32_t, std::vector<int32_t>& )>& CopyIsland )
    {
        StandardChartPacker Packer;
        Packer.Mesh = Mesh;
        // Lower bound of 2: the rebalance uses MaxChartEdge = TextureResolution - 1; upper bound 16384 as the
        // largest plausible texture the UVs could address.
        Packer.TextureResolution = static_cast<uint32_t>( std::clamp( TextureResolution, 2, 16384 ) );
        Packer.bPreserveRotation = bPreserveRotation;
        Packer.bAllowFlips       = bAllowFlips;

        std::vector<UVIsland> AllCharts( static_cast<size_t>( std::max( 0, NumIslands ) ) );
        for ( int32_t ci = 0; ci < NumIslands; ++ci )
        {
            UVIsland& Chart = AllCharts[ci];
            Chart.Id        = ci + 1;
            CopyIsland( ci, Chart.Triangles );
            GetIslandStats( Mesh, Chart.Triangles, Chart.MinUV, Chart.MaxUV, Chart.ScaleToWorld, Chart.UVArea );
        }

        if ( !Packer.FindBestPacking( AllCharts ) )
            return false;

        // Commit chart UVs
        const double Resolution = static_cast<double>( Packer.TextureResolution );
        for ( const UVIsland& Chart : AllCharts )
        {
            const glm::dvec2 PackingBias     = Chart.GetPackingBias() / Resolution;
            const Mat2       NormalizedScale = Chart.UVPackingScale * ( 1.0 / Resolution );
            const Mat2       InverseScale    = NormalizedScale.Inverse();

            std::unordered_set<int32_t> IslandElements;
            for ( const int32_t tid : Chart.Triangles )
            {
                const Index3i Triangle = Mesh->GetUVTriangle( tid );
                IslandElements.insert( Triangle.A );
                IslandElements.insert( Triangle.B );
                IslandElements.insert( Triangle.C );
            }
            for ( const int32_t elemid : IslandElements )
            {
                if ( elemid < 0 )
                    continue;
                const glm::dvec2 UV          = glm::dvec2( Mesh->GetUV( elemid ) );
                const glm::dvec2 Transformed = ( Chart.UVRotation * NormalizedScale * UV ) + PackingBias;
                Mesh->SetUV( elemid, glm::vec2( bPreserveScale ? InverseScale * Transformed : Transformed ) );
            }
        }
        return true;
    }

    void UVPacker::GetIslandStats( IUVMeshView* Mesh, const std::vector<int32_t>& Island, glm::dvec2& IslandMinOut,
                                   glm::dvec2& IslandMaxOut, double& IslandScaleFactorOut,
                                   double& UVAreaOut ) const
    {
        IslandMinOut       = glm::dvec2( DBL_MAX );
        IslandMaxOut       = glm::dvec2( -DBL_MAX );
        UVAreaOut          = 0.0;
        double UVLengthSum = 0.0, WorldLengthSum = 0.0;
        for ( const int32_t tid : Island )
        {
            const Index3i Triangle3D = Mesh->GetTriangle( tid );
            const Index3i TriangleUV = Mesh->GetUVTriangle( tid );
            if ( TriangleUV.A < 0 || TriangleUV.B < 0 || TriangleUV.C < 0 )
                continue; // skip invalid UV triangles

            glm::dvec3 Positions[3];
            glm::dvec2 UVs[3];
            for ( int k = 0; k < 3; k++ )
            {
                Positions[k] = Mesh->GetVertex( Triangle3D[k] );
                UVs[k]       = glm::dvec2( Mesh->GetUV( TriangleUV[k] ) );
                IslandMinOut = glm::min( IslandMinOut, UVs[k] );
                IslandMaxOut = glm::max( IslandMaxOut, UVs[k] );
            }
            const double WorldLength = glm::length( Positions[1] - Positions[0] ) +
                                       glm::length( Positions[2] - Positions[0] ) +
                                       glm::length( Positions[2] - Positions[1] );
            const glm::dvec2 EdgeUV1  = UVs[1] - UVs[0];
            const glm::dvec2 EdgeUV2  = UVs[2] - UVs[0];
            const glm::dvec2 EdgeUV3  = UVs[2] - UVs[1];
            const double     UVLength = glm::length( EdgeUV1 ) + glm::length( EdgeUV2 ) + glm::length( EdgeUV3 );
            UVLengthSum += UVLength;
            WorldLengthSum += WorldLength;
            UVAreaOut += 0.5 * std::abs( EdgeUV1.x * EdgeUV2.y - EdgeUV1.y * EdgeUV2.x );
        }
        if ( IslandMinOut.x > IslandMaxOut.x )
        {
            IslandMinOut = glm::dvec2( 0.0 );
            IslandMaxOut = glm::dvec2( 0.0 );
        }
        // The ratio of edge lengths rather than areas, for robustness (a ~zero-area island from a bad
        // projection or sliver triangles would be over-scaled).
        IslandScaleFactorOut =
             ( !bScaleIslandsByWorldSpaceTexelRatio || UVLengthSum < 1e-8 ) ? 1.0 : WorldLengthSum / UVLengthSum;
    }
} // namespace Desert::Geometry
