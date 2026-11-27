#pragma once

// THE PARTICLE PROGRAMS' GPU RECORDS, declared once: ParticleSimulate's step and Spawn from Channel rows, an
// emitter's Counters slot and draw layout, and the colliding / wind-reading stacks' view and air blocks.
// Tools/ShaderProtocolGen writes them as Generated/VFX/Particles.glslh, which Common/ParticlePool.glslh,
// Common/ParticleDrawLayout.glslh, Programs/Particles/ParticleSimulate.shader and ParticleDispatchArgs.shader
// include. The strides the passes allocate by stay in ParticleGpuLayout.hpp (read back from the SPIR-V by the
// shader tests).

#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>
#include <Engine/Graphic/WindGust.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace Desert::Graphic::System
{
    // A drawn attribute the emitter's stack does not declare (ParticleDrawLayoutGpu's columns): the reader's
    // default (Common/ParticleDrawRead.glslh).
    inline constexpr uint32_t kParticleAttrNone = 0xFFFFFFFFu;

    // One element of ParticleSimulate's step table (ParticleDispatchArgs reads the same table).
#define DESERT_PARTICLE_STEP_GPU_FIELDS( X )                                                                      \
    X( uint32_t, IdBase, "IdBase", 0u ) /* id of the first particle this step may spawn */                        \
    X( uint32_t, Seed, "Seed", 0u )     /* the emitter instance's seed (system xor entity xor emitter) */         \
    X( uint32_t, Budget, "Budget", 0u ) /* how many particles this step spawns, its channel ones included */      \
    /* VFX-10: spawn t < ChannelCount is Spawn from Channel particle ChannelFirst + t */                          \
    X( uint32_t, ChannelFirst, "ChannelFirst", 0u )                                                               \
    X( uint32_t, ChannelCount, "ChannelCount", 0u )
    DESERT_SHADER_STRUCT( ParticleStepGpu, "VFXStep", Std430, DESERT_PARTICLE_STEP_GPU_FIELDS );
#undef DESERT_PARTICLE_STEP_GPU_FIELDS

    // One Spawn from Channel particle of this tick (VFXWorld PlanEmitterSteps), the payload bound to it.
#define DESERT_PARTICLE_CHANNEL_SPAWN_GPU_FIELDS( X )                                                             \
    /* xyz world cm, w = 1 when the module binds a Position field */                                              \
    X( glm::vec4, Position, "Position", glm::vec4( 0.0f ) )                                                       \
    X( glm::vec4, Direction, "Direction",                                                                         \
       glm::vec4( 0.0f ) )                            /* xyz start velocity direction, w = 1 when bound */        \
    X( glm::vec4, Color, "Color", glm::vec4( 1.0f ) ) /* linear rgba, bound when Scalars.z = 1 */                 \
    /* x = lifetime seconds, y = base-size scale (1 when unbound), z = 1 when the colour is bound, w = 1 when the \
       lifetime is */                                                                                             \
    X( glm::vec4, Scalars, "Scalars", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( ParticleChannelSpawnGpu, "VFXChannelSpawn", Std430,
                          DESERT_PARTICLE_CHANNEL_SPAWN_GPU_FIELDS );
#undef DESERT_PARTICLE_CHANNEL_SPAWN_GPU_FIELDS

    // One slot of an emitter's Counters (Common/ParticlePool.glslh says who writes and reads it); the first four
    // members are the VkDrawIndirectCommand the billboard draw reads.
#define DESERT_PARTICLE_DRAW_SLOT_GPU_FIELDS( X )                                                                 \
    X( uint32_t, VertexCount, "VertexCount", 0u )                                                                 \
    X( uint32_t, InstanceCount, "InstanceCount", 1u )                                                             \
    X( uint32_t, FirstVertex, "FirstVertex", 0u )                                                                 \
    X( uint32_t, FirstInstance, "FirstInstance", 0u )                                                             \
    X( uint32_t, FreeCount, "FreeCount", 0u )                                                                     \
    X( uint32_t, Touched, "Touched",                                                                              \
       0u ) /* the particles compact c+1 scans: compact c's alive + step c's spawned */                           \
    X( uint32_t, Pad1, "Pad1", 0u )                                                                               \
    X( uint32_t, Pad2, "Pad2", 0u )
    DESERT_SHADER_STRUCT( ParticleDrawSlotGpu, "ParticleDrawSlot", Std430, DESERT_PARTICLE_DRAW_SLOT_GPU_FIELDS );
#undef DESERT_PARTICLE_DRAW_SLOT_GPU_FIELDS

    // Where a renderer finds an emitter's particle (Common/ParticleDrawLayout.glslh; UE
    // FNiagaraSpriteUniformParameters' data offsets): the first float column of each drawn attribute in the
    // emitter's SoA columns (kParticleAttrNone = the stack does not declare it), the range the columns are strided
    // by, and the transforms a Local emitter's particles are drawn through.
#define DESERT_PARTICLE_DRAW_LAYOUT_GPU_FIELDS( X )                                                               \
    /* x = range count (column stride), y = pool base, z = 1 local, w = the emitter's Sprite row alignment        \
       (0 Unaligned, 1 VelocityAligned; Vertex_ParticleSprite.glslh) */                                           \
    X( glm::uvec4, Range, "Range", glm::uvec4( 0u ) )                                                             \
    /* first float column of x = Position, y = SpriteSize, z = Color, w = SpriteRotation */                       \
    X( glm::uvec4, Columns0, "Columns0", glm::uvec4( kParticleAttrNone ) )                                        \
    /* first float column of x = Age, y = Lifetime, z = RibbonWidth, w = RibbonLinkOrder */                       \
    X( glm::uvec4, Columns1, "Columns1", glm::uvec4( kParticleAttrNone ) )                                        \
    /* the entity's full transform (cm): a Local emitter's particles are drawn through it (UE: a local-space      \
       emitter's LocalToWorld in the sprite / mesh vertex factory) */                                             \
    X( glm::mat4, LocalToWorld, "LocalToWorld", glm::mat4( 1.0f ) )                                               \
    /* x = the Ribbon row's RibbonIdBinding attribute (an Int column; none = one ribbon), y = Scale,              \
       z = MeshOrientation, w = Velocity (a VelocityAligned sprite's axis) */                                     \
    X( glm::uvec4, Columns2, "Columns2", glm::uvec4( kParticleAttrNone ) )                                        \
    /* the emitter's Ribbon row (Vertex_ParticleRibbon.glslh): x = Width (cm), y = UV mode (0 Stretch,            \
       1 TileByDistance), z = UVTileDistance (cm); zero without a Ribbon row */                                   \
    X( glm::vec4, Ribbon, "Ribbon", glm::vec4( 0.0f ) )                                                           \
    /* the entity's transform of the tick before (UE PrevLocalToWorld): a Local emitter's previous positions are  \
       drawn through it; a first tick's is LocalToWorld */                                                        \
    X( glm::mat4, PrevLocalToWorld, "PrevLocalToWorld", glm::mat4( 1.0f ) )                                       \
    /* x = the PreviousPosition column (UE Particles.Previous.Position; PreviousPositionColumn), yzw none */      \
    X( glm::uvec4, Columns3, "Columns3", glm::uvec4( kParticleAttrNone ) )
    DESERT_SHADER_STRUCT( ParticleDrawLayoutGpu, "ParticleDrawLayout", Std430,
                          DESERT_PARTICLE_DRAW_LAYOUT_GPU_FIELDS );
#undef DESERT_PARTICLE_DRAW_LAYOUT_GPU_FIELDS

    // ParticleSimulate's CollisionView block (a colliding stack, VFX_READS_SCENE_DEPTH): the simulating view's
    // jittered view-projection and its inverse (the matrices its scene depth was drawn with), the camera position
    // (cm), w = 1 when SceneDepth is this frame's scene depth (0: the frame has none, nothing collides).
#define DESERT_PARTICLE_COLLISION_VIEW_GPU_FIELDS( X )                                                            \
    X( glm::mat4, ViewProjection, "CollisionViewProjection", glm::mat4( 1.0f ) )                                  \
    X( glm::mat4, InvViewProjection, "CollisionInvViewProjection", glm::mat4( 1.0f ) )                            \
    X( glm::vec4, Camera, "CollisionCamera", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( ParticleCollisionViewGpu, "ParticleCollisionView", Std430,
                          DESERT_PARTICLE_COLLISION_VIEW_GPU_FIELDS );
#undef DESERT_PARTICLE_COLLISION_VIEW_GPU_FIELDS

    // CameraUB::LocalWind / LocalWindSpeed's shapes (Engine/Graphic/WindGust.hpp, Common/LocalWind.glslh).
    using ParticleWindSourceRows = std::array<glm::vec4, kMaxLocalWindSources>;
    using ParticleWindSpeedRows  = std::array<glm::vec4, kMaxLocalWindSources / 4>;

    // ParticleSimulate's SceneWind block (a wind-reading stack, VFX_READS_SCENE_WIND; WIND-FIELD-GPU): the scene
    // view's air, the one the foliage sways by.
#define DESERT_PARTICLE_SCENE_WIND_GPU_FIELDS( X )                                                                \
    /* xz the directional velocity (cm/s), w = cm/s per local unit (the local sources' reference speed) */        \
    X( glm::vec4, Directional, "SceneWindDirectional", glm::vec4( 0.0f ) )                                        \
    X( glm::vec4, Gust, "SceneWindGust", glm::vec4( 0.0f ) )   /* CameraUB::WindGust (Common/WindGust.glslh) */   \
    X( ParticleWindSourceRows, Source, "SceneWindSource", {} ) /* (world position cm, 1 / Radius) */              \
    X( ParticleWindSpeedRows, Speed, "SceneWindSpeed", {} )    /* slot i at [i / 4][i % 4], 0 = empty */
    DESERT_SHADER_STRUCT( ParticleSceneWindGpu, "ParticleSceneWind", Std430,
                          DESERT_PARTICLE_SCENE_WIND_GPU_FIELDS );
#undef DESERT_PARTICLE_SCENE_WIND_GPU_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kParticleStructs[] = {
         &kParticleStepGpuStruct,       &kParticleChannelSpawnGpuStruct,  &kParticleDrawSlotGpuStruct,
         &kParticleDrawLayoutGpuStruct, &kParticleCollisionViewGpuStruct, &kParticleSceneWindGpuStruct,
    };
    inline constexpr ShaderProtocols::ShaderConstantDecl kParticleConstants[] = {
         { .Kind  = ShaderProtocols::ShaderConstantDecl::Type::Uint,
           .Name  = "PARTICLE_ATTR_NONE",
           .Value = kParticleAttrNone },
    };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kParticleShaderInclude{
         .Path         = "Generated/VFX/Particles.glslh",
         .SourceHeader = "Desert/VFX/Source/VFX/Public/ParticleLayout.hpp",
         .Structs      = kParticleStructs,
         .Constants    = kParticleConstants,
    };
} // namespace Desert::Graphic::System
