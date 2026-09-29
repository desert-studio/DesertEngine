// DesertAsset {"Kind":"Shader","Guid":"4df6032e07eb331a9b4c25ef18780bb5","Versions":{"SHDR":1},"Dependencies":[]}
Shader "DebugLine"
{
    Vertex
    {
        // Vertexless debug line list: each vertex is pulled from the Lines storage buffer by gl_VertexIndex and
        // transformed by the shared camera. Lines topology -> every 2 vertices form one world-space segment.
        // Used for AABB wireframes (Scene Settings -> Debug -> Show Bounding Boxes); reusable for other gizmos.

        #include <Common/CameraUB.glslh>

        struct LineVertex
        {
            vec4 PositionWS; // xyz = world position (w unused)
            vec4 Color;      // rgba
        };

        ReadBuffer(1) Lines
        {
            LineVertex u_Lines[];
        };

        Out(0) vec4 v_Color;

        void main()
        {
            LineVertex lv = u_Lines[gl_VertexIndex];
            gl_Position   = cameraUB.Projection * cameraUB.View * vec4(lv.PositionWS.xyz, 1.0);
            v_Color       = lv.Color;
        }
    }

    Fragment
    {
        In(0)  vec4 v_Color;
        Out(0) vec4 oColor;

        void main()
        {
            oColor = v_Color;
        }
    }
}
