// DesertAsset {"Kind":"Shader","Guid":"9340bb3924284ea3b884773a953723c4","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SkyCaptureIrradianceReadback"
{
    Compute
    {
        // THE SKY CAPTURE'S HORIZONTAL ILLUMINANCE, FOR THE CPU (Systems/Scene/Skybox/SkyCapture.cpp): what the
        // captured sky light delivers to an unoccluded upward-facing surface -- UE's SkyLight irradiance, the
        // number a physical sky is checked against (a clear sky gives a horizontal surface roughly 10-20 % of a
        // high sun's illuminance). The irradiance cube stores E / pi (SkyCaptureIrradiance), so E is pi times the
        // texel at the zenith. A separate one-thread pass rather than a lane of SkyCaptureIrradiance, because that
        // program is the .hdr path's too (EnvironmentManager), which has no readback to bind.
        //
        // Written into a per-frame host-readable copy and read FramesInFlight frames later
        // (SkyCapture::ReadBackZenithIlluminance); w = 1 tells a written copy from a never-written one.

        const float PI = 3.141592653589793;

        Uniform sampler LinearClampSampler;
        Uniform(0) textureCube IrradianceTexture;
        #define IrradianceTexture samplerCube(IrradianceTexture, LinearClampSampler)
        Buffer(3) IrradianceReadback
        {
            vec4 ZenithIlluminance; // rgb = E about +Y (lux), w = 1 once written
        };

        LocalSize(1, 1, 1);
        void main()
        {
        	ZenithIlluminance = vec4(textureLod(IrradianceTexture, vec3(0.0, 1.0, 0.0), 0.0).rgb * PI, 1.0);
        }
    }
}
