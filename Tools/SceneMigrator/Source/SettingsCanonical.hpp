#pragma once

// THE ONE PART OF THIS TOOL THAT NEEDS THE ENGINE'S REFLECTION TABLE, AND WHY IT IS ITS OWN FILE.
//
// Every step in SceneMigration.hpp is a pure function over the parsed tree and nothing else — which is
// what lets sixteen suites compile that one translation unit and test a step each without linking an
// engine. Canonicalisation cannot be that: "canonical" means "the bytes the engine's saver writes", so
// it has to enumerate the same fields through the same table and the same serializer, and a hand-written
// field list here would be a second statement of the format that could silently fork.
//
// Putting it beside the steps would have dragged Reflection.gen.cpp into all sixteen of those suites —
// three files and a full engine header sweep each, for a symbol fifteen of them never call. Hence the
// split: SceneMigration.cpp stays registry-free, and the two things that genuinely need the table (this
// file and the tool's main) carry it.

#include <optional>
#include <string>

#include <rflcpp/rfl/Generic.hpp>

namespace Desert::Migration
{
    struct SceneSerialized;

    // What canonicalising one reflected block did.
    struct BlockCanonicalisationReport
    {
        bool BlockCreated   = false; // the scene stated no block at all (Settings only)
        int  KeysAdded      = 0;     // fields this build knows that the file did not state
        int  ValuesRestated = 0;     // fields whose TEXT changed without their value changing (see below)
        bool Rewritten = false; // the block's text changed (created, keys added, values restated or reordered)
        bool Refused   = false; // the block is left untouched; the reason was logged / is in RefusedWhy
        std::string RefusedWhy;
    };

    // Rewrites ONE reflected block into exactly the bytes the ENGINE'S SAVER would write for the values the
    // file states: read into a default-constructed `type` through DeserializeReflected (what the loader does),
    // written back through SerializeReflected (what the saver does).
    //
    // WHY A CONVERSION IS NEEDED FOR SOMETHING THAT CHANGES NO VALUE. K11's relation is "a file read and
    // written back with no change is byte-identical to the source", and it is false for a block for two
    // reasons that have nothing to do with foreign keys:
    //
    //   * a file states only the fields that existed when it was last saved, and the saver writes all of
    //     them, so the first save of any older block ADDS keys (SkyAtmosphere +25, DirectionLight +8 on
    //     Starter when this was written);
    //   * a float field hand-edited to `0.26` cannot survive a narrowing to `float` and back, and comes
    //     out as `0.2599999904632568`; a value written wide (`80.0000011920929`) comes back as `80.0`. The
    //     VALUE is identical either way; the TEXT is not, and byte-identity is a claim about text.
    //
    // Both are settled in the FILES, once, by the migrator's canonical pass - not by every save of every
    // build for ever. No version moves: the document means what it meant (the loader reads a missing key
    // as the same C++ default this pass writes), exactly like a re-layout.
    //
    // ASSET HANDLES ARE KEPT VERBATIM. A reflected AssetHandle is written as a PATH when the saver has an
    // asset resolver and as a raw integer when it does not; this tool has no AssetManager and must not
    // invent one, so a stated AssetHandle field keeps exactly the text the file carried; an unstated one
    // that reads back UNSET gets the saver's spelling of "no asset"; an unstated one that reads back SET is
    // a refusal (the tool cannot name the asset).
    //
    // A KEY THIS BUILD DOES NOT DECLARE is refused, not dropped and not re-placed: the saver keeps such a
    // key where it stood (Serialize/ForeignKeys.hpp), and this pass would have to restate that merge to
    // agree with it byte for byte.
    //
    // PURE apart from the process-wide reflection table it reads, which is const after static init.
    // Idempotent: a block that is already canonical is left byte-identical and reports zero.
    // `where` names the block in a refusal ("Settings", "entity 123 / SkyAtmosphere").
    BlockCanonicalisationReport CanonicaliseReflectedBlock( const char*                  typeName,
                                                            std::optional<rfl::Generic>& block,
                                                            const std::string&           where );

    // The scene's Settings block (SceneSettings). A scene with no block gains the saver's.
    BlockCanonicalisationReport CanonicaliseSettings( std::optional<rfl::Generic>& settings );

    // What the canonical pass did to one scene.
    struct SceneCanonicalisationReport
    {
        int                BlocksRestated = 0; // blocks whose text changed (Settings included)
        int                KeysAdded      = 0;
        int                ValuesRestated = 0;
        std::string        Refused; // non-empty: the first refused block, named; the scene is unchanged
        [[nodiscard]] bool Changed() const
        {
            return BlocksRestated > 0;
        }
    };

    // THE MIGRATOR'S CANONICAL PASS: Settings and every entity-record block listed in
    // Engine/Core/Serialize/ReflectedComponentBlocks.hpp, each through CanonicaliseReflectedBlock.
    //
    // NOT COVERED - blocks whose file form is not their reflection alone, written by hand-written
    // serializers in ComponentRegistry.cpp: StaticMesh, SkinnedMesh, InstancedStaticMesh, the material
    // slots, Script, Landscape / LandscapeTile, Foliage, AnimGraph, CubeGridBlockout, UIRenderTexture,
    // the authored components (Locomotion, Morph, SocketAttachment, Projectile) and the flags. Nor prefab
    // override records, whose blocks are diffs against the prefab. A save of any of those is only as
    // stable as its hand-written writer.
    //
    // All-or-nothing: a refusal anywhere leaves the whole scene exactly as it was read.
    SceneCanonicalisationReport CanonicaliseScene( SceneSerialized& scene );
} // namespace Desert::Migration
