# PHYS-GTA-ARCH — physics programme at GTA 6 level (design, 11-21)

Owner 11-21: «ткань + волосы + кор физика уровня GTA 6, где будет всё: от минимума до царапин на машинах,
людях». The game is an RP multiplayer (GTA 5 RP shape) with SERVER AUTHORITY (owner 10-09: physics behind an
interface, Jolt never exposed, SaveState/RestoreState for prediction). NET-ARCH runs in parallel; every system
below says what the server simulates and what is cosmetic on clients.

This is a DESIGN document: no code, no build. The lead decides from the choice table (§3); slices are §6.

References: UE 5.5 Chaos (rigid, Chaos Cloth + ML Cloth, Chaos Flesh, Chaos Vehicles, Physics Control /
PhysicalAnimationComponent, Groom + strand sim) and RAGE/Euphoria (GTA: active ragdoll, vehicle deformation,
layered damage/dirt/scratch on vehicles and peds, blood/wound decals).

## 1. Audit — what exists, what is a stub (tree at dev tip 11-21)

| Area | State | Where |
|---|---|---|
| Jolt | **Vendored 5.5** with everything this programme needs: `Physics/Vehicle` (Wheeled/Motorcycle/Tracked controllers, engine, transmission, differential, anti-roll), `Physics/SoftBody` (skinned constraints with MaxDistance/backstop, LRA, dihedral bend, volume/pressure, Cosserat rods), `Physics/Hair` (GPU strand sim through `Jolt/Compute` VK/MTL/DX12/CPU — self-declared "in development": no LOD, no wind, convex-hull-only collision), `StateRecorder` (SaveState/RestoreState on bodies, constraints, vehicles, soft bodies), `DeterminismLog` | `ThirdParty/JoltPhysics/Jolt` |
| Jolt build flags | **None set**: no `JPH_CROSS_PLATFORM_DETERMINISTIC`, no compute backend, no double precision | `cmake/ThirdParty/Jolt.cmake` (11 lines) |
| Rigid core | **Real.** `PhysicsWorld` (pimpl over Jolt): fixed step bank (`Step`, `GetStepCount`, interpolation alpha — the server-tick premise is already there), bodies/compound/instanced static/height field, forces/impulses, raycasts, contact events with impulses, pre/post-step callbacks, collision profiles, physical materials, physics-asset format | `Desert/PhysicsCore/Source/PhysicsCore/PhysicsWorld.hpp:275-523`, `PhysicsWorld.cpp` (2272 l.), `CollisionProfiles.*`, `PhysicalMaterial.*`, `PhysicsAssetFormat.*` |
| State save/restore | **Missing.** No SaveState/RestoreState on `PhysicsWorld` (sym.sh: none) | — |
| Character controller | **Real.** Jolt `CharacterVirtual` capsule (slopes, steps, crouch) | `PhysicsWorld.hpp:459-482`, `Engine/ECS/System/CharacterMovement.cpp` |
| Ragdoll | **Real, passive only.** `RagdollMotion { Kinematic, Simulated }` (`PhysicsWorld.hpp:262`), Jolt `Ragdoll` + SwingTwist joints, physics-asset generation, write-back to Animator (`RagdollLifetime.hpp`). No motors, no partial blend, no physical animation (`DriveToPoseUsingKinematics` only, `PhysicsWorld.cpp:1010`) | `Engine/Physics/*`, `Engine/ECS/System/RagdollLifetime.*`, Editor PhysicsAssetEditor |
| Vehicles | **Missing entirely** (sym.sh: no Vehicle/Wheel) | — |
| Destruction | **Real.** Chaos-like: fracture bake (Voronoi/planar/brick/radial), runtime fracture on workers, fields (impulse/strain/kill/anchor), connectivity + strain break, events | `Engine/Destruction/*` (16 files, 3.5k l.), `ECS/System/Destructible*.{hpp,cpp}`, `DestructionFields.*` |
| Cloth | **Interface only.** `IClothingSimulation` / factory / registry, `ClothingAsset` (physical mesh, MaxDistance/backstop maps, config, sphere/capsule colliders), wind via `ECS::WindAt`. Backend "CLO1 JoltSoftBody" named but not written; only a fake in a test | `PhysicsCore/Cloth/ClothingSimulation.hpp`, `ClothingAsset.hpp`; `Animation/Modular/ModularCharacter.hpp` (`CreateCloth`, no implementer) |
| Hair | **Interface only.** `GroomAsset` (curves SoA, guides, binding), `IGroomSimulation`/factory, `HairRenderFrame` contract. No importer, solver, interpolation or strand renderer | `Engine/Hair/GroomAsset.hpp`, `GroomSimulation.hpp` |
| Flesh / jiggle | **Missing** | — |
| Surface damage (scratches/dirt/blood) | **Missing.** DBuffer projected decals exist (world-space boxes over depth — they swim on moving/skinned meshes, so not usable for wounds/scratches); no per-mesh runtime paint masks | `Graphic/Systems/Scene/Decals/DecalRenderer.hpp`, `ECS/System/DecalECSSystem.hpp` |
| Stamp-into-texture compute | **Real, reusable pattern.** `DeformStampApply` stamps contact batches (pos, pressure, speed, slip, seed) into clipmap textures — the same shape as a damage stamp | `Generated/DeformStampApply`, landscape deform |
| Skinning | **Real.** Vertex or compute skin cache; morph targets on GPU | `DrawSkinnedMeshCommand.hpp`, `Programs/Compute/MorphTargets.shader` |
| Networking | **Missing** (no replication code, NET-ARCH is the parallel design) | — |

