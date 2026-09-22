#pragma once

#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/MaterialOverrides.hpp>
#include <Engine/Graphic/Materials/Mesh/MeshVertexPath.hpp>
#include <Engine/Assets/MaterialAsset.hpp>
#include <Engine/Runtime/Services/Material/MaterialIdentity.hpp>

#include <array>

namespace Desert::Graphic
{
    class MaterialPBR;
}

namespace Desert::Runtime
{
    // Owns the runtime materials behind the `.demat` assets.
    //
    // A runtime material is identified by the TRIPLE (asset, vertex path, pass), not by the asset alone.
    // That is the difference between "an artist authors a surface" and "the renderer decides how the
    // geometry is fetched and what the fragment stage writes": one `.demat` legitimately becomes a static
    // material AND a skinned material AND an instanced one AND a G-buffer one, all with the same
    // parameters, because they are built from the same asset. Resolving an asset into ONE material — as
    // this service used to — makes the material's class the vertex path, and a mesh on any other path then
    // has nothing it can be drawn with. See Engine/Graphic/Materials/Mesh/MeshVertexPath.hpp for the four
    // defects that followed from it.
    //
    // THE PASS IS PART OF THE KEY BECAUSE THE DESCRIPTOR LAYOUT IS. A Graphic::Material is one shader's
    // descriptor sets plus a parameter payload, and the shader is `MeshShaderFor(path, pass)` — so a pass
    // with no material of its own has to bind a NEIGHBOURING cell's sets against its own pipeline layout,
    // which Vulkan only tolerates while the two shaders reflect to identical layouts. The deferred
    // G-buffer pass did that, and StaticMeshGBuffer.shader had to declare (and epsilon-touch) fourteen
    // bindings it never read so that the borrow stayed legal. The pass axis is what retires that.
    class MaterialService
    {
    public:
        // Eager: build the runtime Material now.
        //
        // REFUSES, naming both files, when the handle is already held by a DIFFERENT `.demat` (DC 1.4):
        // the first registration keeps the identity and the second is rejected rather than silently
        // taking it over. Before this, whichever material registered second won the service map and the
        // other could never resolve — with nothing logged to say a material had been displaced.
        Common::BoolResultStr Register( const std::shared_ptr<Assets::MaterialAsset>& materialAsset );
        // Lazy: register the asset SHELL + the external->internal map only; the runtime Material (which binds
        // its textures) is built on the first Get. Refuses a colliding identity on the same terms as
        // Register above.
        Common::BoolResultStr RegisterAsset( const std::shared_ptr<Assets::MaterialAsset>& materialAsset );

        // Builds-on-miss from a shell, for ONE (vertex path, pass) cell. A material-INSTANCE handle
        // resolves through its parent chain to the BASE material (an instance has no runtime Material of
        // its own). Returns null when the asset resolves to nothing, or when its shader has no variant in
        // that cell (a custom DSL surface shader on the skinned path, say) — MaterialFactory names which.
        //
        // The defaults are (Static, Forward) because most callers ask "does this asset resolve to a
        // material at all?" and any cell answers that; the mesh renderers pass the cell they are about to
        // draw with.
        Graphic::Material*  Get( const Assets::AssetHandle& handle,
                                 Graphic::MeshVertexPath    path = Graphic::MeshVertexPath::Static,
                                 Graphic::MeshPass          pass = Graphic::MeshPass::Forward ) const;
        Graphic::Material*  GetByExternalHandle( const Common::UUID& handle ) const;
        Assets::AssetHandle GetAssetHandleByExternal( const Common::UUID& uuid ) const;
        void                Clear();

