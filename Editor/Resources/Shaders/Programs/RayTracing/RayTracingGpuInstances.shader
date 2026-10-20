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
            mat4 u_Transforms[]; // the producer's instances, GpuInstanceRefs::Instances
        };

        ReadBuffer(1) Args
        {
            uint u_Args[]; // GpuInstances.hpp: header, 5 words per indexed-indirect record, 4 per bucket
        };

        Buffer(2) TlasRows
        {
            uint u_TlasRows[]; // 16 words per VkAccelerationStructureInstanceKHR
        };

        PushConstant PushConstants
        {
            uint u_RowBase;      // the source's first row
            uint u_Rows;         // its reserved rows
            uint u_Record;       // the record stating the drawn count (instanceCount) and first instance
            uint u_IndexAndMask; // customIndex:24 | mask:8
            uint u_SbtAndFlags;  // sbtOffset:24 | flags:8
            uint u_AddressLo;    // the BLAS device address
            uint u_AddressHi;
            uint u_Pad;
        };

        void main()
        {
            uint i = gl_GlobalInvocationID.x;
            if ( i >= u_Rows )
                return;
            uint row    = ( u_RowBase + i ) * 16u;
            uint record = 4u + u_Record * 5u;
            if ( i >= u_Args[record + 1u] )
            {
                for ( uint w = 0u; w < 16u; ++w )
                    u_TlasRows[row + w] = 0u;
                return;
            }
            mat4 world = u_Transforms[u_Args[record + 4u] + i];
            for ( uint r = 0u; r < 3u; ++r )
                for ( uint c = 0u; c < 4u; ++c )
                    u_TlasRows[row + r * 4u + c] = floatBitsToUint( world[c][r] ); // row-major 3x4
            u_TlasRows[row + 12u] = u_IndexAndMask;
            u_TlasRows[row + 13u] = u_SbtAndFlags;
            u_TlasRows[row + 14u] = u_AddressLo;
            u_TlasRows[row + 15u] = u_AddressHi;
        }
    }
}