## 2. Principles that bind every subsystem

1. **One physics world interface, Jolt inside.** Every new capability enters `PhysicsWorld` (or a sibling in
   PhysicsCore) as an engine type; ECS/Luau see components and events, never JPH types (owner 10-09).
2. **Two simulation classes.** *Authoritative* (server steps it, clients predict/interpolate): rigid bodies that
   affect gameplay, characters, vehicles, part detachment, damage HEALTH. *Cosmetic* (each client steps its own,
   server never): cloth, hair, flesh, ragdoll limb detail, debris, glass shards, surface masks. Cosmetic systems
   receive replicated EVENTS (impact, hit, shatter) with a seed and rebuild the same look locally.
3. **Events, not fields, on the wire.** Damage is replicated as a bounded list of stamps (mesh-local point,
   direction, energy, kind, seed); every client rebuilds the same masks/deformation from them. A late joiner gets
   the compacted list. Never a texture or a vertex buffer over the network.
4. **Cross-platform deterministic Jolt.** Server (Windows/Linux) and clients (Windows/Mac) must replay the same
   inputs to the same state for rewind-and-resimulate of the own car/character: `JPH_CROSS_PLATFORM_DETERMINISTIC`
   on (a few % CPU), same step (`kFixedStepSeconds`), StateRecorder for save/restore.
5. **Look from assets.** Scratch/dent/dirt/blood/burn LOOKS are material-layer assets (textures, normal detail);
   code only says WHERE (masks) and HOW MUCH. Deformation shape comes from the authored mesh + impact; glass
   crack patterns from fracture assets.
6. **LOD by significance, per player count.** Every cosmetic sim has tiers (hero / near / far / off) chosen by a
   single significance manager (UE `USignificanceManager` pattern: screen size, distance, is-local-player,
   is-in-interaction). Budgets in §5.

## 3. Choice table (a = Jolt-native, b = own GPU solver, c = port of UE/Chaos pattern)

