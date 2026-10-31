// DesertAsset {"Kind":"Shader","Guid":"bc2e2cfe6aa74a9e9f6409c4acf73d94","Versions":{"SHDR":1},"Dependencies":[]}
// A round print with a raised rim (DEFORM-FIELD): the stamp of a foot, a pole or a dropped crate in soft ground.
// The sink is a smooth bowl to the contact radius, its depth scaled by the contact's pressure; the material it
// displaced stands as a ring from 0.9 to 1.6 radii, highest at 1.15. Contract: Deformation/DeformStampContract.glslh.
Shader "DS_CircleRim"
{
    Domain DeformStamp
    Stamp
    {
        void DeformStampEvaluate(in DeformStampInputs i, inout DeformStampOutputs o)
        {
            const float r     = length(i.Local);
            const float depth = 6.0 * i.Pressure; // cm at the centre for pressure 1
            // The bowl: full depth inside 0.6 R, rising smoothly to the rest surface at R.
            o.Sink = -depth * (1.0 - smoothstep(0.6, 1.0, r));
            // The rim: a third of the depth at 1.15 R, falling to nothing at 0.9 R and 1.6 R.
            const float rise = smoothstep(0.9, 1.15, r) * (1.0 - smoothstep(1.15, 1.6, r));
            o.Rim        = depth * 0.35 * rise;
            o.Compaction = 1.0 - smoothstep(0.8, 1.1, r);
            o.Flatten    = 1.0 - smoothstep(1.0, 1.4, r);
            // Grass lies outward from the centre: the local offset turned back into world X, Z.
            const vec2 right   = vec2(cos(i.Heading), -sin(i.Heading));
            const vec2 forward = vec2(sin(i.Heading), cos(i.Heading));
            const vec2 outward = i.Local.x * right + i.Local.y * forward;
            o.FlattenDirection = r > 1e-4 ? outward / r : forward;
            o.Wetness          = 0.0;
        }
    }
}
