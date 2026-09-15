#pragma once

#include <Common/Core/UUID.hpp>
#include <Engine/Assets/Common.hpp>
#include <Engine/Geometry/PrimitiveType.hpp>
#include <Engine/Core/Serialize/GLMReflect.hpp>
#include <Engine/Core/Serialize/CustomReflect.hpp>

#include <rflcpp/rfl/Generic.hpp>
#include <rflcpp/rfl/ExtraFields.hpp>

#include <optional>
#include <vector>
#include <string>
#include <array>
#include <glm/glm.hpp>

namespace Desert::Assets
{
    struct VertexSer
    {
        glm::vec3 Position;
        glm::vec3 Normal;
        glm::vec2 TexCoord;
    };

    // Mesh component serialization mirrors. Meshes keep a custom (non-reflected) serializer because they
    // carry DERIVED data reflection can't express: dynamic/edited geometry (CustomVertices/CustomIndices,
    // extracted from the transient RuntimeMesh) and a std::optional primitive type. Asset references
    // (MeshPath / MaterialPaths) round-trip as paths through the shared AssetResolver — same code path the
    // reflected components use.
    // Asset references persist BOTH ways (asset-database):
    //   *Guid  — the stable asset handle (survives file renames/moves; preferred on load)
    //   *Path  — human-readable fallback + back-compat with pre-GUID scenes
    struct StaticMeshComponentSer
    {
        std::optional<std::string>                  MeshPath;
        std::optional<uint64_t>                     MeshGuid;
        std::optional<std::vector<std::string>>     MaterialPaths;
        std::optional<std::vector<uint64_t>>        MaterialGuids;
        std::optional<Geometry::PrimitiveType>      Primitive;
        std::optional<std::vector<VertexSer>>       CustomVertices;
        std::optional<std::vector<uint32_t>>        CustomIndices;
        // Rendering controls (absent = component default, so pre-existing scenes stay loadable).
        std::optional<bool>                         OutlineDraw;
        std::optional<int>                          ForcedLOD;
        std::optional<int>                          LODBias;
        std::optional<bool>                         CastShadows;
        std::optional<bool>                         ReceiveShadows;
        std::optional<uint64_t>                     HiddenSubmeshes;
    };

    struct SkinnedMeshComponentSer
    {
        std::optional<std::string>              MeshPath;
        std::optional<uint64_t>                 MeshGuid;
        std::optional<std::vector<std::string>> MaterialPaths;
        std::optional<std::vector<uint64_t>>    MaterialGuids;
        // Rendering controls (absent = component default, so pre-existing scenes stay loadable).
        // Written only when false, like the static twin's flag above.
        std::optional<bool> CastShadows;
    };

    // UE-style Instanced Static Mesh mirror: one mesh (asset path OR primitive) + N per-instance world
    // matrices, each flattened to 16 floats (column-major) so it serializes natively (no glm::mat4 reflector
    // needed in this translation unit).
    struct InstancedStaticMeshComponentSer
    {
        // GUID AND PATH, LIKE THE STATIC AND SKINNED TWINS ABOVE, AND UNTIL Г26 ONLY THE PATH.
        // The static mesh mirror has carried `MeshGuid`/`MaterialGuids` ("the stable handle itself;
        // rename-safe") since the asset database existed, and the instanced mirror -- the one the owner
        // is about to scatter grass assets through -- carried the path alone. So renaming or moving an
        // instanced mesh or its material silently unresolved every instance of it, while the identical
        // reference on a plain StaticMesh survived. Both ends of that chain looked right; the middle one
        // dropped a property. GUID first, path as the fallback, exactly as StaticMesh does it.
        std::optional<std::string>                       MeshPath;
        std::optional<uint64_t>                           MeshGuid;
        std::optional<std::vector<std::string>>          MaterialPaths;
        std::optional<std::vector<uint64_t>>              MaterialGuids;
        std::optional<Geometry::PrimitiveType>           Primitive;
        std::optional<std::vector<std::array<float, 16>>> InstanceTransforms;
    };