        // The SAME `.demat`, in a different CELL — built on miss, exactly as Get does. This is how a
        // render pass that owns no material asks for one: it holds the runtime material a mesh slot
        // resolved to (static, forward), and needs the sibling whose descriptor sets were allocated from
        // ITS OWN shader's reflection.
        //
        // It exists rather than the renderer calling Get(handle, path, pass) because the renderer does not
        // have the handle: a draw carries MaterialInstance* slots, not asset handles. Answering from the
        // material keeps the asset->material table the single place that knows which `.demat` a runtime
        // material came from.
        //
        // BOTH AXES, AND THE PATH WAS THE ONE THAT WENT MISSING. This used to take the pass alone and
        // read the path off @p built, argued as "the path is not the caller's to choose: the geometry
        // already decided it when the slot resolved". The geometry decides the SLOT's path; whether the
        // draw is hardware-instanced is decided by the RENDERER, per frame, by counting how many objects
        // share a mesh — which is the definition of axis 2 in MeshVertexPath.hpp. With no way to ask for
        // the instanced sibling, MeshRenderer recorded every batch with a material of its own and every
        // batched surface lost its textures (InstancedRecorder.hpp).
        //
        // Null when the engine has no shader for the requested cell, or when @p built is not service-owned
        // — ask Owns() first if the two need telling apart, because they need different handling and a
        // caller that treats them alike either drops geometry or draws it with the wrong textures.
        Graphic::MaterialPBR* GetVariant( const Graphic::MaterialPBR* built, Graphic::MeshVertexPath path,
                                          Graphic::MeshPass pass ) const;

        // Whether this runtime material came from a `.demat` this service holds. FALSE for a material a
        // renderer built for itself — the glass pass, the RSM pass, the instanced batch material, and
        // MeshECSSystem's default material, which stands in for every mesh whose slot does not resolve.
        //
        // It exists because "GetVariant returned null" has two causes and only one of them is an
        // error. Found by rendering, not by reading: a deferred scene containing a mesh with no material
        // slot lost that mesh entirely, because its default material has no asset and so no sibling for
        // any other pass. The caller now recognises that case and draws it with a material of its own.
        bool Owns( const Graphic::Material* material ) const;

        // Every runtime material ALREADY BUILT for this handle, one per (path, pass) cell that has been
        // asked for. Live editing has to reach all of them: a parameter edit that updated only the static
        // forward material would leave the character wearing the old value — and would leave a deferred
        // scene's G-buffer material holding the previous textures, which is the same defect one axis over.
        // Never builds; a cell nobody has asked for is not returned.
        std::vector<Graphic::Material*> GetBuiltVariants( const Assets::AssetHandle& handle ) const;

        // THE way render systems obtain a slot's runtime instance. Base material asset -> a plain
        // instance of it; material-INSTANCE asset -> an instance of the parent chain's base
        // material with every level's overrides applied nearest-last (child wins). Returns null
        // when nothing resolves (caller falls back to its default material).
        // v1 note: instance assets override PARAMS only — texture overrides need per-instance
        // descriptors and are ignored by the batched path.
        //
        // The PATH is the caller's, because the caller is the one that knows what geometry it is about to
        // draw. A skinned mesh component asks for Skinned and gets an instance of the same `.demat` the
        // static twin beside it uses; before the path existed, it asked for "the" material, got the static
        // one, and the renderer dropped it.
        Graphic::MaterialInstancePtr
        CreateRuntimeInstance( const Assets::AssetHandle& handle,
                               Graphic::MeshVertexPath    path = Graphic::MeshVertexPath::Static ) const;

        // The same resolution as CreateRuntimeInstance, but delivered as NAMED VALUES instead of a runtime
        // instance — for a renderer that owns one material of its own and applies a material's parameters
        // to it by name. The terrain is that renderer and is currently the only one: its surface is a
        // single Terrain-domain program whose three splat layers are texture parameters of that program,
        // so a terrain material has no per-object Graphic::MaterialInstance to be.
        //
        // Appends base-first, child-last, so a material INSTANCE's override wins where both name a
        // parameter — the same "nearest wins" order CreateRuntimeInstance applies, and the order the
        // consumers of MaterialOverrides already assume (last write wins).
        //
        // Appends rather than returns so the caller can hand the render command the vectors it already
        // owns; this runs once per terrain entity per frame. Returns false when the handle resolves to no
        // material at all, which is what an unset slot means: the caller then leaves the shader's own
        // schema defaults in place.
        bool ResolveOverrides( const Assets::AssetHandle& handle, Graphic::MaterialOverrides& out ) const;

