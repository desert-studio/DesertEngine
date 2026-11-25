// DesertAsset {"Kind":"Shader","Guid":"bcefeba392c545f6b95dcb1c2554b7a8","Versions":{"SHDR":1},"Dependencies":[]}
Shader "WaterSurface"
{
    // THE WATER SURFACE TEMPLATE (UE: a Surface-domain material of ShadingModel SingleLayerWater, drawn through the
    // water mesh vertex factory). Its one drawn cell is `WaterMesh.SingleLayerWater` (Usage WaterMesh): every tile
    // the water quadtree selected, drawn by Graphic::System::WaterRenderer with Mesh/Surface/Pass_SingleLayerWater.glslh
    // - the physics of light in water. The LOOK is the body's `.demat` of this template: the ripple normal maps and
    // their scroll, roughness and specular (EvaluateSurface), the volume's scattering, absorption, phase and the tint
    // behind (EvaluateSingleLayerWater), the foam's structure, albedo, thresholds and decays. Nothing here states a colour of its own.
    Domain Surface
    Usage WaterMesh
    TwoSided
    // A new water material's template (Content Browser "New Water Material", Common::Content::kWaterMaterialRole).
    Role WaterMaterial

    Properties Binding(2)
    {
        Color       ScatteringCoefficients ("Scattering Coefficients (1/m)", Category("Water")) = (0.02, 0.06, 0.06, 1)
        Color       AbsorptionCoefficients ("Absorption Coefficients (1/m)", Category("Water")) = (0.45, 0.09, 0.06, 1)
        Float       PhaseG ("Phase G", Range(-0.9,0.9), Category("Water")) = 0.3
        Color       ColorScaleBehindWater ("Color Scale Behind Water", Category("Water")) = (1, 1, 1, 1)
        Float       Roughness ("Roughness", Range(0.02,1), Category("Surface")) = 0.05
        Float       Specular ("Specular", Range(0,1), Category("Surface")) = 0.255
        Texture2D   RippleNormalA ("Ripple Normal A", Category("Ripples"), Intent(NormalMap)) = "normal"
        Texture2D   RippleNormalB ("Ripple Normal B", Category("Ripples"), Intent(NormalMap)) = "normal"
        Float       RippleTileSizeA ("Ripple Tile Size A (cm)", Range(10,10000), Category("Ripples")) = 400
        Float       RippleTileSizeB ("Ripple Tile Size B (cm)", Range(10,10000), Category("Ripples")) = 170
        Float       RippleAngleA ("Ripple Direction A (deg)", Range(-180,180), Category("Ripples")) = 20
        Float       RippleAngleB ("Ripple Direction B (deg)", Range(-180,180), Category("Ripples")) = -70
        Float       RippleSpeed ("Ripple Speed (cm/s)", Range(0,500), Category("Ripples")) = 20
        Float       RippleStrength ("Ripple Strength", Range(0,4), Category("Ripples")) = 1
        Float       RippleSlopeRms ("Ripple Maps' Slope RMS", Range(0,1), Category("Ripples")) = 0.1
        Float       WindRippleMaxGain ("Wind Ripple Max Gain", Range(1,4), Category("Ripples")) = 2
        Texture2D   FoamHeight ("Foam Height (R cells, G bubbles)", Category("Foam"), Intent(Mask)) = "white"
        Texture2D   FoamNormal ("Foam Normal", Category("Foam"), Intent(NormalMap)) = "normal"
        Float       FoamAlbedo ("Foam Albedo", Range(0,1), Category("Foam")) = 0.85
        Float       FoamTileSize ("Foam Tile Size (cm)", Range(10,10000), Category("Foam")) = 300
        Float       FoamJacobianThreshold ("Whitecap Jacobian Threshold", Range(-1,1), Category("Foam")) = 0.62
        Float       FoamJacobianSoftness ("Whitecap Jacobian Softness", Range(0.01,1), Category("Foam")) = 0.25
        Float       WhitecapGain ("Whitecap Gain", Range(0,4), Category("Foam")) = 1
        Float       FoamDecaySeconds ("Foam Decay (s)", Range(0.1,60), Category("Foam")) = 6
        Float       BubbleDecaySeconds ("Bubble Decay (s)", Range(0.1,60), Category("Foam")) = 3
        Float       WetDrySeconds ("Wet Sand Drying (s)", Range(0.1,600), Category("Shore")) = 40
        Float       CausticsOnsetDepth ("Caustics Onset Depth (cm)", Range(1,1000), Category("Caustics")) = 30
        Float       CausticsFadeDepth ("Caustics Fade Depth (cm)", Range(1,10000), Category("Caustics")) = 600
        Float       CausticsStrength ("Caustics Strength", Range(0,1), Category("Caustics")) = 1
        Float       DebugFoamMask ("Debug: Foam Mask Only", Range(0,1), Category("Debug")) = 0
    }

    ShadingModel SingleLayerWater

    Surface
    {
        // One ripple octave's slope: a normal map turned by `turnDeg` (so no map's own grain lines up with another's),
        // scrolled at its size's deep-water pace. The octave fades as the pixel's footprint nears its smallest waves;
        // the slope variance it stops drawing is returned in `lost`, for the roughness. UV v runs with -Z, as the
        // water mesh's tangent frame (Vertex_WaterMesh.glslh).
        vec2 WaterRippleOctave( BindlessTexture map, vec2 worldXZ, float tileCm, float turnDeg, float weight,
                                float footprintCm, float seconds, inout float lost )
        {
            const float tile = max( tileCm, 1.0 );
            const float r    = radians( turnDeg );
            const mat2  turn = mat2( cos( r ), sin( r ), -sin( r ), cos( r ) );
            // Waves in a map are about 1/16 of its tile; two pixels a wave is the limit of what can be drawn.
            const float drawn  = 1.0 - smoothstep( 0.25, 1.0, footprintCm * 16.0 * 0.5 / tile );
            const float sigma  = Material.RippleSlopeRms * weight;
            lost += sigma * sigma * ( 1.0 - drawn * drawn );
            const float pace = Material.RippleSpeed * sqrt( tile / max( Material.RippleTileSizeA, 1.0 ) );
            const vec2  uv   = turn * vec2( worldXZ.x, -worldXZ.y ) / tile + vec2( pace * seconds / tile, 0.0 );
            const vec3  n    = SurfaceSampleTangentNormal( map, uv );
            return transpose( turn ) * ( n.xy / max( n.z, 0.1 ) ) * ( weight * drawn );
        }

        // Four octaves from tens of centimetres to over ten metres, headings spread about the two directions (UE's water
        // material's detail normals). xyz = the tangent normal, w = the per-axis slope variance left to the roughness.
        vec4 WaterRipples( vec2 worldXZ, float footprintCm, float seconds )
        {
            const float strength = Material.RippleStrength;
            float lost  = 0.0;
            vec2  slope = vec2( 0.0 );
            slope += WaterRippleOctave( Material.RippleNormalA, worldXZ, Material.RippleTileSizeA * 3.1,
                                        Material.RippleAngleA, 0.55 * strength, footprintCm, seconds, lost );
            slope += WaterRippleOctave( Material.RippleNormalB, worldXZ, Material.RippleTileSizeA,
                                        Material.RippleAngleB, 0.5 * strength, footprintCm, seconds, lost );
            slope += WaterRippleOctave( Material.RippleNormalA, worldXZ, Material.RippleTileSizeB,
                                        Material.RippleAngleA + 97.0, 0.2 * strength, footprintCm, seconds, lost );
            slope += WaterRippleOctave( Material.RippleNormalB, worldXZ, Material.RippleTileSizeB * 0.37,
                                        Material.RippleAngleB + 53.0, 0.1 * strength, footprintCm, seconds, lost );
            return vec4( normalize( vec3( slope, 1.0 ) ), lost );
        }

        SurfaceOutput EvaluateSurface( SurfaceInput i )
        {
            SurfaceOutput s = DefaultSurfaceOutput();
#ifdef WATER_MAX_WAVES
            // A river's surface flows along its spline (WaterRiver.glslh WaterFlowPhases: two copies advected
            // half a period apart, each faded out as it resets); still water reads one copy.
            vec2  p0, p1;
            float w1;
            // On a river's fall the sheet's own coordinate (WaterFlowCoord): the ripples stream down it.
            WaterFlowPhases( WaterFlowCoord( i.WorldPosition.xz, i.UV0, i.VertexColor ), i.VertexColor, i.Time, p0, p1, w1 );
            const vec2  ddx       = dFdx( i.WorldPosition.xz );
            const vec2  ddy       = dFdy( i.WorldPosition.xz );
            const float footprint = sqrt( max( dot( ddx, ddx ), dot( ddy, ddy ) ) );
            vec4 ripples = WaterRipples( p0, footprint, i.Time );
            if ( w1 > 0.0 )
            {
                ripples     = mix( ripples, WaterRipples( p1, footprint, i.Time ), w1 );
                ripples.xyz = normalize( ripples.xyz );
            }
#else
            const vec4 ripples = WaterRipples( i.WorldPosition.xz, 0.0, i.Time );
#endif
            s.Normal    = ripples.xyz;
            s.BaseColor = vec3( 0.0 );
            s.Metallic  = 0.0;
            // The slopes too fine to draw widen the lobe: GGX alpha^2 is twice the per-axis slope variance.
            const float alpha = Material.Roughness * Material.Roughness;
            s.Roughness = sqrt( sqrt( alpha * alpha + 2.0 * ripples.w ) );
            s.Specular  = Material.Specular;
            return s;
        }

        // The foam's structure at a still-water (X, Z), at two scales so it does not repeat: xy = the tangent normal's
        // slope, z = the height (the large cells, the small bubbles broken into their walls).
        vec3 WaterFoamSheet( vec2 stillXZ )
        {
            const vec2 uv  = vec2( stillXZ.x, -stillXZ.y ) / max( Material.FoamTileSize, 1.0 );
            const mat2 turn = mat2( 0.8, -0.6, 0.6, 0.8 );
            const vec2 uv2 = turn * uv * 0.37 + vec2( 0.31, 0.17 );
            const vec4 a = SurfaceSampleMaterial( Material.FoamHeight, uv );
            const vec4 b = SurfaceSampleMaterial( Material.FoamHeight, uv2 );
            const vec3 n = SurfaceSampleTangentNormal( Material.FoamNormal, uv );
            return vec3( n.xy / max( n.z, 0.1 ), clamp( 0.5 * ( a.r + b.r ) + 0.3 * ( a.g - 0.5 ), 0.0, 1.0 ) );
        }

        SingleLayerWaterOutput EvaluateSingleLayerWater( SurfaceInput i )
        {
            SingleLayerWaterOutput w;
            w.ScatteringCoefficients = Material.ScatteringCoefficients.rgb;
            w.AbsorptionCoefficients = Material.AbsorptionCoefficients.rgb;
            w.PhaseG                 = Material.PhaseG;
            w.ColorScaleBehindWater  = Material.ColorScaleBehindWater.rgb;
            // The foam rides the water particle (UV1 = its still-water X, Z; Vertex_WaterMesh.glslh), at two scales
            // so the sheet does not repeat.
            // On a river the sheet is carried by the flow, two-phase as the normals (EvaluateSurface).
            vec2  foam0 = i.UV1, foam1 = i.UV1;
            float foamW1 = 0.0;
#ifdef WATER_MAX_WAVES
            // A fall's foam streaks down its sheet (WaterFlowCoord: the sheet's coordinate, stretched along the flow).
            foam0 = WaterFlowCoord( i.UV1, i.UV0, i.VertexColor );
            WaterFlowPhases( foam0, i.VertexColor, i.Time, foam0, foam1, foamW1 );
#endif
            vec3 sheet = WaterFoamSheet( foam0 );
            if ( foamW1 > 0.0 )
                sheet = mix( sheet, WaterFoamSheet( foam1 ), foamW1 );
            w.FoamNormal             = normalize( vec3( sheet.xy, 1.0 ) );
            w.FoamHeight             = sheet.z;
            w.FoamAlbedo             = Material.FoamAlbedo;
            w.DebugFoamMask          = Material.DebugFoamMask;
            w.WindRippleMaxGain      = Material.WindRippleMaxGain;
            return w;
        }
    }
}
