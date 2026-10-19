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
        // is stored LINEAR (its guide colour space divided by this frame's exposure), so TSR.DecimateHistory
        // re-exposes it with the resurrecting frame's exposure (UE: ResurrectionPreExposureCorrection).
        //
        // TSR_RECORD_GUIDE: 0 copies the colour history (HistoryExtent, linear already), 1 the guide (RenderExtent).
        #ifndef TSR_RECORD_GUIDE
        #define TSR_RECORD_GUIDE 0
        #endif

        LocalSize(8, 8, 1);

        #include <Common/TSRCommon.glslh>

        Uniform(1) sampler2D u_Exposure; // 1x1 previous adapted luminance (the guide's exposure this frame)
        Uniform(2) sampler2D u_Source;   // this frame's TSR.History.Color / TSR.History.Guide
        layout(binding = 3, rgba16f) restrict writeonly uniform image2D u_Destination;

        void main()
        {
            const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
        #if TSR_RECORD_GUIDE
            if (any(greaterThanEqual(pixel, ivec2(u_RenderSize))))
                return;
            const vec4 guide = texelFetch(u_Source, pixel, 0);
            imageStore(u_Destination, pixel, vec4(GCSToLinear(guide.rgb) / TsrExposure(u_Exposure), guide.a));
        #else
            if (any(greaterThanEqual(pixel, ivec2(u_HistorySize))))
                return;
            imageStore(u_Destination, pixel, texelFetch(u_Source, pixel, 0));
        #endif
        }
    }
}