        // WHICH SHADER this handle's material is drawn by — the base of the instance chain's, since an
        // instance overrides values and never the program. Empty when the handle resolves to no `.demat`
        // at all, which is a DIFFERENT answer from "StaticMeshPBR": MaterialData::EffectiveShaderName()
        // substitutes that default for an absent field, and a caller that cannot tell the two apart reads
        // a dangling handle as a request for the standard mesh surface.
        //
        // It exists for the same caller ResolveOverrides exists for — one that owns its own runtime
        // material and needs the asset's values by name — except that such a caller must first know WHICH
        // program to build. The UI path is the one that does: Render2D::UIMaterialCache pairs this with
        // ResolveOverrides, and refuses by name when the program's domain is not UI.
        [[nodiscard]] std::string ShaderNameOf( const Assets::AssetHandle& handle ) const;

        // For editor live-edit of a material-instance asset: entities rebuild their cached
        // runtime instances on the next tick (same mechanism as Invalidate, no graveyard needed —
        // no runtime Material dies here).
        void BumpInvalidationVersion()
        {
            ++m_InvalidationVersion;
        }

        // Drops the built runtime Material so the next Get() rebuilds it from the asset shell.
        // Needed when the asset's SHADER changes (a different runtime material class entirely);
        // plain parameter edits use the Apply*Asset fast path instead.
        //
        // The old material is NOT destroyed here: its descriptor pools may still be referenced
        // by the command buffer being recorded / frames in flight (destroying them mid-frame
        // invalidates the command buffer -> device lost). It parks in a graveyard until
        // CollectGarbage() runs at a safe point.
        void Invalidate( const Assets::AssetHandle& handle );

        // IS ANY RUNTIME MATERIAL BUILT FOR @p handle? Asked, and not answered by calling Get() — Get
        // BUILDS on a miss, so the obvious way to test this is the one way that guarantees the answer is
        // yes. Eviction needs to know whether it is about to drop something without creating it first.
        /// PARSE THE SHELL IF IT IS NOT PARSED. Every read below that touches an asset's `Data()` calls
        /// this first, and the reason is a defect an A -> B -> A round trip found: eviction releases a
        /// material's payload and keeps its shell so the next `Get` can rebuild — and this service's lazy
        /// build read the shell WITHOUT asking whether it still held anything. It built from an emptied
        /// `MaterialData`, so the Cornell scene's green wall came back WHITE after a round trip, with
        /// nothing in the log. §1.4's empty successful answer, at the seam the whole eviction design rests
        /// on. `MeshService::Get` has always done this; this service never had to, because until now
        /// nothing released a material.
        ///
        /// `Load()` AND NOT `AssetBase::EnsureLoaded( manager )`, deliberately: this service is not handed
        /// a registry (five call sites register a material and two of them are in files this task does not
        /// own), and no `MaterialAsset` subclass overrides `ResolveDependencies`, so the two are the same
        /// call. That equality is ASSERTED rather than assumed — Desert/Tests/Engine/AssetRoots goes red
        /// the day a material type declares one, and whoever declares it must give this service a manager.
        Common::BoolResultStr EnsureLoaded( const std::shared_ptr<Assets::MaterialAsset>& asset ) const;

        [[nodiscard]] bool HasBuiltMaterial( const Assets::AssetHandle& handle ) const
        {
            return m_Materials.find( handle ) != m_Materials.end();
        }

        // IS AN ASSET SHELL ON RECORD FOR @p handle? The other half of the question above, and the two are
        // NOT interchangeable: a registered shell that nothing has drawn yet has no built material, and a
        // built material always has a shell.
        //
        // The scene parse asks THIS one, once per material reference, to decide whether it owes the
        // service a registration. It cannot ask through `Get`, which builds the runtime material on a miss
        // — the guard would then do the work it exists to avoid, which is what the resolver used to do:
        // every material a scene named was built during parsing because `!Get(handle)` was the test.
        [[nodiscard]] bool HasAsset( const Assets::AssetHandle& handle ) const
        {
            return m_MaterialAssets.find( handle ) != m_MaterialAssets.end();
        }