    // MaterialComponent (generic data-driven material) mirror. Param values reflect directly (glm::vec4 via
    // GLMReflect); texture refs round-trip as cooked paths through the AssetResolver ("TextureAsset").
    struct MaterialParamSer
    {
        std::string Name;
        glm::vec4   Value;
    };

    // ONE spelling of a texture reference, and it is the `.demat`'s: `{Name, TextureHandle}`.
    //
    // This used to be `{Name, Path, Guid}` and WROTE BOTH — a tagged path and the same reference's handle,
    // side by side, with the reader preferring Guid and silently falling back to Path. DC §4.2 forbids the
    // double write for the reason it always gives: two places holding one value drift, and the fallback is
    // what makes the drift invisible (a stale Path resolves, so the wrong texture appears rather than no
    // texture). Nothing on disk ever exercised it — the census is zero: no `.desce` in the repository
    // carries an inline MaterialComponent with a texture — so dropping Path migrated no file.
    //
    // The FIELD NAME matches MaterialData's `.demat` form on purpose. The same reference written two ways
    // by two serializers is the same defect one indirection further out.
    struct MaterialTextureSer
    {
        std::string Name;
        uint64_t    TextureHandle = 0;
    };

    struct MaterialComponentSer
    {
        std::string                                    ShaderName;
        std::optional<std::vector<MaterialParamSer>>   Params;
        std::optional<std::vector<MaterialTextureSer>> Textures;
    };

    // UIAnimComponent mirror — the authored clip. The playhead is runtime-only and deliberately absent,
    // so scrubbing in the Sequencer can never dirty a saved scene. Enums travel as ints, matching how the
    // reflected path stores them.
    struct UIAnimKeySer
    {
        float     Time   = 0.0f;
        glm::vec4 Value  = glm::vec4( 0.0f );
        int       Easing = 5;
    };
    struct UIAnimTrackSer
    {
        int                       Property = 0;
        std::vector<UIAnimKeySer> Keys;
    };
    struct UIAnimComponentSer
    {
        std::vector<UIAnimTrackSer> Tracks;
        float                       Duration = 1.0f;
        bool                        Loop     = false;
        bool                        Playing  = true;
    };

    // TextComponent mirror — only the authored fields (the glyph mesh is transient, rebuilt at runtime).
    struct TextComponentSer
    {
        std::string Text;
        // `Font` and not `FontPath`, because at scene v17 the value stopped being a path: it is the
        // root-tagged key ServiceKeyForPath mints (ComponentRegistry.cpp). The name now matches the
        // reflected `UIText.Font` slot, which carries the same form through the same two functions —
        // one thing, one spelling. A field called `...Path` is an invitation to hand it to an ifstream,
        // which is exactly how the old value came to be resolved against the wrong root.
        std::string Font;
        glm::vec4   Color             = glm::vec4( 1.0f );
        float       Size              = 1.0f;
        float       EmissiveIntensity = 1.0f;
        bool        Billboard         = false;
    };

    // AnimationComponent mirror — playback settings + the AnimGraph as JSON (the Animator + graph evaluator are
    // transient runtime objects, rebuilt from the mesh skeleton / this JSON). GraphJson is empty when the
    // entity has no state machine.
    struct AnimationComponentSer
    {
        std::string CurrentClip;
        bool        Playing          = true;
        bool        Loop             = true;
        float       PlaybackSpeed    = 1.0f;
        std::string GraphJson;
    };

    // NOTE: camera/light/skybox payloads are no longer mirrored here — they serialize generically through
    // the reflection registry (ComponentRegistry + ReflectionSerializer + AssetResolver). Only the mesh
    // mirrors above remain (derived geometry isn't reflectable).

