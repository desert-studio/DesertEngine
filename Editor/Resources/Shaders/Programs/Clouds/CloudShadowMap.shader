// DesertAsset {"Kind":"Shader","Guid":"7dd4e86463c76a0cbeb1504f63caf9e2","Versions":{"SHDR":1},"Dependencies":[]}
Shader "CloudShadowMap"
{
    Compute
    {
        // The cloud shadow map's producer: one orthographic march down the sun's own direction, one
        // texel per ray, writing the triple `(frontDepthKm, meanExtinctionPerKm, maxOpticalDepth)` that
        // Programs/Deferred/DeferredLighting.shader turns back into a transmittance for a receiver at any
        // depth along that ray. Both halves of that relation live in Common/CloudShadowMap.glslh, which
        // is where the encoding is explained and which both shaders compile.
        //
        // WHY THIS IS NOT A DEPTH MAP. Rendering the clouds into a depth buffer from the sun and
        // comparing against it is the cheaper-looking thing to write, and it answers the wrong question:
        // a medium has no surface, so "is the receiver behind the front of the cloud" is a yes/no about a
        // boundary that does not exist, and it is simply wrong for every receiver that is inside the
        // layer or above its base — which includes the clouds themselves, an aircraft, a mountain and any
        // camera that has climbed. The triple costs the same fetch and is correct for all of them.
        //
        // WHERE IT RUNS. In-frame compute, outside any render pass, EARLY —
        // SceneRenderer::ExecuteCloudShadowMap() is called before the render graph records, because the
        // deferred lighting pass that reads this map runs before the cloud march does. It needs no scene
        // depth and no atmosphere LUT, only the field and the sun, so nothing forces it later.
        //
        // WHAT IT SAMPLES, AND WHAT THAT COSTS. The same field the view march samples, through the same
        // three resources bound at the same three slots: the parameter block, the noise volume and the
        // vertical profile table. `resolution` squared rays, each of `CloudShadowSampleCount` samples,
        // every frame — which is why the resolution is the quality tier's lever on this pass and why the
        // tier scales the extent with it rather than the resolution alone (Graphic::CloudQualityScale).
        // That is the entire price of this feature and it is stated in Docs/Clouds/CALIBRATION.md beside
        // the measurement.

        #include <Common/CloudNoise.glslh>
        #include <Common/CloudGeometry.glslh>

        // rgba32f AND NOT rgba16f, and the difference is not fastidiousness. The front depth is stored in
        // kilometres along a ray whose far plane is four times the map's extent — 120 km — and half
        // precision quantizes 120 to steps of 0.0625 km. The reconstruction multiplies that error by the
        // mean extinction, which at the component's shipped 8 per kilometre turns a 62 m quantization into
        // an optical depth of 0.5 and a transmittance wrong by forty per cent. The fourth channel is
        // unwritten because Core::Formats::ImageFormat has no three-channel float format; that is a fact
        // about the engine's formats and not a reserved slot.
        layout(binding = 0, rgba32f) restrict writeonly uniform image2D u_CloudShadowMap;

        // The noise volume and the procedural modelling volume, at the march's own binding numbers (see
        // Graphic::kCloudShadowNoiseBinding). One vocabulary for one field.
        Uniform(3) sampler3D u_CloudNoise;
        // ALL FOUR OF THE MARCH'S NOISE SLOTS, at the march's own numbers. A cloud shades the ground with
        // the edge it actually has: a cirrus eroded by the fine volume for the eye and by the default one
        // for the shadow map would be two different clouds in one frame, and the disagreement would show
        // as a shadow that does not fit its cloud.
        Uniform(10) sampler3D u_CloudNoise1;
        Uniform(11) sampler3D u_CloudNoise2;
        Uniform(12) sampler3D u_CloudNoise3;
        Uniform(7) sampler3D u_CloudModelling;

        // The same four-way select the march declares, for the same reason and at the same cost. Repeated
        // rather than shared because Common/CloudField.glslh must stay free of samplers to remain
        // compilable as C++ by its tests — which is the same reason the three callbacks below are declared
        // in each shader rather than in the header they feed.
        vec4 CloudFetchNoise( int slot, vec3 p )
        {
            if ( slot == 1 )
                return texture( u_CloudNoise1, p );
            if ( slot == 2 )
                return texture( u_CloudNoise2, p );
            if ( slot == 3 )
                return texture( u_CloudNoise3, p );
            return texture( u_CloudNoise, p );
        }

        // The sculpted hero-cloud body, at the march's own slot too — one vocabulary for one field, and
        // the reason the shadow map needs it at all is that a hero cloud is a cloud: it shades the ground
        // under it because it is the same field, not because anything was added to the deferred pass.
        // ALWAYS BOUND, fallback included, on the terms the two above are bound on.
        Uniform(9) sampler3D u_CloudAuthoredAtlas;

        // The seam's three callbacks, exactly as CloudRaymarch.shader declares them: Common/CloudField.glslh
        // must stay free of samplers to remain compilable as C++ by its tests.
        #define CLOUD_SAMPLE_NOISE(s, p) CloudFetchNoise((s), (p))
        #define CLOUD_SAMPLE_MODELLING(p) textureLod(u_CloudModelling, (p), 0.0f)
        #define CLOUD_SAMPLE_AUTHORED(p) textureLod(u_CloudAuthoredAtlas, (p), 0.0f)

        #define CLOUD_AUTHORED_BUFFER_BINDING 8
        #include <Common/CloudAuthored.glslh>

        #include <Common/CloudField.glslh>
        #include <Common/CloudParams.glslh>

        PushConstant CloudShadowPush
        {
            // The map's clip space back to world units. A texel's ray starts on the near plane at
            // (clip.xy, 0) and runs along u_CloudShadowTrace.xyz.
            mat4 u_CloudShadowMapToWorld;
            // xyz = the direction the light TRAVELS, normalized; w = samples this ray takes.
            vec4 u_CloudShadowTrace;
            // x = the kilometres the map's clip z spans. PUSHED rather than compiled from
            // CLOUD_SHADOWMAP_EXTENT_KM, because the quality tier scales the extent and this shader would
            // otherwise encode every front depth against a far plane the projection it was handed does
            // not have. y/z/w are unwritten — a push constant is laid out in vec4s.
            vec4 u_CloudShadowDepth;
        };

        LocalSize(8, 8, 1);

        // The shell and the field, resolved once in main() and read by the extinction callback below.
        // Globals rather than parameters because the callback's signature is fixed by
        // CLOUD_SHADOW_SAMPLE_EXTINCTION, which takes a position and nothing else — a compute shader's
        // globals are per-invocation, so this is a local by another name.
        CloudLayer       g_ShadowLayer;
        CloudFieldParams g_ShadowField;

        // The medium's extinction, per kilometre, at a planet-centre-relative position in kilometres.
        //
        // IDENTICAL IN FORM TO THE VIEW MARCH'S OWN sigmaT, and it has to be: `density * layer extinction
        // * the winning species' factor`. A shadow map that measured a different quantity from the march
        // would put a cloud's shadow on the ground at a density the cloud itself does not have, and the
        // two would part company as soon as anybody retuned one of them. Desert/Tests/Engine/CloudShadow
        // asserts the agreement on the reconstruction rather than on the formula, which is the stronger
        // statement.
        float CloudShadowExtinctionAt(vec3 posKm)
        {
            float radiusKm = length(posKm);
            if (radiusKm > g_ShadowLayer.TopRadiusKm || radiusKm < g_ShadowLayer.BottomRadiusKm)
                return 0.0f;

            float heightFraction = CloudHeightFraction(g_ShadowLayer, posKm);

            // The ALTITUDE above the layer's base, never the planet-relative height — the same conversion
            // the view march makes, and for the same two reasons: the noise is periodic, so a y of 6363 km
            // wraps to something arbitrary, and float32 resolves 0.4 m at that magnitude.
            vec3 fieldPos = vec3(posKm.x, radiusKm - g_ShadowLayer.BottomRadiusKm, posKm.z);

            CloudFieldSample field = SampleCloudField(g_ShadowField, heightFraction, fieldPos);
            if (field.Profile <= 0.0f)
                return 0.0f;

            float density = CloudSampleDensity(g_ShadowField, field, fieldPos);
            return density * max(u_CloudMarch.w, 0.0f) *
                   CloudSampleExtinctionFactor(g_ShadowField, field, fieldPos);
        }

        #define CLOUD_SHADOW_SAMPLE_EXTINCTION(p) CloudShadowExtinctionAt(p)

        #include <Common/CloudShadowMap.glslh>

        void main()
        {
            ivec2 size  = imageSize(u_CloudShadowMap);
            ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
            if (coord.x >= size.x || coord.y >= size.y)
                return;

            float farDepthKm = max(u_CloudShadowDepth.x, 1e-3f);

            // The texel's ray origin: its own centre on the map's NEAR plane. Through the texel centre and
            // not its corner, because the consumer fetches this map bilinearly and a half-texel
            // disagreement between where a value was traced and where it is read is a shadow offset by
            // half a texel in both axes — 59 m on the ground, which is visible as a shadow that does not
            // sit under its cloud.
            vec2 uv   = (vec2(coord) + vec2(0.5f, 0.5f)) / vec2(size);
            vec2 clip = uv * 2.0f - vec2(1.0f, 1.0f);

            vec4 nearH  = u_CloudShadowMapToWorld * vec4(clip.x, clip.y, 0.0f, 1.0f);
            vec3 nearP  = nearH.xyz / max(nearH.w, 1e-9f);
            vec3 lightDir = normalize(u_CloudShadowTrace.xyz);

            g_ShadowLayer = CloudUnpackLayer();
            g_ShadowField = CloudUnpackFieldParams();

            // THIS WHOLE PASS IS A SHADOW RAY. Every sample it takes is integrated into optical depth and
            // never into radiance, so an authored medium may answer it cheaply — the same permission Epic
            // gives through ShadowSampleDistance. Declared once, here, because the flag rides on the
            // params the callback below already reads out of this global.
            g_ShadowField.ShadowRay = CLOUD_RAY_SHADOW;

            // Planet-centre-relative kilometres, the frame both shells are centred on the origin in —
            // the same change of frame the view march makes, and the same one
            // Graphic::CloudBuildShadowMapView placed the map's anchor in.
            vec3 nearKm   = nearP * (1.0f / CLOUD_WORLD_UNITS_PER_KM);
            vec3 originKm = vec3(nearKm.x, nearKm.y + g_ShadowLayer.PlanetRadiusKm, nearKm.z);

            // The SAME shell intersection the view march uses, including its planet test — a ray aimed
            // down from above the layer meets the ground long before the far side of the shell, and
            // without that test the far side is what a grazing texel at the map's corner would march.
            vec2 segment = CloudLayerIntersect(g_ShadowLayer, originKm, lightDir);

            int  sampleCount = int(clamp(u_CloudShadowTrace.w, 1.0f, 128.0f));
            CloudShadowTexel texel =
                CloudShadowTraceRay(originKm, lightDir, segment.x, segment.y, sampleCount, farDepthKm);

            imageStore(u_CloudShadowMap, coord, vec4(CloudShadowEncode(texel), 0.0f));
        }
    }
}
