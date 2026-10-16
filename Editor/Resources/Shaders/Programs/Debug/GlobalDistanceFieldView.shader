// DesertAsset {"Kind":"Shader","Guid":"a7a7fbc78b784f33b3de5c93b70f477d","Versions":{"SHDR":1},"Dependencies":[]}
// The Global Distance Field view mode (DeferredDebugMode::GlobalDistanceField, UE "Global DistanceField"
// visualization, DistanceFieldVisualization.usf): every pixel sphere-traces the clipmaps with the shared
// TraceGlobalDistanceField and shows the hit lit by its gradient, tinted by the clipmap that answered, with the
// march's step count heat-mapped where it ran long - so both the shape the field holds and its cost are visible.
Shader "GlobalDistanceFieldView"
{
    Vertex
    {
        #include <Common/FullscreenTriangle.glslh>
        Out(0) vec2 v_Ndc;
        void main()
        {
        	v_Ndc       = FullscreenTriangleNdc();
        	gl_Position = vec4(FullscreenTriangleNdc(), 0.0, 1.0);
        }
    }
    Fragment
    {
        In(0) vec2 v_Ndc;
        Uniform(0) sampler3D u_GlobalDistanceField0;
        Uniform(1) sampler3D u_GlobalDistanceField1;
        Uniform(2) sampler3D u_GlobalDistanceField2;
        Uniform(3) sampler3D u_GlobalDistanceField3;
        Out(0) vec4 oColor;

        PushConstant GDFViewPush
        {
            mat4 u_InverseViewProjection;
            vec4 u_Levels[4]; // (centre.xyz, voxel size), centimetres
        };

        #include <Common/GlobalDistanceField.glslh>

        vec3 Unproject(vec2 ndc, float depth)
        {
            const vec4 h = u_InverseViewProjection * vec4(ndc, depth, 1.0);
            return h.xyz / h.w;
        }

        void main()
        {
            // Reversed-Z (ViewRay.glslh): depth 1 is the near plane; 0.5 is a finite point farther along the ray.
            const vec3 origin    = Unproject(v_Ndc, 1.0);
            const vec3 direction = normalize(Unproject(v_Ndc, 0.5) - origin);
            vec4 levels[GDF_NUM_CLIPMAPS];
            for (int i = 0; i < GDF_NUM_CLIPMAPS; ++i)
                levels[i] = u_Levels[i];
            const float maxT = levels[GDF_NUM_CLIPMAPS - 1].w * float(GDF_RESOLUTION);
            // The field as a shadow ray sees it: foliage stops the view too (no dithered transparency).
            const GlobalDistanceFieldHit hit = TraceGlobalDistanceField(origin, direction, maxT, 256, levels, false, 0u);

            const float heat = clamp(float(hit.Steps) / 128.0, 0.0, 1.0);
            vec3 color = mix(vec3(0.02, 0.03, 0.06), vec3(0.35, 0.05, 0.05), heat);
            if (hit.Hit)
            {
                const vec3  p      = origin + direction * hit.T;
                const vec3  normal = normalize(GlobalDistanceFieldGradient(hit.Level, p, levels[hit.Level].w) + vec3(1e-6));
                const vec3  light  = normalize(vec3(0.4, 0.8, 0.45));
                const float lambert = 0.25 + 0.75 * max(dot(normal, light), 0.0);
                const vec3  tints[4] = vec3[4](vec3(1.0), vec3(0.75, 0.9, 1.0), vec3(0.8, 1.0, 0.75), vec3(1.0, 0.85, 0.7));
                color = mix(tints[hit.Level] * lambert, vec3(1.0, 0.2, 0.1), heat * 0.5);
            }
            oColor = vec4(color, 1.0);
        }
    }
}
