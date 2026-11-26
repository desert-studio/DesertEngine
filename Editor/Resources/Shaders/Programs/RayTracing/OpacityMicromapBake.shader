// DesertAsset {"Kind":"Shader","Guid":"bf169de01dfd4c3d9eddf4362cd77c58","Versions":{"SHDR":1},"Dependencies":[]}
// THE GPU OPACITY MICROMAP BAKE (RT-OMM-SER; UE 5.4 r.RayTracing.OpacityMicromaps, NVIDIA OMM SDK GPU baker): one
// non-opaque BLAS segment per dispatch, 16 threads per triangle, 4 triangles per group. Each triangle is cut into
// 4^4 = 256 micro-triangles in the order VK_EXT_opacity_micromap addresses them by (the spec's index2dbary bird
// curve); each thread classifies 16 of them and writes one 32-bit word of 2-bit states
// (VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT, micro-triangle i at bits 2i of the triangle's data).
//
// EXACT, NOT APPROXIMATE: a micro-triangle is OPAQUE / TRANSPARENT only when every mip-0 texel the alpha test's
// bilinear tap can read anywhere inside it (its UV bounding box, widened by the bilinear footprint) is on that side of
// the cutoff - a bilinear blend of texels all >= cutoff is >= cutoff - so the hardware answers exactly what
// RayQueryCandidateBlocks (Common/RayQueryVisibility.glslh, lod 0) would have answered. Anything else, or a footprint
// above kMaxTexels, is UNKNOWN and still reaches the any-hit / ray-query alpha test. A one-sided material's opaque
// micro-triangle is UNKNOWN_OPAQUE: its back face lets the ray through, which only the alpha test knows.
// A triangle whose 256 states are all transparent / all opaque gets the special index -1 / -2 (no data read).
//
// Inputs: the segment as RayTracingSceneMetadata.hpp's geometry record lays it out (addresses, stride, first vertex /
// index, UV offset) and the material record's opacity source (texture, sampler, cutoff, flags); the UV convention is
// the raster's (DesertMeshTextureUV), as in RayQueryCandidateBlocks.
Shader "OpacityMicromapBake"
{
    Compute
    {
        LocalSize(64, 1, 1);

        PushConstant PushConstants
        {
            uvec2 VertexAddress; // lo, hi: the buffer the BLAS reads positions from
            uvec2 IndexAddress;
            uint  VertexStride;
            uint  FirstVertex;
            uint  FirstIndex; // in indices
            uint  UvOffset;   // bytes into the vertex
            uint  TriangleCount;
            uint  OpacityTexture; // RayTracingMaterialRecord
            uint  OpacitySampler;
            float OpacityCutoff;
            uint  MaterialFlags;
            // The three outputs, by device address (OpacityMicromapBakeBuffers, VulkanRayTracingGeometry.hpp): the
            // micromap build and the BLAS build read them by address, so no descriptor names them.
            uvec2 DataAddress;      // 64 B of 2-bit states per triangle (OpacityMicromapBytesPerTriangle(4))
            uvec2 TrianglesAddress; // VkMicromapTriangleEXT per triangle: x = dataOffset, y = level | format << 16
            uvec2 IndicesAddress;   // int per triangle: its micromap triangle or a special index (uint32 index type)
        };

        #extension GL_EXT_buffer_reference : require
        #extension GL_EXT_buffer_reference_uvec2 : require

        #include <Common/BindlessTextures.glslh>
        #include <Mesh/MeshVertexAccess.glslh>

        layout( buffer_reference, std430, buffer_reference_align = 4 ) readonly buffer OmmUintRef
        {
            uint Value;
        };
        layout( buffer_reference, std430, buffer_reference_align = 4 ) readonly buffer OmmVec2Ref
        {
            vec2 Value;
        };
        layout( buffer_reference, std430, buffer_reference_align = 4 ) writeonly buffer OmmUintOut
        {
            uint Value;
        };
        layout( buffer_reference, std430, buffer_reference_align = 8 ) writeonly buffer OmmUvec2Out
        {
            uvec2 Value;
        };
        layout( buffer_reference, std430, buffer_reference_align = 4 ) writeonly buffer OmmIntOut
        {
            int Value;
        };

        const uint kLevel            = 4u;  // VulkanRayTracingBuildInput.hpp kOpacityMicromapLevel
        const uint kWordsPerTriangle = 16u; // 256 states * 2 bits / 32
        const uint kMaxTexels        = 64u; // a larger footprint is UNKNOWN (exact, never guessed)
        const uint kFormat4State     = 2u;  // VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT

        const uint kStateTransparent        = 0u; // VK_OPACITY_MICROMAP_STATE_*_EXT
        const uint kStateOpaque             = 1u;
        const uint kStateUnknownTransparent = 2u;
        const uint kStateUnknownOpaque      = 3u;

        const int kSpecialFullyTransparent = -1; // VK_OPACITY_MICROMAP_SPECIAL_INDEX_*_EXT
        const int kSpecialFullyOpaque      = -2;

        // RayTracingSceneMetadata.hpp kMaterialFlagLandscapeVisibility | kMaterialFlagSdfMedian: other tests than a
        // texel cutoff - the CPU never asks to bake them; all UNKNOWN if it ever did.
        const uint kNotATexelTest = ( 1u << 3u ) | ( 1u << 8u );

        shared uint s_Seen[4]; // per triangle of the group: bit s = state s seen

        uvec2 OmmAddressAdd( uvec2 address, uint bytes )
        {
            uint       carry;
            const uint lo = uaddCarry( address.x, bytes, carry );
            return uvec2( lo, address.y + carry );
        }

        vec2 CornerUv( uint triangle, uint corner )
        {
            const uint indexByte = ( FirstIndex + triangle * 3u + corner ) * 4u;
            const uint vertex    = OmmUintRef( OmmAddressAdd( IndexAddress, indexByte ) ).Value + FirstVertex;
            return DesertMeshTextureUV(
                 OmmVec2Ref( OmmAddressAdd( VertexAddress, vertex * VertexStride + UvOffset ) ).Value );
        }

        // VK_EXT_opacity_micromap's micro-triangle order (Vulkan spec "Micromap indexing", index2dbary / index2bary).
        uint ExtractEvenBits( uint x )
        {
            x &= 0x55555555u;
            x = ( x | ( x >> 1u ) ) & 0x33333333u;
            x = ( x | ( x >> 2u ) ) & 0x0f0f0f0fu;
            x = ( x | ( x >> 4u ) ) & 0x00ff00ffu;
            x = ( x | ( x >> 8u ) ) & 0x0000ffffu;
            return x;
        }
        uint PrefixEor( uint x )
        {
            x ^= x >> 1u;
            x ^= x >> 2u;
            x ^= x >> 4u;
            x ^= x >> 8u;
            return x;
        }
        // The micro-triangle's three corners in the triangle's barycentrics (x = vertex 1's weight, y = vertex 2's:
        // the hit attribute convention the traversal looks the micromap up with).
        void MicroTriangle( uint index, out vec2 b0, out vec2 b1, out vec2 b2 )
        {
            const uint bit0    = ExtractEvenBits( index );
            const uint bit1    = ExtractEvenBits( index >> 1u );
            const uint fx      = PrefixEor( bit0 );
            const uint fy      = PrefixEor( bit0 & ~bit1 );
            const uint t       = fy ^ bit1;
            const uint mask    = ( 1u << kLevel ) - 1u;
            uint       iu      = ( ( fx & ~t ) | ( bit0 & ~t ) | ( ~bit0 & ~fx & t ) ) & mask;
            uint       iv      = ( fy ^ bit0 ) & mask;
            const uint iw      = ( ( ~fx & ~t ) | ( bit0 & ~t ) | ( ~bit0 & fx & t ) ) & mask;
            const bool upright = ( ( iu & 1u ) ^ ( iv & 1u ) ^ ( iw & 1u ) ) != 0u;
            if ( !upright )
            {
                iu += 1u;
                iv += 1u;
            }
            const float scale = 1.0 / float( 1u << kLevel );
            const float d     = upright ? scale : -scale;
            b0                = vec2( float( iu ), float( iv ) ) * scale;
            b1                = b0 + vec2( d, 0.0 );
            b2                = b0 + vec2( 0.0, d );
        }

        vec2 UvAt( vec2 uv0, vec2 uv1, vec2 uv2, vec2 b )
        {
            return uv0 * ( 1.0 - b.x - b.y ) + uv1 * b.x + uv2 * b.y;
        }

        // RayQueryCandidateBlocks' texel test: the channel the record names against the cutoff.
        bool TexelPasses( vec4 texel )
        {
            const uint  channel = ( MaterialFlags >> 1u ) & 3u;
            const float opacity = channel == 0u ? texel.a : texel[int( channel ) - 1];
            return opacity >= OpacityCutoff;
        }

        uint Classify( vec2 a, vec2 b, vec2 c, vec2 size )
        {
            // Texel-centre space: the bilinear tap at p reads texels floor(p - 0.5) and the one after, per axis.
            const vec2  lo    = min( a, min( b, c ) ) * size - 0.5;
            const vec2  hi    = max( a, max( b, c ) ) * size - 0.5;
            const ivec2 first = ivec2( floor( lo ) );
            const ivec2 last  = ivec2( floor( hi ) ) + 1;
            const ivec2 span  = last - first + 1;
            if ( uint( span.x ) * uint( span.y ) > kMaxTexels )
                return kStateUnknownTransparent;
            bool anyOpaque = false;
            bool anyClear  = false;
            for ( int y = first.y; y <= last.y; ++y )
                for ( int x = first.x; x <= last.x; ++x )
                {
                    // A texel centre with the material's own sampler (its wrap mode) returns that texel exactly.
                    const vec2 uv = ( vec2( x, y ) + 0.5 ) / size;
                    if ( TexelPasses( SampleBindless( OpacityTexture, OpacitySampler, uv, 0.0 ) ) )
                        anyOpaque = true;
                    else
                        anyClear = true;
                }
            if ( anyOpaque && anyClear )
                return kStateUnknownTransparent;
            if ( anyClear )
                return kStateTransparent;
            return ( MaterialFlags & 1u ) != 0u ? kStateOpaque : kStateUnknownOpaque;
        }

        void main()
        {
            const uint local    = gl_LocalInvocationID.x / kWordsPerTriangle; // triangle within the group
            const uint word     = gl_LocalInvocationID.x % kWordsPerTriangle;
            const uint triangle = gl_WorkGroupID.x * 4u + local;
            if ( word == 0u )
                s_Seen[local] = 0u;
            barrier();

            if ( triangle < TriangleCount )
            {
                const bool texelTest = ( MaterialFlags & kNotATexelTest ) == 0u;
                const vec2 uv0       = CornerUv( triangle, 0u );
                const vec2 uv1       = CornerUv( triangle, 1u );
                const vec2 uv2       = CornerUv( triangle, 2u );
                const vec2 size      = vec2( textureSize(
                     sampler2D( BindlessTextures[nonuniformEXT( OpacityTexture )],
                                BindlessSamplers[nonuniformEXT( OpacitySampler )] ),
                     0 ) );
                uint bits = 0u;
                uint seen = 0u;
                for ( uint i = 0u; i < 16u; ++i )
                {
                    vec2 b0, b1, b2;
                    MicroTriangle( word * 16u + i, b0, b1, b2 );
                    const uint state = texelTest ? Classify( UvAt( uv0, uv1, uv2, b0 ), UvAt( uv0, uv1, uv2, b1 ),
                                                             UvAt( uv0, uv1, uv2, b2 ), size )
                                                 : kStateUnknownTransparent;
                    bits |= state << ( 2u * i );
                    seen |= 1u << state;
                }
                OmmUintOut( OmmAddressAdd( DataAddress, ( triangle * kWordsPerTriangle + word ) * 4u ) ).Value = bits;
                atomicOr( s_Seen[local], seen );
            }
            barrier();

            if ( triangle < TriangleCount && word == 0u )
            {
                OmmUvec2Out( OmmAddressAdd( TrianglesAddress, triangle * 8u ) ).Value =
                     uvec2( triangle * kWordsPerTriangle * 4u, kLevel | ( kFormat4State << 16u ) );
                const uint seen = s_Seen[local];
                OmmIntOut( OmmAddressAdd( IndicesAddress, triangle * 4u ) ).Value =
                     seen == ( 1u << kStateTransparent ) ? kSpecialFullyTransparent
                     : seen == ( 1u << kStateOpaque )    ? kSpecialFullyOpaque
                                                         : int( triangle );
            }
        }
    }
}
