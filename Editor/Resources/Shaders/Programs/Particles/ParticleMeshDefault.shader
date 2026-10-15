// DesertAsset {"Kind":"Shader","Guid":"5a4722e8f4d6405ca2d83dd5779d5e71","Versions":{"SHDR":1},"Dependencies":[]}
// The engine's mesh particle template (UE's DefaultMeshParticleMaterial): what a Mesh renderer row draws with when
// it names no material, and what ParticleRenderer falls back to BY NAME (logged) when the named one has no
// ParticleMesh.Forward cell. Opaque: the mesh's vertex colour times the particle's colour as its base colour, lit
// like any mesh (the default shading model) on both render paths - by its ParticleMesh.GBuffer cell on the deferred
// one (and into the RSM), by its lit ParticleMesh.Forward cell in the forward one's base pass - and casting through
// its ParticleMesh.ShadowDepth cell into the cascades and the point / spot lights' views. Only its Cull reaches a
// pipeline (two-sided): every node that draws it sets its own depth state.
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

    Surface
    {
        SurfaceOutput EvaluateSurface( SurfaceInput i )
        {
            SurfaceOutput s = DefaultSurfaceOutput();
            s.BaseColor     = i.VertexColor.rgb * u_Material.Tint.rgb;
            return s;
        }
    }
}
