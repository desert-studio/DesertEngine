// DesertAsset {"Kind":"Shader","Guid":"b32bd9a8eff741689d96e3757cb1ba47","Versions":{"SHDR":1},"Dependencies":[]}
Shader "ReflectionCaptureAssemble"
{
    Compute
    {
        // THE SIX FACE VIEWS, RESAMPLED INTO ONE CUBE (mip 0 of the capture's radiance cube). Each face view is
        // the capture's own scene render: a 90-degree square view along one axis (ReflectionCaptureRenderer).
        // The texel's direction is projected through THAT view's view-projection, so the cube does not depend on
        // which up vector, handedness or Y-flip the renderer's projection uses - the view's own matrix is the
        // only convention read. The six 90-degree frusta partition the sphere along the major axis, so the major
        // axis of the direction names the one face view that saw it.

        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform sampler u_LinearClampSampler;
        Uniform(0) texture2D u_FacePosX;
        #define u_FacePosX sampler2D(u_FacePosX, u_LinearClampSampler)
        Uniform(1) texture2D u_FaceNegX;
        #define u_FaceNegX sampler2D(u_FaceNegX, u_LinearClampSampler)
        Uniform(2) texture2D u_FacePosY;
        #define u_FacePosY sampler2D(u_FacePosY, u_LinearClampSampler)
        Uniform(3) texture2D u_FaceNegY;
        #define u_FaceNegY sampler2D(u_FaceNegY, u_LinearClampSampler)
        Uniform(4) texture2D u_FacePosZ;
        #define u_FacePosZ sampler2D(u_FacePosZ, u_LinearClampSampler)
        Uniform(5) texture2D u_FaceNegZ;
        #define u_FaceNegZ sampler2D(u_FaceNegZ, u_LinearClampSampler)

        // Rotation-only view-projections (the views' translation removed: a direction, not a point, is
        // projected), in the face order above; Probe.x = a distance inside every view's depth range.
        ReadBuffer(6) ReflectionCaptureFacesUB
        {
            mat4 u_FaceViewProjection[6];
            vec4 u_FaceProbe;
        };

        layout(binding = 7, rgba16f) writeonly uniform imageCube u_Output;

        // The cube texel's direction, in the convention every cube of the engine is written in
        // (Programs/Compute/PrefilterEnvMap.shader, getSamplingVector).
        vec3 CubeTexelDirection(uvec3 id, vec2 size)
        {
        	vec2 st = (vec2(id.xy) + 0.5) / size;
        	vec2 uv = 2.0 * vec2(st.x, 1.0 - st.y) - vec2(1.0);
        	vec3 ret;
        	if (id.z == 0)      ret = vec3(1.0,  uv.y, -uv.x);
        	else if (id.z == 1) ret = vec3(-1.0, uv.y,  uv.x);
        	else if (id.z == 2) ret = vec3(uv.x, 1.0, -uv.y);
        	else if (id.z == 3) ret = vec3(uv.x, -1.0, uv.y);
        	else if (id.z == 4) ret = vec3(uv.x, uv.y, 1.0);
        	else                ret = vec3(-uv.x, uv.y, -1.0);
        	return normalize(ret);
        }

        vec3 SampleFace(int face, vec2 uv)
        {
        	if (face == 0) return textureLod(u_FacePosX, uv, 0.0).rgb;
        	if (face == 1) return textureLod(u_FaceNegX, uv, 0.0).rgb;
        	if (face == 2) return textureLod(u_FacePosY, uv, 0.0).rgb;
        	if (face == 3) return textureLod(u_FaceNegY, uv, 0.0).rgb;
        	if (face == 4) return textureLod(u_FacePosZ, uv, 0.0).rgb;
        	return textureLod(u_FaceNegZ, uv, 0.0).rgb;
        }

        LocalSize(8, 8, 1);
        void main()
        {
        	const ivec2 size = imageSize(u_Output);
        	if (gl_GlobalInvocationID.x >= uint(size.x) || gl_GlobalInvocationID.y >= uint(size.y))
        		return;

        	const vec3 d = CubeTexelDirection(gl_GlobalInvocationID, vec2(size));
        	const vec3 a = abs(d);
        	int face;
        	if (a.x >= a.y && a.x >= a.z)
        		face = d.x >= 0.0 ? 0 : 1;
        	else if (a.y >= a.z)
        		face = d.y >= 0.0 ? 2 : 3;
        	else
        		face = d.z >= 0.0 ? 4 : 5;

        	const vec4 clip = u_FaceViewProjection[face] * vec4(d * u_FaceProbe.x, 1.0);
        	const vec2 ndc  = clip.xy / clip.w;
        	// The inverse of Common/ScreenUV.glslh's ScreenUVToNdc.
        	const vec2 uv = clamp(vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5), vec2(0.0), vec2(1.0));

        	imageStore(u_Output, ivec3(gl_GlobalInvocationID), vec4(SampleFace(face, uv), 1.0));
        }
    }
}
