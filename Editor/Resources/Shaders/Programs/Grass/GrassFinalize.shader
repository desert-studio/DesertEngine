// DesertAsset {"Kind":"Shader","Guid":"9af5b17398544149b3546d72bc857b7b","Versions":{"SHDR":1},"Dependencies":[]}
// After GrassGenerate: each bucket's appended count, clamped to its capacity, becomes the instanceCount of the
// records that draw it and its first instance their firstInstance (GpuInstances.hpp: one bucket for every section of
// a mesh, bucket s for record s of a blade strip), the UNCLAMPED counts (the generator's atomic counts past the
// capacity) go to Feedback for the renderer to size the next capacities from (GrassRenderer, GPU-count feedback), and
// the counters are reset for the next frame's generation. One thread.
Shader "GrassFinalize"
{
    Compute
    {
        LocalSize(1, 1, 1);

        Buffer(0) Args
        {
            uint Arguments[]; // bucket count, capacity, submesh count, drawn, 5 words per submesh, 4 per bucket
        };

        Buffer(1) Feedback
        {
            uint FeedbackData[]; // per bucket the count the generator wanted, then 1 (this copy was written)
        };

        void main()
        {
            uint buckets = Arguments[0];
            uint table   = 4u + Arguments[2] * 5u;
            for ( uint s = 0u; s < Arguments[2]; ++s )
            {
                uint b = buckets == 1u ? 0u : min( s, buckets - 1u );
                uint at = table + b * 4u;
                Arguments[4u + s * 5u + 1u] = min( Arguments[at], Arguments[at + 2u] );
                Arguments[4u + s * 5u + 4u] = Arguments[at + 1u];
            }
            uint drawn = 0u;
            for ( uint b = 0u; b < buckets; ++b )
            {
                drawn += min( Arguments[table + b * 4u], Arguments[table + b * 4u + 2u] );
                FeedbackData[b]          = Arguments[table + b * 4u];
                Arguments[table + b * 4u] = 0u;
            }
            FeedbackData[buckets] = 1u;
            Arguments[3] = drawn; // the last frame's drawn count, for the stats readout
        }
    }
}
