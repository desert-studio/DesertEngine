// DesertAsset {"Kind":"Shader","Guid":"2cd21e52ff88b0c7a42c25d9a6ab4238","Versions":{"SHDR":1},"Dependencies":[]}
// The glass surface template (UE: a DefaultLit material with BlendMode Translucent and a Refraction input). Like
// every `Domain Surface` shader it is a Properties block plus ONE surface function; being Translucent, the parser
// gives it Forward cells only, built on Mesh/Surface/Pass_Forward_Translucent.glslh, and the mesh renderer draws
// its objects in the translucency pass BECAUSE the template is translucent (ShaderProgramMeta::Blend) — no
// renderer code reads a parameter of it to decide. Its identity is the GUID above, kept from the hand-written
// StaticMeshGlass program this template replaces.
Shader "StaticMeshGlass"
{
    Domain Surface
    BlendMode Translucent

    // The glass's own row: what its surface reads and nothing else. Its colour is the tint the scene behind it is
    // seen through; its coverage is the refraction itself (the pass writes it opaque over the scene it bent).
    Properties Binding(2)
    {
        Color       GlassTint ("Glass Tint", Category("Glass")) = (1, 1, 1, 1)
        Float       IOR ("IOR", Range(1,2.5), Category("Glass")) = 1.5
        // Empty = flat (0.5,0.5,1): a normal map is unpacked with `2*t - 1`, and a white texel is not "no detail".
        Texture2D   u_NormalTexture ("Normal Map", Category("Textures")) = "normal"
    }

    Surface
    {
        layout( binding = 12 ) uniform sampler2D u_NormalTexture;

        SurfaceOutput EvaluateSurface( SurfaceInput i )
        {
            SurfaceOutput s = DefaultSurfaceOutput();
            s.BaseColor     = u_Material.GlassTint.rgb;
            s.Refraction    = u_Material.IOR;
            const ivec2 normalSize = textureSize( u_NormalTexture, 0 );
            if ( normalSize.x > 1 && normalSize.y > 1 )
                s.Normal = SampleTangentNormal( u_NormalTexture, i.UV0 );
            return s;
        }
    }
}
