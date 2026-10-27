// DesertAsset {"Kind":"Shader","Guid":"2dc638c4254c4bffbb361eeea57434cd","Versions":{"SHDR":1},"Dependencies":[]}
// Light Readback of one emitter's Light row (VFX-LIGHT, UE Niagara Light Renderer: a light per particle from its
// position / size / colour). Dispatched by "Particles: Light Readback" after the last compact, in the simulating
// view, for every emitter with an enabled Light row. It copies what the GPU holds - no CPU-side guess:
//   header (4 uints): the lights written (min(alive, Page.y)), the light frame serial (Page.z, never 0), 0, 0;
//   rows (8 floats each): light t is alive particle floor(t * alive / lights) - the lit particles spread evenly
//         over the alive list - its world position (cm, a Local emitter's around its EmitterPos), its larger
//         SpriteSize axis (cm) and its Color rgba, read through the emitter's ParticleDrawLayout with the
//         sprite path's defaults (Common/ParticleDrawRead.glslh).
// The CPU reads the copy FramesInFlight frames later (StorageBuffer::ReadBack, the fence has finished it): no stall.
Shader "ParticleLightReadback"
{
    Compute
    {
        LocalSize(64, 1, 1);

        #include <Common/ParticlePool.glslh>
        #include <Common/ParticleDrawLayout.glslh>

        ReadBuffer(0) AttributeFloats
        {
            float DrawFloats[];
        };

        ReadBuffer(3) DrawLayout
        {
            ParticleDrawLayout DrawLayoutData;
        };

        #include <Common/ParticleDrawRead.glslh>

        ReadBuffer(1) AliveList
        {
            uint AliveData[];
        };

        ReadBuffer(2) Counters
        {
            ParticleDrawSlot Slots[];
        };

        Buffer(4) Readback
        {
            uint ReadbackWords[];
        };

        PushConstant PushConstants
        {
            uvec4 Page;       // x = the last compact's slot, y = lights wanted (1 .. 64), z = serial, w = 0
            uvec4 AliveRange; // x = the drawn alive half's offset in AliveList; yzw = 0
        };

        void main()
        {
            uint t      = gl_GlobalInvocationID.x;
            uint alive  = Slots[Page.x].VertexCount / 6u;
            uint lights = min( Page.y, alive );
            if ( t == 0u )
            {
                ReadbackWords[0] = lights;
                ReadbackWords[1] = Page.z;
                ReadbackWords[2] = 0u;
                ReadbackWords[3] = 0u;
            }
            if ( t >= lights )
                return;
            uint row   = ( t * alive ) / lights;
            uint n     = ParticleLocalIndex( AliveData[AliveRange.x + row] );
            vec3 p     = ParticleWorldPosition( n );
            vec2 size  = ParticleSpriteSize( n );
            vec4 color = ParticleColor( n );
            uint base  = 4u + t * 8u;
            ReadbackWords[base + 0u] = floatBitsToUint( p.x );
            ReadbackWords[base + 1u] = floatBitsToUint( p.y );
            ReadbackWords[base + 2u] = floatBitsToUint( p.z );
            ReadbackWords[base + 3u] = floatBitsToUint( max( size.x, size.y ) );
            ReadbackWords[base + 4u] = floatBitsToUint( color.r );
            ReadbackWords[base + 5u] = floatBitsToUint( color.g );
            ReadbackWords[base + 6u] = floatBitsToUint( color.b );
            ReadbackWords[base + 7u] = floatBitsToUint( color.a );
        }
    }
}
