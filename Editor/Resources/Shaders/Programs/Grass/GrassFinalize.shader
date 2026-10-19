// DesertAsset {"Kind":"Shader","Guid":"9af5b17398544149b3546d72bc857b7b","Versions":{"SHDR":1},"Dependencies":[]}
// After GrassGenerate: each bucket's appended count, clamped to its capacity, becomes the instanceCount of the
// records that draw it and its first instance their firstInstance (GpuInstances.hpp: one bucket for every section of
// a mesh, bucket s for record s of a blade strip), and the counters are reset for the next frame's generation. One
// thread.
Shader "GrassFinalize"
{
    Compute
    {
        LocalSize(1, 1, 1);

        Buffer(0) Args
        {
            uint u_Args[]; // bucket count, capacity, submesh count, drawn, 5 words per submesh, 4 per bucket
        };

        void main()
        {
            uint buckets = u_Args[0];
            uint table   = 4u + u_Args[2] * 5u;
            for ( uint s = 0u; s < u_Args[2]; ++s )
            {
                uint b = buckets == 1u ? 0u : min( s, buckets - 1u );
                uint at = table + b * 4u;
                u_Args[4u + s * 5u + 1u] = min( u_Args[at], u_Args[at + 2u] );
                u_Args[4u + s * 5u + 4u] = u_Args[at + 1u];
            }
            uint drawn = 0u;
            for ( uint b = 0u; b < buckets; ++b )
            {
                drawn += min( u_Args[table + b * 4u], u_Args[table + b * 4u + 2u] );
                u_Args[table + b * 4u] = 0u;
            }
            u_Args[3] = drawn; // the last frame's drawn count, for the stats readout
        }
    }
}
