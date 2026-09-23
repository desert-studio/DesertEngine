Shader "PanoramaToCubemap"
{
    Compute
    {
        const float PI = 3.141592;
        const float TWOPI = 2 * PI;

        Uniform(0, 0) sampler2D inputTexture;
        layout(set=0, binding=1, rgba32f) restrict writeonly uniform imageCube outputTexture;

        // THE AUTHORED LOOK OF AN HDR SKY, and the reason it is a push constant rather than a knob in the
        // frame: this program runs once per bake, not once per frame, and the values are constant across
        // the whole dispatch. Identity ((1,0) and white) is what the procedural sky's own bake passes, so
        // that path is byte-for-byte what it was.
        PushConstant SkyLook
        {
            vec4 YawCosSin; // xy = (cos yaw, sin yaw); zw unused
            vec4 Gain;      // rgb = tint * intensity; a unused
        } skyLook;

        #include <Common/SkyPanorama.glslh>

        vec3 getSamplingVector()
        {
            vec2 st = gl_GlobalInvocationID.xy/vec2(imageSize(outputTexture));
            vec2 uv = 2.0 * vec2(st.x, 1.0-st.y) - vec2(1.0);

            vec3 ret;
        	// Select vector based on cubemap face index.
            // Sadly 'switch' doesn't seem to work, at least on NVIDIA.
            if(gl_GlobalInvocationID.z == 0)      ret = vec3(1.0,  uv.y, -uv.x);
            else if(gl_GlobalInvocationID.z == 1) ret = vec3(-1.0, uv.y,  uv.x);
            else if(gl_GlobalInvocationID.z == 2) ret = vec3(uv.x, 1.0, -uv.y);
            else if(gl_GlobalInvocationID.z == 3) ret = vec3(uv.x, -1.0, uv.y);
            else if(gl_GlobalInvocationID.z == 4) ret = vec3(uv.x, uv.y, 1.0);
            else if(gl_GlobalInvocationID.z == 5) ret = vec3(-uv.x, uv.y, -1.0);
            return normalize(ret);
        }

        LocalSize(32, 32, 1);
        void main(void)
        {
        	// The dispatch covers the face rounded up to whole workgroups; anything past the edge has no
        	// texel to write. See the note on the dispatch in ComputeImages::ProccessForImageCube.
        	if (any(greaterThanEqual(ivec2(gl_GlobalInvocationID.xy), imageSize(outputTexture))))
        		return;

        	vec3 direction = getSamplingVector();

            // The direction->UV mapping AND the sky's authored yaw, out of Common/SkyPanorama.glslh —
            // the same text DiffuseIrradiance reads the same panorama with. It used to be three lines
            // written out here and three more written out there.
            vec2 sampleUV = PanoramaSampleUV(direction, skyLook.YawCosSin.xy);
            vec4 color = texture(inputTexture, sampleUV);

        	imageStore(outputTexture, ivec3(gl_GlobalInvocationID),
        	           vec4(ApplySkyGain(color.rgb, skyLook.Gain.rgb), color.a));
        }
    }
}
