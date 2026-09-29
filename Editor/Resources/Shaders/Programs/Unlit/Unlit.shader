// DesertAsset {"Kind":"Shader","Guid":"68041dd187501b26980a7415a7b882fc","Versions":{"SHDR":1},"Dependencies":[]}
// The flat-colour surface template (UE's MSM_Unlit): a `Domain Surface` shader like StandardSurface, so it has
// no hand-written program of its own — its (Forward|GBuffer|ShadowDepth) cells are the engine's vertex-path and
// Pass_*_Unlit headers around EvaluateSurface below, and those headers name no lighting text or resource. The
// Properties block drives the Details UI and the material's row of `Materials[]`; the albedo map is declared in
// the Surface block on the shared material layout's albedo slot, exactly as StandardSurface declares it.
Shader "Unlit"
{
    Domain Surface
    Role DebugColor

    // The import contract (see StaticMeshPBR): this template takes only a glTF material that declares
    // KHR_materials_unlit, and it takes it over StaticMeshPBR because it requires more of the source.
    Import
    {
        Requires "gltf.KHR_materials_unlit"
        "gltf.baseColorFactor"  -> Color
        "gltf.baseColorTexture" -> u_AlbedoTexture
    }

    Properties Binding(2)
    {
        Color     Color           ("Color")  = (0.8, 0.4, 0.1, 1)
        Texture2D u_AlbedoTexture ("Albedo")
    }

    State
    {
        Cull Back
        ZTest LEqual
        ZWrite On
    }

    ShadingModel Unlit

    Surface
    {
        layout( binding = 11 ) uniform sampler2D u_AlbedoTexture;

        SurfaceOutput EvaluateSurface( SurfaceInput i )
        {
            const vec4 colour = texture( u_AlbedoTexture, i.UV0 ) * u_Material.Color;
            SurfaceOutput s   = DefaultSurfaceOutput();
            s.BaseColor       = vec3( 0.0 );
            s.Emissive        = colour.rgb;
            s.Opacity         = colour.a;
            return s;
        }
    }
}
