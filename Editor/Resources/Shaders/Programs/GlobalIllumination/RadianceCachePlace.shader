// DesertAsset {"Kind":"Shader","Guid":"bf5b4fa048324dd39347fc331c64b1f1","Versions":{"SHDR":1},"Dependencies":[]}
// THE RADIANCE CACHE UPDATE, 5/7 — PLACE (UE LumenRadianceCache: the probe placement of the updated probes, one
// thread per probe, before TraceFromProbes reads it): every probe of this frame's trace list is moved out of
// geometry by the tracer (GIMoveOutOfGeometry; not in UE: a world-grid probe inside a wall would see only the wall's
// back faces; DDGI relocates its probes the same way) - up to a quarter cell of free space around the probe, at most
// 0.45 cell from the cell centre per axis (the probe stays the cell's) - and its offset is written to the probe,
// where Trace reads its ray origin and every reader its interpolation position (RadianceCache.glslh).
// FPS-RC-TRACE2: Trace ran it in thread 0 of every 8x8 trace tile (up to 16 times a probe, a field walk each)
// with the tile's other 63 threads at a barrier; here it runs once a probe, and Trace has no barrier. The result
// is the same point: the same function of the same cell centre and this frame's field / scene. The program is
// compiled per tracer (GlobalIllumination::TracerVariant), as Trace's; it lights no hit (GI_HIT_LIGHTING_NONE),
// so it declares no point-clamped state (DeclareTracerReads Skip).
Shader "RadianceCachePlace"
{
    Compute
    {
        LocalSize(64, 1, 1);

        #include <Common/RadianceCacheState.glslh>

        // 32..42: the tracer's own resources (Common/GITracer.glslh), the bindings Trace gives them.
        #define GI_HIT_LIGHTING_NONE 1
        #define GI_TRACER_BINDING 32
        #include <Common/GITracer.glslh>

        void main()
        {
            const uint count = min( radianceCacheSchedule[RC_SCHEDULE_COUNT], radianceCache.atlas.z );
            if ( gl_GlobalInvocationID.x >= count )
                return;
            const uint               probe   = radianceCacheSchedule[RC_SCHEDULE_LIST + gl_GlobalInvocationID.x];
            const RadianceCacheProbe state   = radianceCacheProbes[probe];
            const uint               clipmap = uint( state.cell.w );
            const float              cell    = RadianceCacheCellSize( radianceCache, clipmap );
            const vec3               centre  = RadianceCacheWorldCellCentre( radianceCache, state.cell.xyz, clipmap );
            const vec3               p       = GIMoveOutOfGeometry( centre, 0.25 * cell, 0.45 * cell );
            radianceCacheProbes[probe].offset = vec4( p - centre, 0.0 );
        }
    }
}
