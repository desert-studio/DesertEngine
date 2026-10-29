// DesertAsset {"Kind":"Shader","Guid":"692a1274228f4c0aba0ad586acc699ff","Versions":{"SHDR":1},"Dependencies":[]}
// The engine's textured additive particle sprite (UE's additive particle material: TextureSample over a
// ParticleSubUV, times ParticleColor). The look is the MATERIAL's texture - a flash, a spark streak, a flipbook
// sheet of an explosion - not anything drawn here: the shader only samples the authored image at the particle's
// flipbook cell and tints it with the simulated colour. Columns x Rows = 1 x 1 is a single image; more is a
// SubUV sheet stepped over the particle's life (DesertParticleSubUV), cells blended when BlendFrames is on (UE
// SubUV "Blended"). Additive (SrcAlpha, One): the texture's alpha times the particle's alpha is how much is added.
Shader "ParticleSpriteTextured"
{
    Domain Surface
    Usage ParticleSprites
    BlendMode Additive
    // UE bResponsiveAA: a sprite writes no velocity of its own; its coverage tells TAA to trust the current frame.
    ResponsiveAA

    Properties Binding(2)
    {
        Color     Tint ("Tint", Category("Surface")) = (1, 1, 1, 1)
        // Emissive scale: an additive spark is a light source; HDR values above 1 reach the bloom.
        Float     Intensity ("Intensity", Range(0,64), Category("Surface")) = 1
        Texture2D SpriteTexture ("Sprite", Category("Textures"), Intent(Colour))
        Float     SubUVColumns ("SubUV Columns", Range(1,16), Category("SubUV")) = 1
        Float     SubUVRows ("SubUV Rows", Range(1,16), Category("SubUV")) = 1
        Float     BlendFrames ("Blend Frames", Range(0,1), Category("SubUV")) = 1
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
            const SurfaceSubUV cell  = DesertParticleSubUV( i.Particle, i.UV0, Material.SubUVColumns, Material.SubUVRows );
            const vec4         a     = SurfaceSampleMaterial( Material.SpriteTexture, cell.UV0 );
            const vec4         b     = SurfaceSampleMaterial( Material.SpriteTexture, cell.UV1 );
            const vec4         texel = mix( a, b, cell.Blend * clamp( Material.BlendFrames, 0.0, 1.0 ) );
            const vec4         tint  = i.Particle.Color * Material.Tint;
            SurfaceOutput      s     = DefaultSurfaceOutput();
            s.BaseColor              = vec3( 0.0 );
            s.Emissive               = texel.rgb * tint.rgb * Material.Intensity;
            s.Opacity                = texel.a * tint.a;
            return s;
        }
    }
}
