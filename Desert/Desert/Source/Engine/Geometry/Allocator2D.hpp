#pragma once
// Ported from UE 5.8 Engine/Source/Runtime/Engine/Public/Allocator2D.h (FAllocator2D: one bit per texel in rows of
// 64-bit words; Test / Find / Alloc place one raster inside another), adapted: a value type with its own width and
// height (UE reuses one TextureResolution-sized grid for the chart and the layout), the row-segment acceleration
// (FindWithSegments) is not ported - the scan tests a placement row word by word and leaves at its first conflict -
// and Alloc takes a signed origin with clipping, so a chart's dilated raster may reach a texel past its rectangle.
#include <cstdint>
#include <vector>

namespace Desert::Geometry
{
    class Allocator2D
    {
    public:
        struct Rect
        {
            uint32_t X = 0;
            uint32_t Y = 0;
            uint32_t W = 0;
            uint32_t H = 0;
        };

        Allocator2D() = default;
        Allocator2D( uint32_t width, uint32_t height );

        [[nodiscard]] uint32_t Width() const
        {
            return m_Width;
        }
        [[nodiscard]] uint32_t Height() const
        {
            return m_Height;
        }

        void                   Clear();
        [[nodiscard]] bool     Test( uint32_t x, uint32_t y ) const;
        void                   SetBit( uint32_t x, uint32_t y );
        [[nodiscard]] uint32_t CountSet() const;

        // True when no set bit of @p other lands on a set bit of this grid with other's origin at (rect.X, rect.Y),
        // false when one does or other leaves the grid there (UE FAllocator2D::Test( Rect, Other )). rect.W / rect.H
        // are not read: other's own size is the placement's.
        [[nodiscard]] bool Fits( const Rect& rect, const Allocator2D& other ) const;
        // The first origin in row order (y, then x) where @p other fits (UE FAllocator2D::Find, FindBitByBit): rect
        // receives it and other's size. False when no origin does.
        [[nodiscard]] bool Find( Rect& rect, const Allocator2D& other ) const;
        // ORs @p other in with its origin at (x, y); its bits outside this grid are dropped (UE FAllocator2D::Alloc).
        void Alloc( int32_t x, int32_t y, const Allocator2D& other );
        // This raster grown by a texel on every side, each set bit spread over its 3 x 3 neighbourhood: a
        // (W + 2) x (H + 2) raster in which this one's bit (x, y) sits at (x + 1, y + 1).
        [[nodiscard]] Allocator2D Dilated() const;

    private:
        // The 64 bits of row @p y starting at column @p x (columns past the width read 0).
        [[nodiscard]] uint64_t RowBitsAt( uint32_t y, uint32_t x ) const;

        uint32_t              m_Width  = 0;
        uint32_t              m_Height = 0;
        uint32_t              m_Pitch  = 0; // 64-bit words per row
        std::vector<uint64_t> m_Bits;
    };
} // namespace Desert::Geometry
