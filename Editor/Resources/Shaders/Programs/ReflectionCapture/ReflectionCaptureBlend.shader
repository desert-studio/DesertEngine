// DesertAsset {"Kind":"Shader","Guid":"62996020502d47c289a4120248c9892a","Versions":{"SHDR":1},"Dependencies":[]}
Shader "ReflectionCaptureBlend"
{
    Compute
    {
        // A RECAPTURE FADES IN (REFL-NORM-SMOOTH): the new cube is written into the array's staging cube, and the
        // live cube the lighting reads moves toward it over ReflectionCaptureRenderer's kCaptureBlendFrames, one
        // step per frame, every mip (the filter is linear, so a blend of filtered mips is the filtered blend).
        // Step k of K writes live = mix(live, staging, 1 / (K - k)): linear in time from the old cube to the new,
        // and exactly the new one at the last step - one dispatch per mip, no copy of the old cube kept.
        //
        // THE STORED DIFFUSE (alpha, measured in the last mip by ReflectionCaptureDiffuse) blends with its
        // radiance while both are measured. A value only one side has is never mixed with 0: the ratio divides by
        // it, and a stored diffuse sliding toward 0 is a reflection blowing up toward the 10000x cap. The new
        // cube's own measure is taken as soon as it exists; an unmeasured new cube keeps the old measure until
        // the last step, then 0 ("not measured", re-measured by the Diffuse pass as any other).

        layout(binding = 0, rgba16f) uniform imageCubeArray u_Cubes; // this mip, every layer

        PushConstant PushConstants
        {
        	uint  u_LiveLayer;    // the live cube's first layer (cube * 6)
        	uint  u_StagingLayer; // the staging cube's first layer
        	float u_Weight;       // 1 / (K - k): the share of the staging cube this step
        	float u_Pad;
        };

        LocalSize(8, 8, 1);
        void main()
        {
        	const ivec2 size = imageSize(u_Cubes).xy;
        	if (gl_GlobalInvocationID.x >= uint(size.x) || gl_GlobalInvocationID.y >= uint(size.y))
        		return;
        	const ivec2 xy      = ivec2(gl_GlobalInvocationID.xy);
        	const int   face    = int(gl_GlobalInvocationID.z);
        	const ivec3 live    = ivec3(xy, int(u_LiveLayer) + face);
        	const vec4  was     = imageLoad(u_Cubes, live);
        	const vec4  now     = imageLoad(u_Cubes, ivec3(xy, int(u_StagingLayer) + face));
        	const bool  last    = u_Weight >= 1.0;
        	float       stored;
        	if (was.a > 0.0 && now.a > 0.0)
        		stored = mix(was.a, now.a, u_Weight);
        	else if (now.a > 0.0)
        		stored = now.a;
        	else
        		stored = last ? 0.0 : was.a;
        	imageStore(u_Cubes, live, vec4(mix(was.rgb, now.rgb, u_Weight), stored));
        }
    }
}
