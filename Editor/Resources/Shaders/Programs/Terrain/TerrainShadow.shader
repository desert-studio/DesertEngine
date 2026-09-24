Shader "TerrainShadow"
{
    // The terrain as a cascade shadow caster: depth only, into the cascade targets the mesh casters write
    // (MeshRenderer's cascade pass records it through IShadowCaster). The patch, its tessellation and its
    // displacement are the camera pass's own .glslh — and the tessellation level is the camera's, because
    // the control stage measures distance with TerrainUB.View, the MAIN camera, in this pass as well: a
    // cascade sees exactly the triangles the camera sees, so the shadow cannot swim or open a gap where a
    // camera-driven LOD change would move them. TessEval projects with the pushed cascade matrix.
    //
    // No render state for depth compare: the cascades are STANDARD-Z (SetupShadowPass), so the
    // TerrainRenderer sets LessOrEqual itself; a `ZTest` here would be mirrored for the reversed-Z camera.

    State
    {
        Topology Patches 4
        Cull None
        ZWrite On
    }

    Vertex
    {
        #include <Programs/Terrain/TerrainVertex.glslh>
    }

    TessControl
    {
        #include <Programs/Terrain/TerrainTessControl.glslh>
    }

    TessEval
    {
        // Positions only: the normal and the height tint are for shading, and nothing shades a caster.
        #define TERRAIN_DEPTH_ONLY
        #include <Programs/Terrain/TerrainTessEval.glslh>
    }

    Fragment
    {
        Out(0) vec4 o_Depth;

        void main()
        {
            // Same encoding as Shadow.shader: the R32F-in-RGBA32F target the receivers sample.
            o_Depth = vec4( gl_FragCoord.z, 0.0, 0.0, 1.0 );
        }
    }
}