| Subsystem | a Jolt | b own GPU | c UE/Chaos | **Recommendation** |
|---|---|---|---|---|
| Rigid core + state | Jolt 5.5, already ours; StateRecorder exists; deterministic flag | — | Chaos: slower, determinism weaker across platforms | **a.** Add SaveState/RestoreState + deterministic flag (PHYS-1). Chaos is architecturally worse here (UE networked physics resim is still experimental in 5.5). |
| Vehicle handling | `VehicleConstraint` + Wheeled/Motorcycle controllers, sweeps for wheels, engine/gearbox/diff/anti-roll, StateRecorder-ready | — | Chaos Vehicles: same raycast-wheel model, tied to UE movement component | **a + own tyre/assist layer.** Jolt chassis/suspension/drivetrain; tyre forces through a data-driven `VehicleHandling` asset (GTA `handling.meta` pattern: grip curves, traction bias, steering lock, assists) plugged as Jolt friction callbacks. |
| Crumple deformation | Soft body per car: BeamNG quality but ~5-10k nodes x 64 cars on CPU — no | **Vehicle deformation field**: vehicle-local sparse displacement lattice, stamped by impacts, applied in the skin-cache compute pass to render mesh + coarse collision hull rebuilt on server | Chaos: geometry-collection fracture only, no crumple | **b (RAGE pattern).** UE has nothing here — architecturally worse; GTA's deformation texture is the reference. Deterministic from the stamp list. |
| Detachable parts | Jolt hinge/fixed constraints with break thresholds, parts become free bodies | — | Chaos: GC clusters | **a.** Doors/hood/trunk/bumpers = constrained bodies with part health; break → free body (authoritative while moving, then cosmetic sleep/despawn). |
| Glass | — | — | Chaos fracture | **Existing Destruction** (radial runtime fracture), cosmetic shards; server holds a per-window `Intact/Cracked/Shattered` bit. |
| Surface damage layer (vehicles + characters) | — | **UV-space runtime masks** in a pooled atlas (scratch, dent-AO, dirt, mud, wet, blood, burn), stamped by compute (DeformStampApply shape), read by a material Damage layer | UE: ad-hoc render-target painting, RVT does not fit skinned/moving | **b.** One `SurfaceDamage` system for cars, peds and clothes; skinned "decals" = UV-space stamps (they do not swim). UE has no engine-level answer; RAGE layered damage is the reference. |
| Ragdoll + hit reactions | Jolt `Ragdoll::DriveToPoseUsingMotors` (SwingTwist motors), per-part motor strength | — | UE Physics Control / PhysicalAnimationComponent (spring targets from anim) | **a + c pattern.** Jolt motors driven by UE Physics Control-style per-body profiles (strength, damping, blend), partial-body blend for hit reactions. |
| Active ragdoll / balance (Euphoria) | motors as actuators | — | UE has none at Euphoria level | **own behaviour layer on a.** Balance controller (CoM over support, stepping, bracing, protect-head, catch-fall, writhe) choosing motor targets; behaviours as data assets. UE is worse here — RAGE/Euphoria is the reference. |
| Flesh / jiggle | Jolt skinned soft body per region: good, CPU-heavy x64 | **GPU XPBD tetra** (volume + skinned attachment), hero tier | Chaos Flesh: FEM, mostly offline/ML-baked | **Two tiers:** near = **spring-bone secondary motion** (AnimDynamics/“jiggle bones”, all characters, CPU, cheap); hero = **b GPU XPBD tetra** for close-up and contact scenes. ML deformer later (not in this programme). |
| Character-to-character contact (incl. intimate) | collision proxies (capsules) for both | flesh XPBD with partner's SDF/capsules as colliders | UE Contextual Animation + Motion Warping | **c pattern + b.** Paired interaction asset (synced clips, alignment, IK contacts) is the base; physics adds mutual flesh/cloth collision against partner proxies. Server = interaction state only. |
| Cloth | **Jolt soft body** (skinned constraints = MaxDistance/backstop, LRA, dihedral): the seam was designed for it (CLO1) | GPU XPBD backend later if budgets demand | Chaos Cloth: same XPBD family, ML Cloth for hero | **a** (CLO1), with LOD tiers; registry keeps room for a GPU backend. Quality parity with Chaos Cloth; ML cloth is a later hero tier. |
| Hair | Jolt Hair: GPU, but no LOD/wind, convex-only collision, own compute device | **own GPU guide solver in our RDG/DSL**, algorithm after Jolt Hair (MIT) + UE Groom interpolation | UE Groom: strands + cards + meshes LOD, Niagara guides | **b with c structure.** Groom asset → binding → guide sim (GPU) → interpolation → strand renderer; LOD to cards/meshes with bone-chain dynamics. Collision = body capsules + character SDF. Not Jolt Hair directly: second compute device, missing features. |
| Prediction / rewind | StateRecorder | — | UE networked physics (experimental) | **a**, design owned by NET-ARCH; physics provides save/restore + input replay hooks. |

## 4. Server vs client split

