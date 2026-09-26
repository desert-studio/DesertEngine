// DesertAsset {"Kind":"Shader","Guid":"a4151c46e9a6c08adc882779439b4699","Versions":{"SHDR":1},"Dependencies":[]}
// Text IN THE WORLD: samples the multi-channel signed-distance font atlas and outputs EMISSIVE HDR
// colour so the existing bloom pass picks up bright text for free (EmissiveIntensity > ~1 blooms).
// Alpha-blended over the scene.
//
// A world label is the hard case the whole distance-field arrangement exists for: it is read from
// touching distance, from the horizon, and at any angle in between, so neither the edge width nor the
// corner quality may depend on a size chosen anywhere. Both come out of Common/SdfText.glslh from the
// screen-space derivative of the ATLAS UV, which is the one quantity that already knows the distance,
// the foreshortening and the perspective divide.
Shader "TextSDF"
{
    Domain Surface

    Properties Binding(1) TextureBinding(2)
    {
        Color     TextColor         ("Text Color") = (1, 1, 1, 1)
        float     EmissiveIntensity ("Emissive Intensity") = 1.0
        Texture2D u_SDFAtlas        ("SDF Atlas")
    }

    State
    {
        Cull None
        ZTest LEqual
        ZWrite Off
        Blend SrcAlpha OneMinusSrcAlpha
    }

    Vertex
    {
        In(0) vec3 a_Position;
        In(1) vec3 a_Normal;
        In(2) vec3 a_Tangent;
        In(3) vec3 a_Bitangent;
        In(4) vec2 a_TextureCoord;

        #include <Common/CameraUB.glslh>

        // Transform + the material row index, in the one block both stages declare (see the header).
        // Text is the producer that needed the row most: every 3D label in a scene draws through ONE
        // shared TextSDF material, so a per-material colour block gave them all the first label's colour.
        #include <Common/MaterialTransport.glslh>

        Out(0) vec2 v_UV;

        void main()
        {
            v_UV        = a_TextureCoord;
            gl_Position = cameraUB.Projection * cameraUB.View * m_PushConstants.Transform * vec4( a_Position, 1.0 );
        }
    }

    Fragment
    {
        In(0) vec2 v_UV;
        Out(0) vec4 o_Color;

        #include <Common/SdfText.glslh>

        void main()
        {
            vec3  msd     = texture( u_SDFAtlas, v_UV ).rgb;
            float pxRange = SdfTextScreenPxRange( fwidth( v_UV ), vec2( textureSize( u_SDFAtlas, 0 ) ) );
            float alpha   = SdfTextAlpha( msd, pxRange );

            if ( alpha <= 0.0 )
                discard;

            vec3 emissive = u_Material.TextColor.rgb * u_Material.EmissiveIntensity;
            o_Color       = vec4( emissive, alpha * u_Material.TextColor.a );
        }
    }
}
