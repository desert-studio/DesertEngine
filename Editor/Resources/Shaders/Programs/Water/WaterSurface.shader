// DesertAsset {"Kind":"Shader","Guid":"bcefeba392c545f6b95dcb1c2554b7a8","Versions":{"SHDR":1},"Dependencies":[]}
Shader "WaterSurface"
{
    // THE WATER SURFACE TEMPLATE (UE: a Surface-domain material of ShadingModel SingleLayerWater, drawn through the
    // water mesh vertex factory). Its one drawn cell is `WaterMesh.SingleLayerWater` (Usage WaterMesh): every tile
    // the water quadtree selected, drawn by Graphic::System::WaterRenderer with Mesh/Surface/Pass_SingleLayerWater.glslh
    // - the physics of light in water. The LOOK is the body's `.demat` of this template: the ripple normal maps and
    // their scroll, roughness and specular (EvaluateSurface), the volume's scattering, absorption, phase and the tint
    // behind (EvaluateSingleLayerWater), the foam's texture, tint and thresholds. Nothing here states a colour of its own.
    Domain Surface
    Usage WaterMesh
    TwoSided

    Properties Binding(2)
    {
        Color       ScatteringCoefficients ("Scattering Coefficients (1/m)", Category("Water")) = (0.02, 0.06, 0.06, 1)
        Color       AbsorptionCoefficients ("Absorption Coefficients (1/m)", Category("Water")) = (0.45, 0.09, 0.06, 1)
        Float       PhaseG ("Phase G", Range(-0.9,0.9), Category("Water")) = 0.3
        Color       ColorScaleBehindWater ("Color Scale Behind Water", Category("Water")) = (1, 1, 1, 1)
        Float       Roughness ("Roughness", Range(0.02,1), Category("Surface")) = 0.05
        Float       Specular ("Specular", Range(0,1), Category("Surface")) = 0.255
        Float       RefractionStrength ("Refraction Strength (cm)", Range(0,200), Category("Surface")) = 40
        Texture2D   RippleNormalA ("Ripple Normal A", Category("Ripples"), Intent(NormalMap)) = "normal"
        Texture2D   RippleNormalB ("Ripple Normal B", Category("Ripples"), Intent(NormalMap)) = "normal"
        Float       RippleTileSizeA ("Ripple Tile Size A (cm)", Range(10,10000), Category("Ripples")) = 400
        Float       RippleTileSizeB ("Ripple Tile Size B (cm)", Range(10,10000), Category("Ripples")) = 170
        Float       RippleAngleA ("Ripple Direction A (deg)", Range(-180,180), Category("Ripples")) = 20
        Float       RippleAngleB ("Ripple Direction B (deg)", Range(-180,180), Category("Ripples")) = -70
        Float       RippleSpeed ("Ripple Speed (cm/s)", Range(0,500), Category("Ripples")) = 20
        Float       RippleStrength ("Ripple Strength", Range(0,4), Category("Ripples")) = 1
        Texture2D   FoamTexture ("Foam Colour", Category("Foam"), Intent(Colour)) = "white"
        Texture2D   FoamStructure ("Foam Structure", Category("Foam"), Intent(Mask)) = "white"
        Color       FoamTint ("Foam Tint", Category("Foam")) = (1, 1, 1, 1)
        Float       FoamTileSize ("Foam Tile Size (cm)", Range(10,10000), Category("Foam")) = 300
        Float       FoamJacobianThreshold ("Whitecap Jacobian Threshold", Range(-1,1), Category("Foam")) = 0.5
        Float       FoamJacobianSoftness ("Whitecap Jacobian Softness", Range(0.01,1), Category("Foam")) = 0.25
        Float       FoamDecaySeconds ("Foam Decay (s)", Range(0.1,30), Category("Foam")) = 3
        Float       FoamBreakerIndex ("Breaker Index H/d", Range(0.4,1.5), Category("Foam")) = 0.78
        Float       ShoreFadeCm ("Shore Fade Depth (cm)", Range(0,500), Category("Shore")) = 40
        Float       DebugFoamMask ("Debug: Foam Mask Only", Range(0,1), Category("Debug")) = 0
    }

    ShadingModel SingleLayerWater

    Surface
    {
        // One ripple layer: a tangent-space normal map scrolled across the water at its own size and heading. UV v
        // runs with -Z, as the water mesh's tangent frame (Vertex_WaterMesh.glslh).
        vec3 WaterRipple( BindlessTexture map, vec2 worldXZ, float tileCm, float angleDeg, float speed, float seconds )
        {
            const float r   = radians( angleDeg );
            const vec2  dir = vec2( cos( r ), sin( r ) );
            const vec2  p   = ( worldXZ + dir * speed * seconds ) / max( tileCm, 1.0 );
            return SurfaceSampleTangentNormal( map, vec2( p.x, -p.y ) );
        }

        SurfaceOutput EvaluateSurface( SurfaceInput i )
        {
            SurfaceOutput s = DefaultSurfaceOutput();
            // Two ripple maps crossing (UE's water material's detail normals), whiteout-blended, tilted by strength.
            const vec3 a = WaterRipple( Material.RippleNormalA, i.WorldPosition.xz, Material.RippleTileSizeA,
                                        Material.RippleAngleA, Material.RippleSpeed, i.Time );
            const vec3 b = WaterRipple( Material.RippleNormalB, i.WorldPosition.xz, Material.RippleTileSizeB,
                                        Material.RippleAngleB, Material.RippleSpeed, i.Time );
            s.Normal    = normalize( vec3( ( a.xy + b.xy ) * Material.RippleStrength, a.z * b.z ) );
            s.BaseColor = vec3( 0.0 );
            s.Metallic  = 0.0;
            s.Roughness = Material.Roughness;
            s.Specular  = Material.Specular;
            return s;
        }

        SingleLayerWaterOutput EvaluateSingleLayerWater( SurfaceInput i )
        {
            SingleLayerWaterOutput w;
            w.ScatteringCoefficients = Material.ScatteringCoefficients.rgb;
            w.AbsorptionCoefficients = Material.AbsorptionCoefficients.rgb;
            w.PhaseG                 = Material.PhaseG;
            w.ColorScaleBehindWater  = Material.ColorScaleBehindWater.rgb;
            w.RefractionOffset       = Material.RefractionStrength;
            // The foam rides the water particle (UV1 = its still-water X, Z; Vertex_WaterMesh.glslh), at two scales
            // so the sheet does not repeat.
            const vec2 foamUv        = vec2( i.UV1.x, -i.UV1.y ) / max( Material.FoamTileSize, 1.0 );
            const vec2 foamUv2       = mat2( 0.8, -0.6, 0.6, 0.8 ) * foamUv * 0.37 + vec2( 0.31, 0.17 );
            w.FoamAlbedo             = SurfaceSampleMaterial( Material.FoamTexture, foamUv ).rgb * Material.FoamTint.rgb;
            w.FoamCoverage           = 0.5 * ( SurfaceSampleMaterial( Material.FoamStructure, foamUv ).r +
                                     SurfaceSampleMaterial( Material.FoamStructure, foamUv2 ).r );
            w.FoamJacobianThreshold  = Material.FoamJacobianThreshold;
            w.FoamJacobianSoftness   = Material.FoamJacobianSoftness;
            w.FoamDecaySeconds       = Material.FoamDecaySeconds;
            w.FoamBreakerIndex       = Material.FoamBreakerIndex;
            w.ShoreFadeCm            = Material.ShoreFadeCm;
            w.DebugFoamMask          = Material.DebugFoamMask;
            return w;
        }
    }
}