| System | Server (authoritative) | Client |
|---|---|---|
| Rigid bodies (props that matter) | simulates, replicates state | interpolates; predicts only own-touched bodies |
| Character capsule | simulates (input → CharacterVirtual) | predicts own, interpolates others |
| Vehicle | simulates chassis/wheels/drivetrain from driver input | predicts own (rewind on correction), interpolates others; wheel spin, suspension visual |
| Vehicle deformation | applies stamps to coarse collision hull + part health; replicates stamp list | rebuilds render deformation field from stamps |
| Detached parts / glass | part detach + window state bits; moving detached part replicated until sleep | shards, crack visuals, sleeping parts cosmetic |
| Surface damage masks | nothing but the stamp events (from hits it already resolves) | stamps masks locally |
| Health / wounds | hit zones, bleed state | wound stamps, blood masks, decals |
| Ragdoll | down/dead state, root transform (low rate), hit impulse event | full ragdoll + active ragdoll, settles locally; root corrected softly |
| Active ragdoll / stumble | gameplay outcome (fell / stayed up) decided server-side | behaviour motion cosmetic, constrained to outcome |
| Cloth, hair, flesh, jiggle | nothing | all, LOD by significance |
| Close-contact interaction | interaction id, participants, anchor, clip time | paired animation, IK, mutual flesh/cloth collision |
| Destruction | fracture events (what broke, seed) for gameplay-relevant objects | pieces, debris |

## 5. Cost at 64+ players on screen (targets, mid PC / Mac M-class)

| Tier | Who | Cloth | Hair | Flesh | Ragdoll |
|---|---|---|---|---|---|
| Hero | local player, interaction partners, camera < 3 m | full particle mesh | strands, guides sim | GPU XPBD tetra | active ragdoll |
| Near | < 15 m, top ~8 by significance | reduced mesh | cards + bone chains | spring bones | active ragdoll |
| Far | < 60 m | skinned only | cards, no sim | none | passive ragdoll |
| Off | beyond | skinned | mesh LOD | none | animation only |

Rigid/vehicle budget: server 64 vehicles + 128 characters at 60 Hz in < 4 ms on 8 threads (Jolt range).
Cosmetic budget per client: cloth ≤ 2 ms CPU (jobs), hair + flesh ≤ 1.5 ms GPU, masks ≤ 0.3 ms GPU per frame.

## 6. Slices PHYS-1..N (minimum first; each = one result)

1. **PHYS-1 STATE** — `PhysicsWorld::SaveState/RestoreState` over Jolt StateRecorder + `JPH_CROSS_PLATFORM_DETERMINISTIC` in `Jolt.cmake`.
   Тогда [phys1_restore]: a stack of 20 boxes saved at step 60, run to 180, restored and re-run to 180 shows the identical pile; overlay prints equal state hashes.
2. **PHYS-2 VEHICLE-CORE** — `.devehicle` asset (`VehicleHandling`: wheels, suspension, engine, gearbox, diff, grip curves), `VehicleComponent`, Jolt `VehicleConstraint`, Luau throttle/steer/brake.
   Тогда [phys2_drive]: a car accelerates, turns and brakes on the test track; suspension compresses on the kerb; wheels turn with speed.
3. **PHYS-3 RAGDOLL-HIT** — knockdown/death ragdoll from a hit impulse at the hit bone, get-up blend back to animation.
   Тогда [phys3_hit]: CesiumMan hit on the left shoulder falls to the left, settles, then stands up blending into idle without a pop.
4. **PHYS-4 CLOTH-CLO1** — Jolt soft-body cloth backend behind `IClothingSimulation`, ECS stepping at the fixed step, ModularCharacter implementer.
   Тогда [phys4_cloth]: a skirt/cape on the walking humanoid swings with the stride and does not pass through the legs.
5. **PHYS-5 HAIR-CARDS** — hair cards/mesh LOD with bone-chain dynamics (spring bones) for all characters.
   Тогда [phys5_cards]: a ponytail of cards lags and swings when the character turns sharply.
6. **PHYS-6 SURFACE-DAMAGE** — `SurfaceDamage` system: pooled UV-space masks, stamp compute, material Damage layer asset; first client: vehicles (scratch, dirt).
   Тогда [phys6_scratch]: a car scraped along a wall shows a scratch band along the scraped side only, paint cut to primer per the layer asset.
7. **PHYS-7 VEHICLE-PARTS** — doors/hood/trunk/bumpers as constrained parts with health; break into free bodies.
   Тогда [phys7_parts]: a side impact swings the door open on its hinge; a second impact tears it off and it tumbles on the road.
8. **PHYS-8 GLASS** — windows crack then shatter through runtime radial fracture; server window state bit.
   Тогда [phys8_glass]: one shot cracks the windscreen (crack star), the second shatters it into falling shards.
