// DesertAsset {"Kind":"Shader","Guid":"62162bdefe547edc0dad159167dd342f","Versions":{"SHDR":1},"Dependencies":[]}
Shader "TerrainShadow"
{
    // The terrain as a cascade shadow caster: depth only, into the cascade targets the mesh casters write
    // (MeshRenderer's cascade pass records it through IShadowCaster). The grid, its LOD and its morph are the
    // camera pass's own vertex stage — and the LOD is the camera's, because the TerrainRenderer picks it from
    // the MAIN camera and passes it in the tile's row: a cascade sees exactly the triangles the camera sees,
    // so the shadow cannot swim or open a gap where a LOD change would move them. The vertex stage projects
    // with the pushed cascade matrix.
    //
    // No render state for depth compare: the cascades are STANDARD-Z (SetupShadowPass), so the
    // TerrainRenderer sets LessOrEqual itself; a `ZTest` here would be mirrored for the reversed-Z camera.

    State
    {
        Topology Triangles
        Cull None
        ZWrite On
    }

    Vertex
    {
        // Positions only: the normal and the height tint are for shading, and nothing shades a caster.
        #define TERRAIN_DEPTH_ONLY
        #include <Programs/Terrain/TerrainVertex.glslh>
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
