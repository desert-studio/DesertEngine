// DesertAsset {"Kind":"Shader","Guid":"6c1f0d9a4b2e47a8b35d91e0c7a4f216","Versions":{"SHDR":1},"Dependencies":[]}
// Debug Readback of one emitter (VFX-16, UE Niagara Debugger's GPU counts and particle attribute view). Dev only:
// dispatched by "Particles: Debug Readback" after the last compact, in the simulating view, only while an editor
// debugger asks (ParticleWorldGpu::SetDebugRequest). It copies what the GPU holds - no CPU-side guess:
//   header (4 uints): alive count and free count of the last compact's slot (Page.x), the debug frame serial
//                     (AliveRange.w, never 0), the rows this page wrote;
//   rows: per alive-list entry Page.y + t (t < Page.z, t < alive - first): the pool index, then the emitter's
//         Page.w float attribute components, then the Columns.x int components as their bits (SoA column c
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
            ParticleDrawSlot Slots[];
        };

        ReadBuffer(1) AliveList
        {
            uint AliveData[];
        };

        ReadBuffer(2) AttributeFloats
        {
            float FloatAttributes[];
        };

        ReadBuffer(3) AttributeInts
        {
            int IntAttributes[];
        };

        Buffer(4) Readback
        {
            uint ReadbackWords[];
        };

        PushConstant PushConstants
        {
            uvec4 Page;  // x = the last compact's slot, y = first alive row, z = rows (0 = counts only), w = float columns
            uvec4 AliveRange; // x = the drawn alive half's offset in AliveList, y = range count, z = pool base, w = serial
            uvec4 Columns; // x = int columns; yzw = 0
        };

        void main()
        {
            uint t     = gl_GlobalInvocationID.x;
            uint alive = Slots[Page.x].VertexCount / 6u;
            uint first = Page.y;
            uint rows  = first < alive ? min( Page.z, alive - first ) : 0u;
            if ( t == 0u )
            {
                ReadbackWords[0] = alive;
                ReadbackWords[1] = Slots[Page.x].FreeCount;
                ReadbackWords[2] = AliveRange.w;
                ReadbackWords[3] = rows;
            }
            if ( t >= rows )
                return;
            uint index  = AliveData[AliveRange.x + first + t];
            uint local  = index - AliveRange.z;
            uint stride = 1u + Page.w + Columns.x;
            uint base   = 4u + t * stride;
            ReadbackWords[base] = index;
            for ( uint c = 0u; c < Page.w; ++c )
                ReadbackWords[base + 1u + c] = floatBitsToUint( FloatAttributes[c * AliveRange.y + local] );
            for ( uint c = 0u; c < Columns.x; ++c )
                ReadbackWords[base + 1u + Page.w + c] = uint( IntAttributes[c * AliveRange.y + local] );
        }
    }
}
