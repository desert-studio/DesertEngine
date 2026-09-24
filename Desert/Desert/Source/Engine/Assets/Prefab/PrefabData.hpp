#pragma once

#include <Common/Content/TextAssetHeader.hpp>

#include <Common/Core/UUID.hpp>
#include <Engine/Assets/Common.hpp>
#include <Engine/Geometry/EditMeshSerialization.hpp>
#include <Engine/Geometry/PrimitiveType.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>

#include <rflcpp/rfl/Generic.hpp>
#include <rflcpp/rfl/ExtraFields.hpp>

#include <optional>
#include <vector>
#include <string>
#include <array>
#include <glm/glm.hpp>

namespace Desert::Assets
{
    // Mesh component serialization mirrors. Meshes keep a custom (non-reflected) serializer because they
    // carry data reflection can't express: a mesh built in the editor (the component's EditMesh, stored as
    // its own saved form - Engine/Geometry/EditMeshSerialization.hpp) and a std::optional primitive type. Asset
    // references resolve through the shared AssetResolver — same code path the reflected components use. An
    // asset reference persists BOTH ways:
    //   *Guid  — the asset's header GUID as text (meshes SCNE 28, materials SCNE 27): the identity
    //   *Path  — a locator only: loads the asset when it is not resident, and must hold that GUID's asset
    struct StaticMeshComponentSer
    {
        std::optional<std::string>                  MeshPath;
        std::optional<std::string>                  MeshGuid; // mesh header GUID text (SCNE 28)
        std::optional<std::vector<std::string>>     MaterialPaths;
        std::optional<std::vector<std::string>>     MaterialGuids; // material header GUID text (SCNE 27)
        std::optional<Geometry::PrimitiveType>      Primitive;
        // The editor-built mesh, the SOURCE the render mesh is derived from. Schema v22 replaced the v21
        // CustomVertices/CustomIndices render arrays with it (Tools/SceneMigrator, MigrateEditMeshV21ToV22).
        std::optional<Geometry::EditMeshSer> EditMesh;
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
        std::optional<std::string>              MeshGuid; // mesh header GUID text (SCNE 28)
        std::optional<std::vector<std::string>> MaterialPaths;
        std::optional<std::vector<std::string>> MaterialGuids; // material header GUID text (SCNE 27)
        // Rendering controls (absent = component default, so pre-existing scenes stay loadable).
        // Written only when false, like the static twin's flag above.
        std::optional<bool> CastShadows;
    };

    // UE-style Instanced Static Mesh mirror: one mesh (asset path OR primitive) + N per-instance world
    // matrices, each flattened to 16 floats (column-major) so it serializes natively (no glm::mat4 reflector
    // needed in this translation unit).
    struct InstancedStaticMeshComponentSer
    {
        // GUID AND PATH, LIKE THE STATIC AND SKINNED TWINS ABOVE, AND UNTIL Г26 ONLY THE PATH: the instanced
        // mirror carried the path alone, so renaming an instanced mesh or its material silently unresolved
        // every instance while the same reference on a plain StaticMesh survived. It names its assets
        // exactly as StaticMesh does.
        std::optional<std::string>                       MeshPath;
        std::optional<std::string>                        MeshGuid; // mesh header GUID text (SCNE 28)
        std::optional<std::vector<std::string>>          MaterialPaths;
        std::optional<std::vector<std::string>>           MaterialGuids; // material header GUID text (SCNE 27)
        std::optional<Geometry::PrimitiveType>           Primitive;
        std::optional<std::vector<std::array<float, 16>>> InstanceTransforms;
        // Absent = component default (true), and written only when false — the same shape the static
        // and skinned mirrors use, so no existing `.desce` changes a byte and no schema version moves.
        std::optional<bool> CastShadows;
    };

    // MaterialComponent (generic data-driven material) mirror. Param values reflect directly (glm::vec4 via
    // GlmReflection.hpp); texture refs round-trip as cooked paths through the AssetResolver ("TextureAsset").
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

    // AnimationComponent mirror — playback settings plus the `.danimgraph` this entity plays (the Animator
    // and the graph evaluator are transient runtime objects, rebuilt from the mesh skeleton and from that
    // file).
    //
    // `Graph` IS A PATH AND `GraphJson` IS GONE. The blob carried the whole state machine inside every
    // entity that used one, which is what stopped two characters sharing a walk graph; schema step 21
    // (`kSceneVersionAnimGraphAsset`) extracted the six blobs in this repository into files and put their
    // relative paths here. RELATIVE, on exactly the terms `ControlRigData::Rig` is relative: a graph is
    // content that ships WITH the project, and an absolute path would carry one developer's home
    // directory into every scene that names one.
    //
    // `std::optional`, so "this entity has no state machine" is the ABSENCE of a key rather than an empty
    // string that a reader then has to agree means the same thing.
    struct AnimationComponentSer
    {
        std::string                CurrentClip;
        bool                       Playing       = true;
        bool                       Loop          = true;
        float                      PlaybackSpeed = 1.0f;
        std::optional<std::string> Graph;
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

        // Where this entity stands among its siblings (same `parent`, or the scene's roots when absent).
        // Since scene v25 a .desce lists its records sorted by id, so the file order no longer says
        // anything and a child's place under its parent has to be stated. It is a SORT KEY, not a dense
        // slot: the loader orders siblings by it, ties falling back to file order. Written by the scene
        // saver only; a .deprefab keeps its records in hierarchy order and does not state it.
        std::optional<uint32_t> siblingIndex;

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
        // First member: the header (Common/Content/TextAssetHeader.hpp) - the prefab's GUID and the two
        // generations it states (SCNE, UNIT); since scene v26 nowhere else.
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;
        std::string                                               Name;
        std::vector<EntityData>                                   Entities;
        Common::UUID                                              Root;
    };
} // namespace Desert::Assets