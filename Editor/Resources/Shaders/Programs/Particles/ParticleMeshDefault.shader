// DesertAsset {"Kind":"Shader","Guid":"5a4722e8f4d6405ca2d83dd5779d5e71","Versions":{"SHDR":1},"Dependencies":[]}
// The engine's mesh particle template (UE's DefaultMeshParticleMaterial): what a Mesh renderer row draws with when
// it names no material, and what ParticleRenderer falls back to BY NAME (logged) when the named one has no
// ParticleMesh.Forward cell. Opaque: the mesh's vertex colour times the particle's colour, unlit.
Shader "ParticleMeshDefault"
{
    Domain Surface
    Usage ParticleMeshes
    BlendMode Opaque

    Properties Binding(2)
    {
        Color Tint ("Tint") = (1, 1, 1, 1)
    }

    State
    {
        Cull None
        ZTest LEqual
        ZWrite Off
    }

    ShadingModel Unlit

    Surface
    {
        SurfaceOutput EvaluateSurface( SurfaceInput i )
        {
            SurfaceOutput s = DefaultSurfaceOutput();
            s.BaseColor     = vec3( 0.0 );
            s.Emissive      = i.VertexColor.rgb * u_Material.Tint.rgb;
            return s;
        }
    }
}
