// DesertAsset {"Kind":"Shader","Guid":"9c3e51d27a8f4b6e8d0f2a7c4e9b1d63","Versions":{"SHDR":1},"Dependencies":[]}
Shader "TSRRecordPersistentFrame"
{
    Compute
    {
        // TSR.RecordPersistentFrame (r.TSR.Resurrection). UE 5.8 TemporalSuperResolution.cpp keeps every TSR frame
        // in one Texture2DArray whose slices roll (FTSRHistorySliceSequence :1561-1665: two transient slices and
        // r.TSR.Resurrection.PersistentFrameCount persistent ones, one recorded every
        // r.TSR.Resurrection.PersistentFrameInterval frames). Adapted: the colour and guide histories stay the
        // engine's swapped pairs, and on a recording frame this pass copies the frame just written into one of the
        // persistent textures (HistoryLifetime::Persistent), so no pass ever reads and writes one array. The guide
        // is stored LINEAR (its guide colour space divided by this frame's exposure: the scene colour at this frame's
        // pre-exposure, TsrPersistentFrame::PreExposure), so TSR.DecimateHistory corrects it by
        // ResurrectionPreExposureCorrection and re-exposes it with the resurrecting frame's exposure.
        //
        // TSR_RECORD_GUIDE: 0 copies the colour history (HistoryExtent, linear already), 1 the guide (RenderExtent).
        #ifndef TSR_RECORD_GUIDE
        #define TSR_RECORD_GUIDE 0
        #endif

        LocalSize(8, 8, 1);

        #include <Common/TSRCommon.glslh>

        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform(4) sampler u_PointClampSampler;
        Uniform(1) texture2D u_Exposure; // 1x1 previous adapted luminance (the guide's exposure this frame)
        Uniform(2) texture2D u_Source;   // this frame's TSR.History.Color / TSR.History.Guide
        #define u_Source sampler2D(u_Source, u_PointClampSampler)
        layout(binding = 3, rgba16f) restrict writeonly uniform image2D u_Destination;

        void main()
        {
            const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
        #if TSR_RECORD_GUIDE
            if (any(greaterThanEqual(pixel, ivec2(u_RenderSize))))
                return;
            const vec4 guide = texelFetch(u_Source, pixel, 0);
            imageStore(u_Destination, pixel, vec4(GCSToLinear(guide.rgb) / TsrExposure(u_Exposure, u_PointClampSampler), guide.a));
        #else
            if (any(greaterThanEqual(pixel, ivec2(u_HistorySize))))
                return;
            imageStore(u_Destination, pixel, texelFetch(u_Source, pixel, 0));
        #endif
        }
    }
}
