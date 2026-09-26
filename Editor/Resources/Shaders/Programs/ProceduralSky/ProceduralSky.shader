// DesertAsset {"Kind":"Shader","Guid":"e94b07612049b42f3e565e49d2b0ca1f","Versions":{"SHDR":1},"Dependencies":[]}
Shader "ProceduralSky"
{
    Fragment
    {
        // The engine-generated sky. Two models behind one switch (the payload's SkyModel lane):
        //
        //   * ArtisticGradient — the authored palette evaluated from the view ray and the sun
        //     direction (Common/Atmosphere.glslh's EvaluateSky), bit-for-bit the sky this pass has
        //     always drawn;
        //   * PhysicalAtmosphere — the Hillaire 2020 model: the per-view Sky-View LUT sampled through
        //     its horizon-warped mapping, plus the analytic sun disc (outer-space luminance times the
        //     transmittance LUT, limb-darkened), plus the lit ground below the horizon.
        //
        // Output is LINEAR HDR — the post-process tonemap pass applies exposure/gamma downstream.
        // The models live in the shared headers and are shared with the IBL bake
        // (Compute/BakeProceduralSky) so the visible sky and the light it casts are identical.

        #include <Common/Atmosphere.glslh>
        #include <Common/SkyMedium.glslh>
        #include <Common/SkyScattering.glslh>

        In(0) vec3 v_RayDir;
        // The camera position, forwarded from the vertex stage the way every mesh shader forwards it —
        // this engine keeps CameraUB a vertex-stage block. Constant across the quad, so interpolation
        // returns it exactly.
        In(1) vec3 v_CameraPos;
        Out(0) vec4 oColor;

        // The sky parameter block. A std430 STORAGE buffer rather than a uniform block, because the
        // atmosphere LUT passes evaluate the same sky from a compute shader and ComputePipeline has no
        // SetUniformBuffer — its binding surface is inputs, outputs, storage buffers and push constants.
        // One buffer, one layout, every consumer.
        //
        // The binding is written explicitly as (1) and must stay equal to Graphic::kSkyPayloadBinding: the
        // graphics descriptor write uses the buffer's OWN binding number while a compute dispatch passes
        // one as an argument, and when those disagree the buffer quietly lands on another resource's slot.
        ReadBuffer(1) SkyBuffer
        {
            vec4 u_SkyPacked[SKY_PACKED_VEC4_COUNT];
        };

        // PhysicalAtmosphere only. When the gradient renders, both stay on their fallback descriptors
        // and the branch below never samples them.
        Uniform(2) sampler2D u_TransmittanceLut; // cached 256x64 (SkyboxRenderer)
        Uniform(3) sampler2D u_SkyViewLut;       // per-view 192x104, refilled every frame

        // Luminance ceiling of the physical path's ON-SCREEN pixels. The disc's true outer-space
        // luminance (illuminance over its half-degree solid angle) is ~10^4 x the tonemapper's white
        // point; unclamped it saturates every bloom box it touches (the Karis average only suppresses
        // ISOLATED fireflies) and the frame gains a 25-degree white blob for a halo. 1000 keeps the
        // disc two orders over white — still the brightest thing in any frame, still driving a
        // visible glow — while the bloom chain stays proportionate. Raising this belongs to the
        // deferred sun-perceptual-stack task (auto-exposure + lens flare tuned against the disc),
        // not to the sky. The IBL bake is untouched: it excludes the disc entirely.
        const float kSkyLuminanceClamp = 1000.0f;

        vec3 EvaluatePhysicalSky(SkyPacked s, vec3 dir)
        {
            SkyAtmParams atm = SkyMakeAtmParams(UnpackMediumRayleigh(s), UnpackMediumMie(s),
                                                UnpackMediumMieAbsorption(s), UnpackMediumOzone(s),
                                                UnpackMediumGround(s), UnpackMediumTentPlanet(s));

            // The same camera-to-(r, zenith) reduction the Sky-View fill ran this frame — the mapping
            // only agrees with the texels if both sides start from the same viewHeight.
            vec3  cameraKm   = v_CameraPos / SKY_WORLD_UNITS_PER_KM;
            float viewHeight = clamp(SkyViewHeightKm(cameraKm, atm.BottomRadiusKm),
                                     atm.BottomRadiusKm + SKY_PLANET_RADIUS_OFFSET_KM,
                                     atm.TopRadiusKm - 0.01f);
            vec3  zenith     = SkyViewZenith(cameraKm, atm.BottomRadiusKm);

            vec3  sunDir        = UnpackSunDirection(s);
            float viewZenithCos = clamp(dot(zenith, dir), -1.0f, 1.0f);
            float lightViewCos  = SkyViewLightViewCos(zenith, dir, sunDir);

            vec2 unit = SkyViewUnitFromParams(atm.BottomRadiusKm, viewHeight, viewZenithCos, lightViewCos);
            vec2 uv   = vec2(SkyUnitToTexelUv(unit.x, SKY_VIEW_LUT_WIDTH),
                             SkyUnitToTexelUv(unit.y, SKY_VIEW_LUT_HEIGHT));

            // rgb = in-scattered luminance (the SkyAndAP tint is already inside), a = the view path's
            // mean transmittance.
            vec4 lut = texture(u_SkyViewLut, uv);
            vec3 sky = lut.rgb;

            vec3 sunIlluminance = UnpackSkyConfig(s).sunColor * UnpackSunIntensity(s);

            // BELOW THE HORIZON THIS PASS DRAWS ONLY AIR — never a lit ground, which is what it used to
            // do and what made the lower half of an empty scene a flat beige plate that no scene setting
            // could remove. `sky` already holds the in-scattering marched along the ray up to where it
            // meets the planet, and that is the whole of the sky's contribution downward.
            //
            // This is Unreal's behaviour and it is checkable in one action: switch off the
            // ExponentialHeightFog in a UE scene with no geometry and the ground goes BLACK. The colour
            // that is normally there belongs to the fog, not to the sky — the sky atmosphere never
            // shades the planet in the main pass. `GroundAlbedo` still matters, but through the
            // multiple-scattering LUT, where it brightens the sky near the horizon by light that bounced
            // off the ground and came back up. That is the ONLY route it should take to the screen, and
            // the tooltip on the field has always said so.
            //
            // The ground bounce stays where it is genuinely light rather than a picture: the IBL bake
            // (Compute/BakeProceduralSky.shader) and the distant sky light the fog reads
            // (SkyScattering.glslh, SkyDistantLightRadiance). A scene lit from below by ground bounce is
            // correct; a scene that DRAWS that bounce as a floor is not, and only the second one is a
            // divergence.
            if (!SkyViewHitsGround(atm.BottomRadiusKm, viewHeight, viewZenithCos))
            {
                // The analytic sun disc: outer-space luminance (illuminance over the disc's solid
                // angle) times the transmittance toward the view, limb-darkened, soft-edged. Not in
                // the LUT — 192x104 texels would alias a half-degree disc into a smear.
                float cosApex = cos(UnpackSunAngularRadius(s));
                float vDotSun = dot(dir, sunDir);
                float edge    = SkySunDiscSoftEdge(vDotSun, cosApex);
                if (edge > 0.0f)
                {
                    float apex         = max(UnpackSunAngularRadius(s), 1e-4f);
                    float centerToEdge = clamp(acos(clamp(vDotSun, -1.0f, 1.0f)) / apex, 0.0f, 1.0f);
                    float solidAngle   = 2.0f * SKY_PI * (1.0f - cosApex);

                    vec3 viewT = texture(u_TransmittanceLut,
                                         SkyTransmittanceLutUvFromParams(atm.BottomRadiusKm,
                                                                         atm.TopRadiusKm, viewHeight,
                                                                         viewZenithCos)).rgb;

                    sky += sunIlluminance / max(solidAngle, 1e-7f) * viewT *
                           SkySunLimbDarkening(centerToEdge) * edge;
                }
            }

            // The on-screen-only art tint (UE's SkyLuminanceFactor), then the fp16 headroom clamp.
            return min(sky * UnpackSkyLuminanceFactor(s), vec3(kSkyLuminanceClamp));
        }

        void main()
        {
            SkyPacked s;
            for (int i = 0; i < SKY_PACKED_VEC4_COUNT; ++i)
                s.v[i] = u_SkyPacked[i];

            vec3 dir = normalize(v_RayDir);

            vec3 color;
            if (UnpackSkyModelIsPhysical(s))
                color = EvaluatePhysicalSky(s, dir);
            else
                color = EvaluateSky(dir, UnpackSunDirection(s), UnpackSunIntensity(s),
                                    UnpackSunAngularRadius(s), UnpackSkyConfig(s));

            // (Dithering is done at the FINAL 8-bit output in the composite/tonemap pass — doing it here in linear
            // HDR is lost through Reinhard+gamma. See SceneComposite.glsl.frag.)
            oColor = vec4(color, 1.0);
        }
    }

    Vertex
    {
        #include <Common/QuadPositions.glslh>
        #include <Common/CameraUB.glslh>

        // World-space view ray for this fullscreen pixel (un-normalized; normalized in the fragment).
        Out(0) vec3 v_RayDir;
        // Camera position for the physical model's altitude (constant across the quad).
        Out(1) vec3 v_CameraPos;

        void main()
        {
            vec4 position = vec4(QUAD_POSITIONS[gl_VertexIndex], 1.0, 1.0);
            gl_Position = position;

            // DIRECTION-ONLY world-space view ray (camera rotation only — NO far-plane-worldPos minus cameraPos).
            // That subtraction of two large world-space points loses float precision and, as the camera MOVES, makes
            // the ray direction jitter frame-to-frame → the tiny sun/stars "boil". Unproject to VIEW space, then
            // rotate to world with mat3(invView). Correct for this sky, which depends only on direction.
            vec4 viewH   = inverse(cameraUB.Projection) * position;
            vec3 viewRay = viewH.xyz / viewH.w;
            v_RayDir     = mat3(inverse(cameraUB.View)) * viewRay;
            v_CameraPos  = cameraUB.CameraPos;
        }
    }
}
