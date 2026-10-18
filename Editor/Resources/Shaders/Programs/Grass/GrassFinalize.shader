// DesertAsset {"Kind":"Shader","Guid":"9af5b17398544149b3546d72bc857b7b","Versions":{"SHDR":1},"Dependencies":[]}
// After GrassGenerate: the appended count, clamped to the capacity, becomes every section record's instanceCount
// (GpuInstances.hpp), and the counter is reset for the next frame's generation. One thread.
Shader "GrassFinalize"
{
    Compute
    {
        LocalSize(1, 1, 1);

        Buffer(0) Args
        {
            uint u_Args[]; // count, capacity, submesh count, 0, then 5 words per submesh
        };

        void main()
        {
            uint count = min( u_Args[0], u_Args[1] );
            for ( uint s = 0u; s < u_Args[2]; ++s )
                u_Args[4u + s * 5u + 1u] = count;
            u_Args[3] = count; // the last frame's drawn count, for the stats readout
            u_Args[0] = 0u;
        }
    }
}
