// DesertAsset {"Kind":"Shader","Guid":"dd04673cb49a468eb7c0ed6f7c6089d1","Versions":{"SHDR":1},"Dependencies":[]}
Shader "VirtualShadowMapInvalidateCasters"
{
    // "VSM: Invalidate Casters" (UE FVirtualShadowMapCacheManager::ProcessInvalidations, the per-primitive
    // invalidation over the pages its bounds cover). One group per (world box, level): the box is a moved caster's
    // bounds this frame or last frame (where it stood: its old depth must go too), projected into the level; every
    // mapped page of the level's window under it is marked dirty, so it is cleared and drawn again when requested.
    Compute
    {
        LocalSize(64, 1, 1);

        #include <Common/VirtualShadowMap.glslh>

        ReadBuffer(0) VsmFrameData
        {
            VsmFrame u_Frame;
            VsmLevel u_Levels[];
        };
        Buffer(1) VsmPageTable
        {
            VsmPageEntry u_Entries[];
        };
        ReadBuffer(2) VsmInvalidations
        {
            vec4 u_Boxes[]; // box b: [2b].xyz = world min cm, [2b + 1].xyz = world max cm
        };

        void main()
        {
            const uint  box   = gl_WorkGroupID.x;
            const uint  level = gl_WorkGroupID.y;
            const vec3  lo    = u_Boxes[2u * box].xyz;
            const vec3  hi    = u_Boxes[2u * box + 1u].xyz;
            const mat4  vp    = u_Levels[level].ViewProjection;
            vec2        first = vec2( 1.0e30 );
            vec2        last  = vec2( -1.0e30 );
            for ( uint c = 0u; c < 8u; ++c )
            {
                const vec3 corner = mix( lo, hi, vec3( float( c & 1u ), float( ( c >> 1u ) & 1u ), float( ( c >> 2u ) & 1u ) ) );
                const vec2 page   = ( ( vp * vec4( corner, 1.0 ) ).xy * 0.5 + 0.5 ) * float( VSM_LEVEL_PAGES );
                first             = min( first, page );
                last              = max( last, page );
            }
            const ivec2 lowPage  = max( ivec2( floor( first ) ), ivec2( 0 ) );
            const ivec2 highPage = min( ivec2( floor( last ) ), ivec2( int( VSM_LEVEL_PAGES ) - 1 ) );
            if ( any( greaterThan( lowPage, highPage ) ) )
                return;
            const ivec2 size  = highPage - lowPage + 1;
            const uint  count = uint( size.x * size.y );
            for ( uint k = gl_LocalInvocationID.x; k < count; k += 64u )
            {
                const ivec2 windowPage = lowPage + ivec2( int( k ) % size.x, int( k ) / size.x );
                const uint  index      = VsmEntryIndex( level, u_Levels[level].Window.xy + windowPage );
                if ( ( u_Entries[index].Flags & VSM_FLAG_ALLOCATED ) != 0u )
                    atomicOr( u_Entries[index].Flags, VSM_FLAG_DIRTY );
            }
        }
    }
}
