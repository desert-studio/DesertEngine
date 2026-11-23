// DesertAsset {"Kind":"Shader","Guid":"573862fc3c794aa4a288362075ae2f5f","Versions":{"SHDR":1},"Dependencies":[]}
Shader "WaterFFTSpectrum"
{
    Compute
    {
        // THE OCEAN AT THIS INSTANT, IN WAVE-NUMBER SPACE (Tessendorf 2001 eq. 26, 29-30, 36): every cascade's
        // initial spectrum h0 (Engine/Water/FFTOceanSpectrum.cpp, uploaded once per wave set as a 256 x 768 atlas,
        // cascade c in rows [256 c, 256 c + 256)) advanced to the view's time by the deep-water dispersion
        // omega = sqrt(g |k|), and the seven spectra the surface needs derived from it: the choppy displacement
        // (Dx, Dz), the height (Dy), the height's slopes and the choppy map's three derivatives (the Jacobian).
        // Two real fields per complex texel (WaterFFTPack), so one inverse FFT of two rgba32f arrays yields all eight.
        // One invocation per texel per cascade (Graphic::System::WaterRenderer::AddWavePasses).
        #extension GL_EXT_samplerless_texture_functions : require
        #include <Water/WaterFFT.glslh>

        layout(binding = 0) uniform texture2D WaterFFTInitialSpectrum;
        // xy = Dx + i Dy, zw = Dz + i dDx/dz.
        layout(binding = 1, rgba32f) restrict writeonly uniform image2DArray WaterFFTSpectrumA;
        // xy = dDy/dx + i dDy/dz, zw = dDx/dx + i dDz/dz.
        layout(binding = 2, rgba32f) restrict writeonly uniform image2DArray WaterFFTSpectrumB;

        Buffer(3) WaterFFTParamsBuffer
        {
            WaterFFTParams Params;
        };

        LocalSize(8, 8, 1);

        void main()
        {
            const ivec3 id = ivec3(gl_GlobalInvocationID);
            if (id.x >= WATER_FFT_SIZE || id.y >= WATER_FFT_SIZE || id.z >= WATER_FFT_CASCADES)
                return;

            const vec4  h0    = texelFetch(WaterFFTInitialSpectrum, ivec2(id.x, id.y + id.z * WATER_FFT_SIZE), 0);
            const float patchSize = id.z == 0 ? Params.Cascade.x : (id.z == 1 ? Params.Cascade.y : Params.Cascade.z);
            const vec2  k     = (vec2(id.xy) - float(WATER_FFT_SIZE / 2)) * (WATER_FFT_TWO_PI / patchSize);
            const float kl    = length(k);

            vec4 a = vec4(0.0);
            vec4 b = vec4(0.0);
            if (kl > 1e-8)
            {
                // h(k, t) = h0(k) e^(i w t) + conj(h0(-k)) e^(-i w t): Hermitian in k, so the field is real.
                const float omega = sqrt(WATER_FFT_GRAVITY * kl);
                const float phase = omega * Params.Cascade.w;
                const vec2  e     = vec2(cos(phase), sin(phase));
                const vec2  h     = WaterFFTMul(h0.xy, e) + WaterFFTMul(h0.zw, vec2(e.x, -e.y));
                const vec2  n     = k / kl;

                const vec2 ih     = vec2(-h.y, h.x);  // i h
                const vec2 dx     = vec2(h.y, -h.x) * n.x; // -i (kx / |k|) h
                const vec2 dz     = vec2(h.y, -h.x) * n.y; // -i (kz / |k|) h
                const vec2 dydx   = ih * k.x;            // i kx h
                const vec2 dydz   = ih * k.y;            // i kz h
                const vec2 dxdx   = h * (k.x * n.x);     // kx^2 / |k| h
                const vec2 dzdz   = h * (k.y * n.y);     // kz^2 / |k| h
                const vec2 dxdz   = h * (k.x * n.y);     // kx kz / |k| h

                a = vec4(WaterFFTPack(dx, h), WaterFFTPack(dz, dxdz));
                b = vec4(WaterFFTPack(dydx, dydz), WaterFFTPack(dxdx, dzdz));
            }
            imageStore(WaterFFTSpectrumA, id, a);
            imageStore(WaterFFTSpectrumB, id, b);
        }
    }
}