        // FORGET a material asset entirely — its runtime materials, its shell, and its entry in the
        // external -> internal map.
        //
        // Invalidate() above drops the built materials and KEEPS the shell, because the asset is still
        // there and the next draw has to rebuild from it. This is the other case: the asset itself stops
        // existing. The Material Editor's working copy is the asset that does — it is registered beside
        // its subject while a document is open and must leave nothing behind when the window closes, or
        // the service accumulates one dead material per material ever opened and GetAssetHandleByExternal
        // keeps answering for an id nothing holds.
        //
        // The built materials go through the graveyard exactly as Invalidate sends them, and for the same
        // reason: a frame in flight may still be executing against their descriptor pools.
        void Release( const Assets::AssetHandle& handle );

        // Destroys invalidated materials. Call at the START of a frame (before any command
        // recording); waits for the device to go idle first, so no in-flight frame can still
        // reference the dying descriptor pools. No-op (and free) when the graveyard is empty.
        void CollectGarbage();

        // Monotonic stamp, bumped on every Invalidate(). Consumers that cache raw Material* /
        // instances (the mesh components' RuntimeMaterialInstances) compare their stored stamp
        // and rebuild when it moved — dangling parent pointers become impossible without any
        // "remember to clear the instances" discipline at the Invalidate call sites.
        uint32_t GetInvalidationVersion() const
        {
            return m_InvalidationVersion;
        }

    private:
        // BOOLSUCCESS when `handle` is free, or held by the very file that is registering again. Otherwise
        // it LOGS both filepaths and returns the same text as an error, and the caller writes nothing.
        Common::BoolResultStr RefuseOnCollision( const Assets::AssetHandle&                    handle,
                                                 const std::shared_ptr<Assets::MaterialAsset>& incoming ) const;

        // One slot per (vertex path x pass) cell, built lazily. An array and not a second map because both
        // axes are closed sets the renderer enumerates — a map would let a cell exist that no draw can ask
        // for. Most cells stay empty for most assets: a scene with no skinned geometry and no deferred path
        // builds exactly one material per `.demat`, because nothing ever asks for the others.
        static constexpr size_t kVariantCount =
             static_cast<size_t>( Graphic::kMeshVertexPathCount ) * Graphic::kMeshPassCount;
        using PathVariants                    = std::array<std::shared_ptr<Graphic::Material>, kVariantCount>;

        static constexpr size_t VariantSlot( Graphic::MeshVertexPath path, Graphic::MeshPass pass )
        {
            return static_cast<size_t>( path ) * Graphic::kMeshPassCount + static_cast<size_t>( pass );
        }

        uint32_t                                                                        m_InvalidationVersion = 0;
        mutable std::unordered_map<Assets::AssetHandle, PathVariants>                   m_Materials;
        std::unordered_map<Common::UUID, Assets::AssetHandle>                           m_ExternalToInternal;
        std::unordered_map<Assets::AssetHandle, std::shared_ptr<Assets::MaterialAsset>> m_MaterialAssets;

        // Which `.demat` a built runtime material came from — the inverse of m_Materials, and the only way
        // GetVariant can answer without the caller carrying an asset handle it does not have.
        //
        // It is an INDEX and not a second source of truth: m_Materials decides which materials exist, and
        // every one of the four places that changes it (Register, the lazy build in Get, Invalidate,
        // Clear) updates this in the same statement. No test can hold it to that — building a runtime
        // material needs a device, so a headless suite cannot construct this class at all — so what keeps
        // it honest is that a stale entry is not silent: GetVariant would hand a render pass a
        // material that is on its way to the graveyard, and the pass would draw with descriptors the next
        // CollectGarbage destroys. That is why Invalidate erases here and not in CollectGarbage.
        mutable std::unordered_map<const Graphic::Material*, Assets::AssetHandle> m_BuiltToAsset;

        // Invalidated materials awaiting safe destruction (see Invalidate/CollectGarbage).
        std::vector<std::shared_ptr<Graphic::Material>> m_Graveyard;
    };
} // namespace Desert::Runtime