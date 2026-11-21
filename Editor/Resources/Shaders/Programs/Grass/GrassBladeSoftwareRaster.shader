// FPS-GRASS-SW-RASTER, THE FAR BLADES' SOFTWARE RASTER (UE 5 Nanite NaniteRasterizer.usf MicropolyRasterize: a
// cluster whose triangles are a pixel or two goes to a compute raster - the hardware rasterizer's 2x2 quads and
// its setup per triangle cost more than the few pixels it fills). FIELD_Barn TURN: the HW visibility raster of the
// blades past the mid strip level ('Grass: Visibility Raster Far' + 'Grass: Visibility Raster') was 0.55 ms for
// 1-2 px triangles. One thread a drawn blade of a software bucket (GrassRenderer::AddFrameBladeSoftwareRaster: the
// buckets >= MeshRenderer::kGrassRasterMidBuckets of every blade section MeshRenderer::PrepareGrassSoftwareRaster
// admitted; the mesh grass and the nearer buckets stay on the HW raster). The blade is placed from its setup record
// exactly as Mesh/Surface/Vertex_GrassBlade.glslh places the HW strip (BladeCurvePoint, the card's hull), each of its
// 2 x segments triangles is scanned over the pixel centres its box holds with edge functions (inclusive: a pixel on
// a shared edge is written by both triangles, the atomic max keeps one - no crack), the G-buffer depth tested
// CloserOrEqual (the HW raster's early test), then Mesh/Surface/Pass_Visibility.glslh's fragment in the same order:
// the hidden-word test, the material's opacity mask at the UV's analytic gradient (the material's own row of the
// scene table, MeshRenderer::GrassSoftwareRasterMask), and DesertWriteVisibility with the triangle's id - the word
// the resolve rebuilds the pixel from (Pass_VisibilityResolve.glslh). A blade never cross-fades its strip levels
// (Vertex_GrassBlade.glslh: LodFadeSide 0), so the LOD dither (DesertLODTransitionClipped at side 0) never clips.
Shader "GrassBladeSoftwareRaster"
{
    Compute
    {
        LocalSize(64, 1, 1);

        #include <Common/VisibilityBuffer.glslh>
        #include <Common/BindlessTextures.glslh>
        #include <Mesh/Surface/GrassBlade.glslh>

        // GrassRenderer GrassBladeSoftwareRasterRowGpu, std430: the variety's whole GrassBladeShapeGpu (the hull
        // too), where this phase's records lie and the section's mask in the scene material table.
        struct GrassBladeRasterRow
        {
            GrassBladeShapeData Shape;
            vec4                HullTexels;
            vec4                Hull[128];
            // x = the camera records' first word in the args pool, y = the software buckets (bit b: bucket b is
            // rastered here), z = the records (one per strip level), w = the row's first entry
            uvec4 Args;
            // x = the section's first slot in BladeRasterMaterialSlots, y = the mask texture's slot, z = the channel
            // parameter's slot, w = the fixed channel (z = kNoSlot)
            uvec4 Mask;
            uvec4 MaskClip; // x = the clip parameter's slot, y = the mask's UV set (0 the card, 1 clump / height)
        };

        ReadBuffer(1) BladeRasterTable
        {
            uvec4               BladeRasterInfo; // x = rows, y = entries, z = the mode, w = 0
            GrassBladeRasterRow BladeRasterRows[];
        };
        ReadBuffer(2) BladeRasterArgs
        {
            uint BladeRasterArgWords[];
        };
        ReadBuffer(3) BladeRasterSetup
        {
            GrassBladeSetupData BladeRasterSetupRecord[];
        };
        // The scene material table (GpuScene::AddMaterialRow): every parameter one vec4 slot.
        ReadBuffer(4) BladeRasterMaterials
        {
            vec4 BladeRasterMaterialSlots[];
        };
        Buffer(5) BladeRasterWork
        {
            uint BladeRasterWorkWords[];
        };
        Uniform sampler BladeRasterDepthSampler;
        Uniform(6) texture2D BladeRasterDepth;

        const uint kRasterGroupThreads = 64u; // LocalSize
        const uint kModeWork           = 0u;
        const uint kWorkHeaderWords    = 4u;
        const uint kMaxGroupsX         = 65535u;
        const uint kRecordWords        = 5u; // VkDrawIndexedIndirectCommand
        const uint kArgsHeaderWords    = 4u; // GpuInstances.hpp
        const uint kNoSlot             = 0xFFFFFFFFu;
        const uint kMaxStripVertices   = 32u; // (kGrassBladeMaxSegments 15 + 1) x 2

        // The hull's inputs (Mesh/Surface/GrassBladeHull.glslh) are the row's.
        uint g_Row = 0u;
        #define Blade           BladeRasterRows[g_Row].Shape
        #define BladeHullTexels BladeRasterRows[g_Row].HullTexels
        #define BladeHull       BladeRasterRows[g_Row].Hull
        #include <Mesh/Surface/GrassBladeHull.glslh>

        shared uint s_Scan[64];

        uint RowOfEntry( uint entry )
        {
            uint lo = 0u;
            uint hi = BladeRasterInfo.x;
            while ( hi - lo > 1u )
            {
                const uint mid = ( lo + hi ) / 2u;
                if ( BladeRasterRows[mid].Args.w <= entry )
                    lo = mid;
                else
                    hi = mid;
            }
            return lo;
        }

        uint RecordWordOf( uint r, uint entry )
        {
            return BladeRasterRows[r].Args.x + kArgsHeaderWords + ( entry - BladeRasterRows[r].Args.w ) * kRecordWords;
        }

        // The blades entry @p entry draws here: its record's instances when its bucket is a software one.
        uint EntryDrawn( uint entry )
        {
            const uint r      = RowOfEntry( entry );
            const uint bucket = entry - BladeRasterRows[r].Args.w;
            if ( bucket >= 32u || ( BladeRasterRows[r].Args.y & ( 1u << bucket ) ) == 0u )
                return 0u;
            const uint record = RecordWordOf( r, entry );
            const uint count  = BladeRasterArgWords[record + 1u];
            const uint first  = BladeRasterArgWords[record + 4u];
            const uint base   = BladeRasterRows[r].Shape.Lists.x;
            const uint slots  = BladeRasterRows[r].Shape.Visibility.w;
            if ( first < base || first - base >= slots )
                return 0u;
            return min( count, slots - ( first - base ) );
        }

        // kModeWork (GrassBladeSetup.shader BuildWork's scan): each entry's software blades before it, the total and
        // the raster's dispatch.
        void BuildWork()
        {
            const uint entries = BladeRasterInfo.y;
            const uint lane    = gl_LocalInvocationID.x;
            uint       carry   = 0u;
            for ( uint c = 0u; c < entries; c += kRasterGroupThreads )
            {
                const uint entry = c + lane;
                const uint drawn = entry < entries ? EntryDrawn( entry ) : 0u;
                s_Scan[lane]     = drawn;
                barrier();
                for ( uint o = 1u; o < kRasterGroupThreads; o <<= 1u )
                {
                    const uint add = lane >= o ? s_Scan[lane - o] : 0u;
                    barrier();
                    s_Scan[lane] += add;
                    barrier();
                }
                if ( entry < entries )
                    BladeRasterWorkWords[kWorkHeaderWords + entry] = carry + s_Scan[lane] - drawn;
                carry += s_Scan[kRasterGroupThreads - 1u];
                barrier();
            }
            if ( lane != 0u )
                return;
            const uint groups = ( carry + kRasterGroupThreads - 1u ) / kRasterGroupThreads;
            const uint x      = min( groups, kMaxGroupsX );
            BladeRasterWorkWords[0] = x;
            BladeRasterWorkWords[1] = x == 0u ? 0u : ( groups + x - 1u ) / x;
            BladeRasterWorkWords[2] = 1u;
            BladeRasterWorkWords[3] = carry;
        }

        uint EntryOfBlade( uint blade )
        {
            uint lo = 0u;
            uint hi = BladeRasterInfo.y;
            while ( hi - lo > 1u )
            {
                const uint mid = ( lo + hi ) / 2u;
                if ( BladeRasterWorkWords[kWorkHeaderWords + mid] <= blade )
                    lo = mid;
                else
                    hi = mid;
            }
            return lo;
        }

        float EdgeFunction( vec2 a, vec2 b, vec2 c )
        {
            return ( b.x - a.x ) * ( c.y - a.y ) - ( b.y - a.y ) * ( c.x - a.x );
        }

        // The mask of Pass_Visibility.glslh (DesertSurfaceOpacityMaskGrad against DESERT_SURFACE_CLIP_THRESHOLD),
        // read from the section's own material row.
        bool MaskPasses( uint r, vec2 uv, vec2 dx, vec2 dy )
        {
            const uvec4 mask = BladeRasterRows[r].Mask;
            if ( mask.y == kNoSlot )
                return true; // an opaque template: no mask
            const vec4  slot    = BladeRasterMaterialSlots[mask.x + mask.y];
            const uint  texSlot = floatBitsToUint( slot.x );
            const uint  smpSlot = floatBitsToUint( slot.y );
            const uint  channel = mask.z == kNoSlot
                                      ? min( mask.w, 3u )
                                      : uint( clamp( BladeRasterMaterialSlots[mask.x + mask.z].x, 0.0, 3.0 ) );
            const uint  clipAt  = BladeRasterRows[r].MaskClip.x;
            const float clip    = clipAt == kNoSlot ? 0.0 : BladeRasterMaterialSlots[mask.x + clipAt].x;
            const vec4  texel   = textureGrad( sampler2D( BindlessTextures[nonuniformEXT( texSlot )],
                                                          BindlessSamplers[nonuniformEXT( smpSlot )] ),
                                               uv, dx, dy );
            return texel[channel] >= clip;
        }

        void RasterBlade()
        {
            const uint group = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
            const uint blade = group * kRasterGroupThreads + gl_LocalInvocationID.x;
            if ( blade >= BladeRasterWorkWords[3] || BladeRasterInfo.y == 0u )
                return;
            const uint entry = EntryOfBlade( blade );
            const uint r     = RowOfEntry( entry );
            g_Row            = r;
            const uint first = BladeRasterArgWords[RecordWordOf( r, entry ) + 4u];
            const uint base  = BladeRasterRows[r].Shape.Lists.x;
            const uint slot  = first - base + ( blade - BladeRasterWorkWords[kWorkHeaderWords + entry] );
            if ( first < base || slot >= BladeRasterRows[r].Shape.Visibility.w )
                return;
            // Vertex_GrassBlade.glslh's row and its refusals: no records this frame, or a row past them.
            const uint row = BladeRasterRows[r].Shape.Visibility.x + slot;
            if ( BladeRasterRows[r].Shape.Lists.y == 0u || row >= uint( BladeRasterSetupRecord.length() ) )
                return;
            const GrassBladeSetupData setup    = BladeRasterSetupRecord[row];
            const uint                segments = clamp( BladeSetupSegments( setup ), 1u, kMaxStripVertices / 2u - 1u );
            const float               rowSpan  = 1.0 / float( segments );
            const BladeCurve          curve    = BladeSetupCurve( setup, false );
            const float               width    = BladeSetupWidth( setup );
            const uint                card     = BladeSetupCard( setup );
            const vec4                rect     = Blade.Cards[card];
            const bool                cardUV   = BladeRasterRows[r].MaskClip.y == 0u;

            // The strip's vertices (GrassRenderer::BladeStrip: vertex 2 x row + (side > 0)), placed as the HW
            // vertex stage places them, in the raster's pixel space (gl_FragCoord's: DesertPixelToJitteredNdc
            // inverted), with 1 / w and UV / w for the perspective-correct mask UV.
            const vec2 pixelToNdc = cameraUB.PixelToNdc.xy;
            const vec2 ndcOrigin  = cameraUB.PixelToNdc.zw;
            vec3       screen[kMaxStripVertices]; // xy pixel, z NDC depth (reversed)
            vec3       attrib[kMaxStripVertices]; // UV / w, 1 / w
            bool       behind = false;
            const uint vertices = 2u * ( segments + 1u );
            for ( uint v = 0u; v < vertices; ++v )
            {
                const float      side     = ( v & 1u ) != 0u ? 1.0 : -1.0;
                const float      t        = float( v >> 1u ) * rowSpan;
                const BladePoint full     = BladeCurvePoint( curve, width, side, t );
                const float      hullSide = BladeHullSideOf( card, width, full, side, t, rowSpan );
                const BladePoint point    = BladeHullPointOf( width, full, side, hullSide );
                const vec4       clip     = cameraUB.JitteredViewProjection * vec4( point.Position, 1.0 );
                behind                    = behind || clip.w <= 1.0e-4;
                const float invW          = 1.0 / max( clip.w, 1.0e-4 );
                const vec2  ndc           = clip.xy * invW;
                screen[v]                 = vec3( ( ndc - ndcOrigin ) / pixelToNdc, clip.z * invW );
                const vec2 uv = cardUV ? BladeCardUV( rect, hullSide, t ) : vec2( BladeSetupClump( setup ), t );
                attrib[v]     = vec3( uv * invW, invW );
            }
            if ( behind )
                return; // a far bucket's blade never reaches the near plane; one that does is not drawn whole

            const ivec2 extent   = ivec2( VisibilityHeader.xy );
            const float mipScale = exp2( cameraUB.MaterialMipBias );
            const uint  tris     = 2u * segments;
            for ( uint triangle = 0u; triangle < tris; ++triangle )
            {
                // BladeTriangleCorners' order: (a, a+1, a+2) and (a+1, a+3, a+2), a = 2 x row.
                const uint a  = 2u * ( triangle >> 1u );
                const uint i0 = ( triangle & 1u ) == 0u ? a : a + 1u;
                const uint i1 = ( triangle & 1u ) == 0u ? a + 1u : a + 3u;
                const uint i2 = a + 2u;
                const vec2 p0 = screen[i0].xy;
                const vec2 p1 = screen[i1].xy;
                const vec2 p2 = screen[i2].xy;
                const float area = EdgeFunction( p0, p1, p2 );
                if ( abs( area ) < 1.0e-8 )
                    continue; // a zero-width hull row: no pixel, as the HW raster
                // The pixel centres (x + 0.5) inside the box, clamped to the words.
                const vec2  lo  = min( p0, min( p1, p2 ) );
                const vec2  hi  = max( p0, max( p1, p2 ) );
                const ivec2 bLo = max( ivec2( ceil( lo - 0.5 ) ), ivec2( 0 ) );
                const ivec2 bHi = min( ivec2( floor( hi - 0.5 ) ), extent - 1 );
                if ( any( greaterThan( bLo, bHi ) ) )
                    continue; // covers no pixel centre: the HW raster draws none either
                const float invArea = 1.0 / area;
                // The barycentrics' change across one pixel (constant over the triangle): the screen-space plane of
                // depth, 1 / w and UV / w - the mask's gradient is analytic, as the resolve's
                // (VisibilityBarycentricDerivatives).
                const vec3 dbdx = vec3( p1.y - p2.y, p2.y - p0.y, p0.y - p1.y ) * invArea;
                const vec3 dbdy = vec3( p2.x - p1.x, p0.x - p2.x, p1.x - p0.x ) * invArea;
                const vec3 z    = vec3( screen[i0].z, screen[i1].z, screen[i2].z );
                const vec3 w    = vec3( attrib[i0].z, attrib[i1].z, attrib[i2].z );
                const vec3 u    = vec3( attrib[i0].x, attrib[i1].x, attrib[i2].x );
                const vec3 vv   = vec3( attrib[i0].y, attrib[i1].y, attrib[i2].y );
                for ( int y = bLo.y; y <= bHi.y; ++y )
                {
                    for ( int x = bLo.x; x <= bHi.x; ++x )
                    {
                        const vec2 s  = vec2( x, y ) + 0.5;
                        const vec3 b  = vec3( EdgeFunction( p1, p2, s ), EdgeFunction( p2, p0, s ),
                                              EdgeFunction( p0, p1, s ) ) *
                                       invArea;
                        if ( any( lessThan( b, vec3( 0.0 ) ) ) )
                            continue;
                        const float depth = dot( b, z );
                        if ( depth <= 0.0 || depth > 1.0 )
                            continue; // outside the view's depth range (reversed Z), clipped by the HW raster
                        const ivec2 pixel = ivec2( x, y );
                        // The G-buffer's depth, CloserOrEqual (the HW raster's early test; nothing written).
                        if ( depth < texelFetch( sampler2D( BladeRasterDepth, BladeRasterDepthSampler ), pixel, 0 ).r )
                            continue;
                        if ( DesertVisibilityHidden( pixel, depth ) )
                            continue;
                        const float invW = dot( b, w );
                        const vec2  uvW  = vec2( dot( b, u ), dot( b, vv ) );
                        const vec2  uv   = uvW / invW;
                        const float dWdx = dot( dbdx, w );
                        const float dWdy = dot( dbdy, w );
                        const vec2  dx   = ( vec2( dot( dbdx, u ), dot( dbdx, vv ) ) - uv * dWdx ) / invW * mipScale;
                        const vec2  dy   = ( vec2( dot( dbdy, u ), dot( dbdy, vv ) ) - uv * dWdy ) / invW * mipScale;
                        if ( !MaskPasses( r, uv, dx, dy ) )
                            continue;
                        DesertWriteVisibility( pixel, depth, row, triangle );
                    }
                }
            }
        }

        void main()
        {
            if ( BladeRasterInfo.z == kModeWork )
                BuildWork();
            else
                RasterBlade();
        }
    }
}
