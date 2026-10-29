// DesertAsset {"Kind":"Shader","Guid":"a1e8438cb2974f95ab07bf109bd52564","Versions":{"SHDR":1},"Dependencies":[]}
// THE GPU-WRITTEN TLAS ROWS (RayTracingSceneRenderer.hpp, RayTracingGpuInstanceRows; UE RayTracingInstanceBufferUtil's
// GPU instance copy): one thread per row a GPU-instanced source reserved. A row under the drawn count becomes a
// VkAccelerationStructureInstanceKHR of the instance the producer generated - its transform the very mat4 the
// raster's vertex stage reads (InstanceTransforms), its count the indexed-indirect record GrassFinalize wrote; a
// row past it is inactive (every word 0: mask 0 and no BLAS, UE does the same).
Shader "RayTracingGpuInstances"
{
    Compute
    {
        LocalSize(64, 1, 1);

        ReadBuffer(0) Transforms
        {
            mat4 InstanceTransforms[]; // the producer's instances, GpuInstanceRefs::Instances
        };

        ReadBuffer(1) Args
        {
            uint Arguments[]; // GpuInstances.hpp: header, 5 words per indexed-indirect record, 4 per bucket
        };

        Buffer(2) TlasRows
        {
            uint TlasRowWords[]; // 16 words per VkAccelerationStructureInstanceKHR
        };

        PushConstant PushConstants
        {
            uint RowBase;      // the source's first row
            uint Rows;         // its reserved rows
            uint Record;       // the record stating the drawn count (instanceCount) and first instance
            uint IndexAndMask; // customIndex:24 | mask:8
            uint SbtAndFlags;  // sbtOffset:24 | flags:8
            uint AddressLo;    // the BLAS device address
            uint AddressHi;
            uint Pad;
        };

        void main()
        {
            uint i = gl_GlobalInvocationID.x;
            if ( i >= Rows )
                return;
            uint row    = ( RowBase + i ) * 16u;
            uint record = 4u + Record * 5u;
            if ( i >= Arguments[record + 1u] )
            {
                for ( uint w = 0u; w < 16u; ++w )
                    TlasRowWords[row + w] = 0u;
                return;
            }
            mat4 world = InstanceTransforms[Arguments[record + 4u] + i];
            for ( uint r = 0u; r < 3u; ++r )
                for ( uint c = 0u; c < 4u; ++c )
                    TlasRowWords[row + r * 4u + c] = floatBitsToUint( world[c][r] ); // row-major 3x4
            TlasRowWords[row + 12u] = IndexAndMask;
            TlasRowWords[row + 13u] = SbtAndFlags;
            TlasRowWords[row + 14u] = AddressLo;
            TlasRowWords[row + 15u] = AddressHi;
        }
    }
}
