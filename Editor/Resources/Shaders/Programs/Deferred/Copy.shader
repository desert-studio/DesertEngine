// DesertAsset {"Kind":"Shader","Guid":"0f46529c8bcc0c931957ba9c7f44cc66","Versions":{"SHDR":1},"Dependencies":[]}
Shader "Copy"
{
    // Full-screen image copy (scene colour snapshot for glass refraction).

    Vertex
    {
        #include <Common/QuadPositions.glslh>
        #include <Common/QuadTextureCoords.glslh>

        Out(0) vec2 v_TexCoord;

        void main()
        {
        	v_TexCoord  = QUAD_TEXTURE_COORDINATES[gl_VertexIndex];
        	gl_Position = vec4(QUAD_POSITIONS[gl_VertexIndex], 0.0, 1.0);
        }
    }

    Fragment
    {
        // Trivial full-screen copy: samples an input image and writes it out. Used to snapshot the composited scene
        // colour into a separate texture so the glass pass can sample it (refraction) without a read+write feedback
        // loop on the scene target.

        In(0) vec2 v_TexCoord;

        Uniform(1) sampler2D u_Input;

        Out(0) vec4 oColor;

        void main()
        {
        	oColor = texture(u_Input, v_TexCoord);
        }
    }
}
