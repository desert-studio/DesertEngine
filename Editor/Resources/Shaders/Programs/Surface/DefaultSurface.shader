// DesertAsset {"Kind":"Shader","Guid":"49d552d5d3df4d40bb0a4bb1e78fe15a","Versions":{"SHDR":1},"Dependencies":[]}
// The engine's DEFAULT SURFACE (AL1-12, UE's WorldGridMaterial role): what a mesh is drawn with while its own
// material's pipeline is still compiling on a worker, or after the driver refused it. A global shader: it is
// compiled at startup and its pipeline is one the reveal waits for, so the fallback itself never falls back.
// Lit mid-grey with a 100 cm world-space checker, so a stand-in is recognisable as one and not as a material.
// The particle cells (UE: WorldGridMaterial is what a Niagara renderer draws when its material is invalid or lacks
// the usage flag) give the same checker to a sprite / ribbon row whose named material is broken, missing or has no
// particle cell (ParticleRenderer::ResolveSprite): a defect reads as the stand-in, never as an authored soft disc.
// Those cells are unlit (Pass_ParticleSprite.glslh: BaseColor + Emissive), so the checker is the two greys as is.
Shader "DefaultSurface"
{
    Domain Surface
    // Found by this role (MaterialService::StandInSurfaceShader), never by its name.
    Role StandInSurface
    Usage ParticleSprites
    Usage ParticleRibbons

    State
    {
        Cull Back
        ZTest LEqual
        ZWrite On
    }

    // A surface template like any material: its default program is the Static.Forward cell, and the other
    // cells give the stand-in to instanced and skinned meshes, the G-buffer and the shadow pass as well.
    Surface
    {
        SurfaceOutput EvaluateSurface( SurfaceInput i )
        {
            // 1 unit = 1 cm: one checker cell per metre.
            const ivec3 cell    = ivec3( floor( i.WorldPosition / 100.0 ) );
            const float checker = float( ( cell.x + cell.y + cell.z ) & 1 );
            SurfaceOutput s     = DefaultSurfaceOutput();
            s.BaseColor         = mix( vec3( 0.30 ), vec3( 0.45 ), checker );
            s.Metallic          = 0.0;
            s.Roughness         = 0.6;
            return s;
        }
    }
}
