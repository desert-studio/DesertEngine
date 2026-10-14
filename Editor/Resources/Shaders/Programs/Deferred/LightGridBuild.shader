// DesertAsset {"Kind":"Shader","Guid":"14b61e565a344b8e8e8edd8df0e9f7d5","Versions":{"SHDR":1},"Dependencies":[]}
Shader "LightGridBuild"
{
    // "Deferred: LightGrid" — the view's light grid (Mesh/LightGrid.glslh), UE LightGridInjection.usf: one thread per
    // froxel tests every point and spot light's bounding sphere against the froxel's view-space box and lists the
    // ones that touch it. Every test is conservative (the box contains every pixel the cell can hold, the sphere
    // contains every point the light reaches), so a light left out of a cell contributes exactly zero there.

    Compute
    {
        #include <Mesh/LightSources.glslh> // the PointLight / SpotLight rows, the same text the composite reads
        #include <Mesh/LightGrid.glslh>
        #include <Common/ScreenUV.glslh>

        ReadBuffer(0) LightGridParamsUB
        {
        	LightGridParams lightGrid;
        };
        ReadBuffer(1) PointLightsUB
        {
        	PointLight pointLights[];
        };
        ReadBuffer(2) SpotLightsUB
        {
        	SpotLight spotLights[];
        };
        // [cell counts][cell lists] — see Mesh/LightGrid.glslh.
        Buffer(3) LightGridCellsUB
        {
        	uint lightGridCells[];
        };

        LocalSize(4, 4, 4);

        // The view-space point on the ray through NDC @p ndc at view depth @p depth. Two points of the ray from the
        // inverse projection, then the one at -z = depth: valid for perspective and orthographic projections alike,
        // and for reversed or standard depth (both depths sampled are finite in either).
        vec3 PointAtDepth(vec2 ndc, float depth)
        {
        	const vec4 a4 = lightGrid.invProjection * vec4(ndc, 0.25, 1.0);
        	const vec4 b4 = lightGrid.invProjection * vec4(ndc, 0.75, 1.0);
        	const vec3 a  = a4.xyz / a4.w;
        	const vec3 b  = b4.xyz / b4.w;
        	const float t = (-depth - a.z) / (b.z - a.z);
        	return a + t * (b - a);
        }

        // Squared distance from @p c to the box [lo, hi].
        float DistanceSqToBox(vec3 c, vec3 lo, vec3 hi)
        {
        	const vec3 d = max(lo - c, vec3(0.0)) + max(c - hi, vec3(0.0));
        	return dot(d, d);
        }

        bool SphereTouchesBox(vec3 center, float radius, vec3 lo, vec3 hi)
        {
        	return radius > 0.0 && DistanceSqToBox(center, lo, hi) <= radius * radius;
        }

        // The smallest sphere around a spot light's reach — the part of the ball of `range` inside the cone of
        // half-angle acos(cosOuter) (the cap included, since the falloff is by distance). Up to 45 degrees: the
        // sphere through the apex and the cap's rim, centred on the axis; wider: the sphere around the rim's disc;
        // past 90 degrees the whole ball.
        vec4 SpotBoundingSphere(SpotLight light, vec3 apex, vec3 axis)
        {
        	const float c = light.cosOuter;
        	if (c >= 0.70710678)
        	{
        		const float r = light.range / (2.0 * c);
        		return vec4(apex + axis * r, r);
        	}
        	if (c > 0.0)
        		return vec4(apex + axis * (light.range * c), light.range * sqrt(1.0 - c * c));
        	return vec4(apex, light.range);
        }

        void main()
        {
        	const uvec3 cell = gl_GlobalInvocationID;
        	if (any(greaterThanEqual(cell, lightGrid.size.xyz)))
        		return;

        	// The froxel's box in view space: the four rays through the tile's pixel corners, cut at the slice's
        	// two depths. Slice 0 starts at the camera and the last slice ends at far, matching LightGridSlice.
        	const float tile   = lightGrid.zParams.w;
        	const vec2  extent = lightGrid.extent.xy;
        	const vec2  px0    = vec2(cell.xy) * tile;
        	const vec2  px1    = min(px0 + tile, extent);
        	const uint  last   = lightGrid.size.z - 1u;
        	// A relative margin both ways: the reader's log() and the builder's exp() round differently at a
        	// slice boundary, and a pixel there must still find the lights of the slice it lands in.
        	const float nearD  = cell.z == 0u ? 0.0 : LightGridSliceDepth(lightGrid, cell.z) * 0.999;
        	const float farD   = (cell.z == last ? lightGrid.zParams.z : LightGridSliceDepth(lightGrid, cell.z + 1u)) * 1.001;

        	vec3 lo = vec3( 3.0e38);
        	vec3 hi = vec3(-3.0e38);
        	for (uint k = 0u; k < 4u; k++)
        	{
        		const vec2 px  = vec2((k & 1u) != 0u ? px1.x : px0.x, (k & 2u) != 0u ? px1.y : px0.y);
        		const vec2 ndc = ScreenUVToNdc(px / extent);
        		const vec3 pn  = PointAtDepth(ndc, nearD);
        		const vec3 pf  = PointAtDepth(ndc, farD);
        		lo = min(lo, min(pn, pf));
        		hi = max(hi, max(pn, pf));
        	}
        	// The same margin sideways (a pixel's surface is reconstructed through a pixel CENTRE, strictly inside
        	// the tile, but the box corners are rounded too).
        	const vec3 pad = vec3(farD * 1.0e-3 + 0.01);
        	lo -= pad;
        	hi += pad;

        	const uint index    = LightGridCellIndex(lightGrid, cell);
        	const uint list     = LightGridListOffset(lightGrid, index);
        	const uint capacity = lightGrid.size.w;
        	uint       count    = 0u;

        	for (uint i = 0u; i < lightGrid.counts.x; i++)
        	{
        		const vec3 center = (lightGrid.view * vec4(pointLights[i].position, 1.0)).xyz;
        		if (SphereTouchesBox(center, pointLights[i].radius, lo, hi))
        		{
        			if (count < capacity)
        				lightGridCells[list + count] = i;
        			count++;
        		}
        	}
        	for (uint i = 0u; i < lightGrid.counts.y; i++)
        	{
        		const vec3 apex   = (lightGrid.view * vec4(spotLights[i].position, 1.0)).xyz;
        		const vec3 axis   = normalize(mat3(lightGrid.view) * spotLights[i].direction);
        		const vec4 sphere = SpotBoundingSphere(spotLights[i], apex, axis);
        		if (SphereTouchesBox(sphere.xyz, sphere.w, lo, hi))
        		{
        			if (count < capacity)
        				lightGridCells[list + count] = lightGrid.counts.x + i;
        			count++;
        		}
        	}
        	lightGridCells[index] = count;
        }
    }
}
