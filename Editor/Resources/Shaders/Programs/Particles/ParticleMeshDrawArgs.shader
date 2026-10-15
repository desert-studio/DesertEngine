// DesertAsset {"Kind":"Shader","Guid":"055be538c8d8448fa30fc9cc8e7e47bf","Versions":{"SHDR":1},"Dependencies":[]}
Shader "ParticleMeshDrawArgs"
{
    Compute
    {
        // "Particles: Mesh Args {draw}" (UE FNiagaraGPUInstanceCountManager::UpdateDrawIndirectBuffers): one
        // VkDrawIndexedIndirectCommand per mesh section of a Mesh renderer draw, from the emitter's draw slot the
        // last compact filled - instanceCount = the alive count, firstInstance = the alive half's start (so
        // gl_InstanceIndex is an entry of the alive list, Mesh/Surface/Vertex_ParticleMesh.glslh). One dispatch of
        // one thread per section; the section's index range comes in the push block.
        LocalSize(1, 1, 1);

        #include <Common/ParticlePool.glslh>

        ReadBuffer(0) Counters
        {
            ParticleDrawSlot u_Slots[];
        };

        Buffer(1) MeshDrawArgs
        {
            uint u_Args[];
        };

        PushConstant PushConstants
        {
            uvec4 u_Section; // x = index count, y = first index, z = vertex offset, w = the draw slot
            uvec4 u_Out;     // x = the command's first uint in MeshDrawArgs
        };

        void main()
        {
            ParticleDrawSlot slot = u_Slots[u_Section.w];
            uint             o    = u_Out.x;
            u_Args[o]             = u_Section.x;
            u_Args[o + 1u]        = slot.VertexCount / 6u;
            u_Args[o + 2u]        = u_Section.y;
            u_Args[o + 3u]        = u_Section.z;
            u_Args[o + 4u]        = slot.FirstVertex / 6u;
        }
    }
}