9. **PHYS-9 CRUMPLE** — vehicle deformation field (vehicle-local lattice, compute in skin cache), coarse hull rebuild on server.
   Тогда [phys9_crumple]: a frontal pole crash pushes the bonnet in around the pole; the same stamps replayed in a second editor give the same dent.
10. **PHYS-10 CHAR-DAMAGE** — SurfaceDamage on peds and garments: wounds, blood, dirt, wet; skinned stamps.
    Тогда [phys10_wound]: a shot to the torso leaves a wound and a blood run on shirt and skin that move with the walking body.
11. **PHYS-11 PHYS-ANIM** — Physics Control-style motor profiles via `DriveToPoseUsingMotors`, partial-body hit reactions.
    Тогда [phys11_flinch]: a punch to the head snaps the head and upper torso back while the legs keep walking, recovery within 0.5 s.
12. **PHYS-12 BALANCE** — Euphoria-style behaviours (stumble-step, brace, catch-fall, protect head) as assets.
    Тогда [phys12_stumble]: a shove makes the character take two recovery steps and stay up; a harder shove makes it fall with hands out.
13. **PHYS-13 JIGGLE** — spring-bone secondary motion for flesh regions (near tier).
    Тогда [phys13_jiggle]: on a jump landing, belly/chest regions overshoot and settle in ~0.3 s; no motion at rest.
14. **PHYS-14 CLOTH-LOD-WIND** — significance manager + cloth tiers + `ECS::WindAt` + cloth on vehicles (flags, tarps).
    Тогда [phys14_crowd]: 64 walking characters in wind, near ones' garments simulated, far ones skinned; frame time within §5 budget on the overlay.
15. **PHYS-15 GROOM-ASSET** — groom asset, Alembic curves importer, binding builder (roots on skinned mesh).
    Тогда [phys15_groom]: an imported groom stands on the animated head in rest shape, roots following the scalp.
16. **PHYS-16 GROOM-SIM** — GPU guide solver (stretch/bend/twist, gravity, wind, body-capsule + SDF collision) + strand interpolation.
    Тогда [phys16_strands]: long strands swing with head turns, rest on the shoulders, do not pass through the neck.
17. **PHYS-17 FLESH-XPBD** — GPU tetra soft body (volume, skinned attachment), hero tier.
    Тогда [phys17_flesh]: a hand pressing the upper arm dents the flesh under the fingers and it recovers when released.
18. **PHYS-18 CONTACT** — close-contact interaction asset (paired clips, alignment, IK contacts) + mutual flesh/cloth collision against partner proxies.
    Тогда [phys18_contact]: two characters in a paired hug: arms rest on the partner's back without penetration, clothes compress between the bodies.
19. **PHYS-19 BIKES-TYRES** — motorcycles (Jolt MotorcycleController, lean), tyre burst / rim driving.
    Тогда [phys19_bike]: a motorbike leans into a turn; a burst front tyre visibly flattens and the bike pulls to that side.
20. **PHYS-20 PREDICT** — client prediction + rewind of own vehicle/character on correction (with NET-ARCH).
    Тогда [phys20_rewind]: with 150 ms simulated latency the own car drives smoothly; a forced server correction snaps nothing visible.

Order rationale: 1-5 are the minimum playable (state for netcode, cars, ragdoll, cloth, cheap hair); 6-10
are the GTA damage stack; 11-13 make bodies alive; 14-18 the AAA tier; 19-20 widen.

## 7. Open owner questions (5)

1. **Intimate scenes:** the engine provides generic close contact (PHYS-18). Does the game ship explicit
   content (nudity/anatomy assets, age-gating, platform rules)? That decides whether flesh hero tier covers it.
2. **Crumple depth:** GTA-style deformation field for all cars (recommended), or additionally BeamNG-style full
   soft-body for the player's own car (very expensive, server cannot afford it for 64 cars)?
3. **Hair strands:** strands for hero tier only and cards for the crowd (recommended), or strands for everyone
   near (RTX-class GPUs only)?
4. **Server platform + tick:** Linux dedicated server and 60 Hz physics (recommended), or 30 Hz? Determinism flag
   and budgets depend on it (shared with NET-ARCH).
5. **Handling feel:** GTA arcade-sim with assists (recommended) or simulation-grade handling?
