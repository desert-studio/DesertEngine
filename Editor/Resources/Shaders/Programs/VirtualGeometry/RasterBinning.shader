// DesertAsset {"Kind":"Shader","Guid":"5b0e7f2c9a1d4c3e8f6a7b2d1c0e9f84","Versions":{"SHDR":1},"Dependencies":[]}
Shader "RasterBinning"
{
    // "VirtualGeometry: Raster Binning" (Engine/Graphic/VirtualGeometry/VirtualGeometryRenderer.cpp
    // AddRasterBinning; UE 5.8 Shaders/Private/Nanite/NaniteRasterBinning.usf): the cut's HW clusters
    // (VisibleClusters[VisibleCapacity - 1 - i], i < RasterArgs.HwDraw[1]) sorted into the material groups of ONE
    // mesh pass, so each group draws its clusters with its own `.VirtualGeometry` cell (one draw instance = one
    // cluster, Common/GpuScene.glslh DesertResolveVgDraw). A cluster's group is the pass's: the lod-0 draw slot of
    // its DAG's submesh (FirstDrawSlot + Dag.Submesh, the slot the cut took its transform from) -> the slot's bin
    // -> the bin's IndirectGroup. A slot with no bin in this pass (the submesh does not draw there) bins nothing.
    //
    // One program, three steps (Params.x), each its own dispatch (the RDG orders them):
    //  0 COUNT    one thread per HW cluster: GroupArgs[group].instanceCount += 1 (and, when Params2.w names the
    //             cut views, GroupViewCounts[group x views + its view] += 1: what each view of the cut drew).
    //  1 OFFSETS  one thread: the groups' exclusive prefix -> GroupArgs[g] = { kClusterMaxTriangles x 3, 0, 0,
    //             first }, the count kept in GroupCursor[g] for the step-2 check.
    //  2 SCATTER  one thread per HW cluster: Rows[first + atomicAdd( instanceCount )] = its VisibleClusters row.
    // Rows is the draw's instance id stream (binding 2, a_VisibleRow): instance-rate, so firstInstance offsets it
    // on every driver.
    Compute
    {
        LocalSize(64, 1, 1);

        #include <Common/GpuScene.glslh>
        #include <Common/VirtualGeometry.glslh>

        ReadBuffer(0) RasterBinningInstances
        {
            GpuSceneInstance Instances[];
        };
        ReadBuffer(1) RasterBinningPrimitiveDraws
        {
            GpuPrimitivePassDraws PrimitiveDraws[];
        };
        ReadBuffer(2) RasterBinningDrawSlots
        {
            GpuDrawSlot DrawSlots[];
        };
        ReadBuffer(3) RasterBinningDrawBins
        {
            GpuDrawBin DrawBins[];
        };
        ReadBuffer(4) RasterBinningDags
        {
            VgClusterDag Dags[];
        };
        ReadBuffer(5) RasterBinningClusters
        {
            VgCluster Clusters[];
        };
        ReadBuffer(6) RasterBinningVisibleClusters
        {
            VgVisibleCluster Visible[];
        };
        ReadBuffer(7) RasterBinningRasterArgs
        {
            VgRasterArgs RasterArgs;
        };
        Buffer(8) RasterBinningGroupArgs
        {
            uint GroupArgs[]; // VkDrawIndirectCommand per group: vertexCount, instanceCount, firstVertex, firstInstance
        };
        Buffer(9) RasterBinningGroupCursor
        {
            uint GroupCursor[]; // step 1: the group's count after COUNT (SCATTER refills instanceCount up to it)
        };
        Buffer(10) RasterBinningRows
        {
            uint Rows[]; // [VisibleCapacity]: the VisibleClusters row of each draw instance
        };
        Buffer(11) RasterBinningGroupViewCounts
        {
            uint GroupViewCounts[]; // [groups x Params2.w]: the clusters of group g in cut view v (Params2.w > 0)
        };

        PushConstant PushConstants
        {
            uvec4 Params;  // x = step, y = visible capacity, z = group count, w = primitive rows of the pass table
            // x = draw slot rows, y = bin rows, z = vertices per cluster draw, w = the cut views GroupViewCounts
            // counts (0 = none)
            uvec4 Params2;
        };

        #define kStepCount   0u
        #define kStepOffsets 1u
        #define kStepScatter 2u

        // The group of HW cluster @p i in this pass, VG_INVALID_INDEX when the pass does not draw it.
        uint GroupOf( uint visibleRow )
        {
            const VgVisibleCluster visible   = Visible[visibleRow];
            const uint             primitive = Instances[visible.Instance].Primitive;
            if ( primitive >= Params.w )
                return VG_INVALID_INDEX;
            const GpuPrimitivePassDraws draws = PrimitiveDraws[primitive];
            const uint                  sub   = Dags[Clusters[visible.Cluster].Dag].Submesh;
            if ( sub >= draws.SubmeshCount )
                return VG_INVALID_INDEX;
            const uint slot = draws.FirstDrawSlot + sub; // lod 0
            if ( slot >= Params2.x )
                return VG_INVALID_INDEX;
            const uint bin = DrawSlots[slot].Bin;
            if ( bin >= Params2.y )
                return VG_INVALID_INDEX;
            const uint group = DrawBins[bin].Group;
            return group < Params.z ? group : VG_INVALID_INDEX;
        }

        void main()
        {
            const uint step = Params.x;
            if ( step == kStepOffsets )
            {
                if ( gl_GlobalInvocationID.x != 0u )
                    return;
                uint first = 0u;
                for ( uint g = 0u; g < Params.z; ++g )
                {
                    const uint count       = GroupArgs[g * 4u + 1u];
                    GroupCursor[g]         = count;
                    GroupArgs[g * 4u + 0u] = Params2.z;
                    GroupArgs[g * 4u + 1u] = 0u;
                    GroupArgs[g * 4u + 2u] = 0u;
                    GroupArgs[g * 4u + 3u] = first;
                    first += count;
                }
                return;
            }
            const uint i = gl_GlobalInvocationID.x;
            if ( i >= min( RasterArgs.HwDraw[1], Params.y ) )
                return;
            const uint row   = Params.y - 1u - i;
            const uint group = GroupOf( row );
            if ( group == VG_INVALID_INDEX )
                return;
            if ( step == kStepCount )
            {
                atomicAdd( GroupArgs[group * 4u + 1u], 1u );
                const uint view = Visible[row].View;
                if ( view < Params2.w )
                    atomicAdd( GroupViewCounts[group * Params2.w + view], 1u );
                return;
            }
            // SCATTER: the same clusters in the same groups as COUNT, so a group never takes more than its count.
            const uint k = atomicAdd( GroupArgs[group * 4u + 1u], 1u );
            if ( k < GroupCursor[group] )
                Rows[GroupArgs[group * 4u + 3u] + k] = row;
        }
    }
}
