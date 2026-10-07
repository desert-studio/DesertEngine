// DesertAsset {"Kind":"Shader","Guid":"13cdf49c6bf0428790436694facfc331","Versions":{"SHDR":1},"Dependencies":[]}
// Engine VFX module `engine:CurlNoiseForce` (UE Niagara Content/Modules/Update/Forces/CurlNoiseForce): adds
// Strength times the curl of a vector simplex noise field to the particle's force. The field is sampled at
// (Position + Offset - Pan * EmitterAge) * Frequency — Frequency in cycles per centimetre, Pan the field's travel in
// cm/s, Offset a shift of the field (a Random-source range is UE's per-emitter randomisation vector). The curl is
// the analytic one of UE JacobianSimplex_ALU (Common/VFXNoise.glslh), evaluated at the particle every step — UE's
// stateless noise LUT exists only because a stateless particle has no position to sample (plan §5.5.1). Not
// normalised, so the field stays divergence-free. A force: SolveForcesAndVelocity divides by the mass. Update group.
Shader "CurlNoiseForce"
{
    Domain Particle
    Particle
    {
        Attribute Position vec3
        Attribute PhysicsForce vec3
        Input Strength float
        Input Frequency float
        Input Pan vec3
        Input Offset vec3

        void Module( inout ParticleCtx p, inout VFXSim sim, in ModuleInputs i )
        {
            const vec3 at = ( p.Position + i.Offset - i.Pan * sim.EmitterAge ) * i.Frequency;
            p.PhysicsForce += VFX_CurlNoise( at ) * i.Strength;
        }
    }
}
