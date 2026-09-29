// DesertAsset {"Kind":"Shader","Guid":"49d552d5d3df4d40bb0a4bb1e78fe15a","Versions":{"SHDR":1},"Dependencies":[]}
// The engine's DEFAULT SURFACE (AL1-12, UE's WorldGridMaterial role): what a mesh is drawn with while its own
// material's pipeline is still compiling on a worker, or after the driver refused it. A global shader: it is
// compiled at startup and its pipeline is one the reveal waits for, so the fallback itself never falls back.
// Lit mid-grey with a 100 cm world-space checker, so a stand-in is recognisable as one and not as a material.
Shader "DefaultSurface"
{
    Domain Surface

    State
    {
        Cull Back
        ZTest LEqual
        ZWrite On
    }

    Vertex
    {
        #define GRAPH_LIT 1
        #include <Common/GraphVertex.glslh>
    }

    Fragment
    {
        layout( location = 0 ) in vec2 v_UV;
        layout( location = 1 ) in vec3 v_Normal;
        layout( location = 2 ) in vec3 v_WorldPos;
        layout( location = 3 ) in vec3 v_CameraPos;
        layout( location = 0 ) out vec4 o_Color;

        #include <Common/GraphSurfaceLighting.glslh>

        void main()
        {
            // 1 unit = 1 cm: one checker cell per metre.
            ivec3 cell    = ivec3( floor( v_WorldPos / 100.0 ) );
            float checker = float( ( cell.x + cell.y + cell.z ) & 1 );
            vec3  albedo  = mix( vec3( 0.30 ), vec3( 0.45 ), checker );
            vec3  N       = normalize( v_Normal );
            vec3  view    = normalize( v_CameraPos - v_WorldPos );
            o_Color       = vec4( ShadeGraphSurface( v_WorldPos, N, view, albedo, 0.0, 0.6, 1.0 ), 1.0 );
        }
    }
}
