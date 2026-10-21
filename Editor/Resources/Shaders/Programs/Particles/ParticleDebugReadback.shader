// DesertAsset {"Kind":"Shader","Guid":"6c1f0d9a4b2e47a8b35d91e0c7a4f216","Versions":{"SHDR":1},"Dependencies":[]}
// Debug Readback of one emitter (VFX-16, UE Niagara Debugger's GPU counts and particle attribute view). Dev only:
// dispatched by "Particles: Debug Readback" after the last compact, in the simulating view, only while an editor
// debugger asks (ParticleWorldGpu::SetDebugRequest). It copies what the GPU holds - no CPU-side guess:
//   header (4 uints): alive count and free count of the last compact's slot (u_Page.x), the debug frame serial
//                     (u_Range.w, never 0), the rows this page wrote;
//   rows: per alive-list entry u_Page.y + t (t < u_Page.z, t < alive - first): the pool index, then the emitter's
//         u_Page.w float attribute components, then the u_Columns.x int components as their bits (SoA column c
//         at c * range count + local index, local index = pool index - pool base: ParticleDrawRead.glslh's
//         ParticleLocalIndex over ParticleDrawLayout's Range, the layout ParticleSimulate writes).
// The CPU reads the copy FramesInFlight frames later (StorageBuffer::ReadBack, the fence has finished it): no stall.
Shader "ParticleDebugReadback"
{
    Compute
    {
        LocalSize(64, 1, 1);

        #include <Common/ParticlePool.glslh>

        ReadBuffer(0) Counters
        {
            ParticleDrawSlot u_Slots[];
        };

        ReadBuffer(1) AliveList
        {
            uint u_Alive[];
        };

        ReadBuffer(2) AttributeFloats
        {
            float u_Floats[];
        };

        ReadBuffer(3) AttributeInts
        {
            int u_Ints[];
        };

        Buffer(4) Readback
        {
            uint u_Out[];
        };

        PushConstant PushConstants
        {
            uvec4 u_Page;  // x = the last compact's slot, y = first alive row, z = rows (0 = counts only), w = float columns
            uvec4 u_Range; // x = the drawn alive half's offset in AliveList, y = range count, z = pool base, w = serial
            uvec4 u_Columns; // x = int columns; yzw = 0
        };

        void main()
        {
            uint t     = gl_GlobalInvocationID.x;
            uint alive = u_Slots[u_Page.x].VertexCount / 6u;
            uint first = u_Page.y;
            uint rows  = first < alive ? min( u_Page.z, alive - first ) : 0u;
            if ( t == 0u )
            {
                u_Out[0] = alive;
                u_Out[1] = u_Slots[u_Page.x].FreeCount;
                u_Out[2] = u_Range.w;
                u_Out[3] = rows;
            }
            if ( t >= rows )
                return;
            uint index  = u_Alive[u_Range.x + first + t];
            uint local  = index - u_Range.z;
            uint stride = 1u + u_Page.w + u_Columns.x;
            uint base   = 4u + t * stride;
            u_Out[base] = index;
            for ( uint c = 0u; c < u_Page.w; ++c )
                u_Out[base + 1u + c] = floatBitsToUint( u_Floats[c * u_Range.y + local] );
            for ( uint c = 0u; c < u_Columns.x; ++c )
                u_Out[base + 1u + u_Page.w + c] = uint( u_Ints[c * u_Range.y + local] );
        }
    }
}