    // ONE ENTITY'S DEVIATION FROM THE PREFAB IT CAME FROM — the thing that makes an instance an
    // instance rather than a copy.
    //
    // WHY IT EXISTS AT ALL. Before this, a prefab instance survived a `.desce` round trip as its ROOT'S
    // TRANSFORM AND NOTHING ELSE: SceneSerializer skips every entity under a PrefabComponent when it
    // writes (so no child was ever recorded), and on load it applied only Translation/Rotation/Scale to
    // the instantiated root (so even the root's own component payloads, which WERE written, were
    // discarded). Recolour a button inside an instance, save, reload, and the colour is gone with no
    // error anywhere. That is the exact failure the prefab concept exists to prevent, and it was silent.
    //
    // WHY A DIFF AND NOT A COPY OF THE INSTANCE. A copy would pin every field, so a later edit to the
    // source prefab would reach no instance that had ever been touched — which is copy-paste wearing a
    // prefab's name. Only the keys that DIFFER are recorded, so everything else keeps following the
    // source. That is the whole difference, and it is why the capture is a comparison rather than a
    // serialization.
    //
    // ADDRESSING: `Path` is the chain of RECORD ids from the outermost prefab file down to this entity's
    // own record — one id for an entity of the instance itself, two for an entity inside a prefab nested
    // one level down, and so on. Record ids live in the `.deprefab` and do not change when an instance is
    // created, which is what makes an override survive a reload; the entity UUIDs cannot be used because
    // PrefabFactory mints fresh ones on every instantiation by design.
    struct PrefabOverrideData
    {
        std::vector<Common::UUID> Path;

        std::optional<std::string> Tag;

        std::optional<glm::vec3> Translation;
        std::optional<glm::vec3> Rotation;
        std::optional<glm::vec3> Scale;

        // Only the component keys whose payload differs from the base record's. Spread at this record's
        // top level by ExtraFields, exactly as EntityData spreads its own components.
        rfl::ExtraFields<rfl::Generic> Components;
    };

    struct EntityData
    {
        std::optional<Common::UUID> id;
        std::optional<Common::UUID> parent;

        std::optional<std::string> PrefabPath;

        std::optional<std::string> Tag;

        // Transform
        std::optional<glm::vec3>   Translation;
        std::optional<glm::vec3>   Rotation;
        std::optional<glm::vec3>   Scale;

        // Component payloads keyed by ComponentRegistry key (e.g. "StaticMesh", "DirectionLight").
        // ExtraFields spreads these at the entity's top level on write and captures top-level component
        // keys on read — so the on-disk shape matches the original per-component layout (full back/forward
        // compatibility). Reflected blocks are filled by ReflectionSerializer; asset-bearing ones by
        // custom handlers in ComponentRegistry.
        rfl::ExtraFields<rfl::Generic> Components;

        // Set ONLY on a record that carries a PrefabPath: how this instance differs from the file it
        // names. Absent means "identical to the source", which is what every instance is at birth.
        //
        // It is a NAMED field and not another component key so that the diff is addressable as data —
        // and because ExtraFields would otherwise hand it to ComponentRegistry, which has no such key
        // and would drop it.
        std::optional<std::vector<PrefabOverrideData>> PrefabOverrides;
    };

    struct PrefabData
    {
        std::string             Name;
        std::vector<EntityData> Entities;
        Common::UUID            Root;

        // THE SAME TWO GENERATION INTEGERS A .desce CARRIES, deliberately not a third numbering scheme: a
        // prefab's payload is the scene's own EntityData, written by the same ComponentRegistry, so a
        // schema step that moves Core::kSceneVersion moves this file's format with it whether anyone
        // remembered prefabs or not. Before Д28 nothing here said which generation a .deprefab was — a
        // format change broke prefabs silently and the user's load was where it surfaced (the crash Ф1
        // fixed was this class of defect). The saver stamps both (WritePrefabJson), the loader requires
        // both (ParseLoadablePrefab in PrefabFormat.hpp), and Tools/SceneMigrator converts anything else.
        //
        // Optional so an OLD file still PARSES - into a tree the gate then refuses BY NAME instead of a
        // read error. Absent = version 0, not "current" (see PrefabIsAtCurrentVersion).
        std::optional<int> SceneVersion;
        std::optional<int> UnitVersion;
    };
} // namespace Desert::Assets