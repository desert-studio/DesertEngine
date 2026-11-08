#pragma once

#include "MaterialProperty.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Graphic
{
    class Material;
    class MaterialInstance;

    using MaterialInstancePtr = std::shared_ptr<MaterialInstance>;
    using MaterialPtr         = std::shared_ptr<Material>;

    // ONE ENTITY'S MATERIAL SLOTS, AS THE RENDER PATH IS ALLOWED TO HOLD THEM.
    //
    // WHY THIS TYPE EXISTS AT ALL — A8-3. The mesh path used to carry
    // `const std::vector<MaterialInstance*>*`: the ADDRESS OF A VECTOR MEMBER OF AN ECS COMPONENT. Both
    // halves of that were unsound, and the second half is the one that a copy alone would not have fixed:
    //
    //   * the vendored entt keeps components BY VALUE in a flat `std::vector`, so `AddComponent` moves
    //     every component already in the pool and `DestroyEntity` swap-and-pops another one over the hole
    //     (asserted, not argued, by PointerOwnership.EnttComponentAddressesAreNotStable);
    //   * and the MaterialInstances themselves are owned by that same component's
    //     `RuntimeMaterialInstances`, so destroying the entity destroys them too.
    //
    // Between the two there is a real window and it is not a narrow one: `MeshECSSystem` RECORDS the draw,
    // `ScriptSystem` — registered after it — then runs USER LUA (`World.spawnMarker` adds a component,
    // `entity:destroy()` removes one), and only afterwards does `RenderCommandBuffer::ExecuteAll` read what
    // was recorded, with five more passes reading it after that. The path from "took the address" to "read
    // it" runs through code this project does not write.
    //
    // Ownership question 1 — who is obliged to destroy this? — has two answers here whose order is not
    // fixed: the component that authored the slots, and any draw still in flight. Two owners with unordered
    // deaths is precisely what a `shared_ptr` is for, so the render path CO-OWNS the binding instead of
    // pointing at the component's storage. The cost is one atomic increment per mesh per frame, not the
    // per-frame allocate-and-copy of a slot vector that the raw pointer was introduced to avoid (measured
    // at ~0.9 ms of ExecuteAll for 256 meshes in Debug, which is why the naive fix is the wrong one).
    struct MaterialSlotBinding
    {
        // Keeps every instance alive for as long as ANY holder of this binding lives. The render path never
        // reads this vector; it exists to make the raw view below safe to read.
        std::vector<MaterialInstancePtr> Owned;

        // What the render path reads. Parallel to Owned, and rebuilt with it — never separately.
        std::vector<MaterialInstance*> Slots;

        // The same slots for a draw over DISPLACED geometry (MaterialService displaced siblings: base cells, no
        // parallax march - CONTRACT-DISPLACE §1a). Built with this binding when the mesh is displaced
        // (Mesh::IsDisplaced), null otherwise; MeshRenderer::ApplyDisplacement swaps it in with the twin.
        std::shared_ptr<const MaterialSlotBinding> Displaced;
    };

    using MaterialSlotBindingPtr = std::shared_ptr<const MaterialSlotBinding>;

    class MaterialInstance : public std::enable_shared_from_this<MaterialInstance>
    {
    public:
        MaterialInstance( Material* parentMaterial, const std::string& name = "" );
        // Out of line: an instance whose write waits for its publish leaves the pending list (any thread: the
        // last owner may be a draw's MaterialSlotBinding released by the render task).
        virtual ~MaterialInstance();

        // Getters with type safety
        float     GetFloat( const std::string& name, float defaultValue = 0.0f ) const;
        int       GetInt( const std::string& name, int defaultValue = 0 ) const;
        bool      GetBool( const std::string& name, bool defaultValue = false ) const;
        glm::vec2 GetVec2( const std::string& name, const glm::vec2& defaultValue = glm::vec2( 0.0f ) ) const;
        glm::vec3 GetVec3( const std::string& name, const glm::vec3& defaultValue = glm::vec3( 0.0f ) ) const;
        glm::vec4 GetVec4( const std::string& name, const glm::vec4& defaultValue = glm::vec4( 0.0f ) ) const;
        glm::mat4 GetMat4( const std::string& name, const glm::mat4& defaultValue = glm::mat4( 1.0f ) ) const;
        void*     GetTexture( const std::string& name ) const;

        // Setters with dirty tracking
        void SetFloat( const std::string& name, float value );
        void SetInt( const std::string& name, int value );
        void SetBool( const std::string& name, bool value );
        void SetVec2( const std::string& name, const glm::vec2& value );
        void SetVec3( const std::string& name, const glm::vec3& value );
        void SetVec4( const std::string& name, const glm::vec4& value );
        void SetMat4( const std::string& name, const glm::mat4& value );
        void SetTexture( const std::string& name, void* texture );

        // Generic by-name write from a packed vec4, dispatched on the param's OWN reflected type (float reads
        // .x, vec2 .xy, vec3 .xyz, vec4 all). This is the data-driven counterpart to the typed setters: apply a
        // MaterialComponent's (name, vec4) overrides with no per-name if-chain — the shader schema decides the
        // type. Mirrors DataDrivenMaterial::SetParamRaw for the instance path. Returns false if `name` is unknown
        // or its type isn't vec4-packable (mat4/texture). See MaterialProperty for the type enum.
        bool SetParamFromVec4( const std::string& name, const glm::vec4& value );

        // The read counterpart: THIS instance's own override of `name`, packed into a vec4 the way
        // SetParamFromVec4 unpacks it (float/int/bool in .x, vec2 .xy, vec3 .xyz); nullopt when the instance does
        // not override it (the parent's / material's value shows) or the value is not vec4-packable.
        std::optional<glm::vec4> GetOverrideAsVec4( const std::string& name ) const;
        // Drop this instance's own override of `name` — the parent's / material's value shows again.
        void ClearOverride( const std::string& name );

        // Batch operations
        void SetParameters( const std::vector<std::pair<std::string, MaterialPropertyValue>>& params );
        void SetParameters( const MaterialPropertySet& properties );

        // Instance management
        MaterialInstancePtr CreateChildInstance( const std::string& name = "" );
        Material*           GetParentMaterial() const
        {
            return m_ParentMaterial;
        }
        // TwoSided of this instance: its own override (a `.demat` instance that sets MaterialData::TwoSided), else
        // its parent instance's, else the base material's. Nothing = inherit.
        void SetTwoSidedOverride( std::optional<bool> twoSided ) noexcept;
        // THE RENDER-COPY PUBLISH EPOCH (UE: a MaterialInstance parameter change invalidates the cached mesh draw
        // commands that use it). Moves with every write of any instance's render copy (Publish, a child's first
        // copy) and every two-sided override, never otherwise: a render-side cache of per-instance answers
        // (MeshRenderer::StaticPlanRows: HasOverrides, IsTwoSided) holds while it stands.
        [[nodiscard]] static uint64_t RenderPublishGeneration() noexcept;
        [[nodiscard]] bool  IsTwoSided() const;
        MaterialInstancePtr GetParentInstance() const
        {
            return m_ParentInstance.lock();
        }
        const std::vector<MaterialInstancePtr>& GetChildInstances() const
        {
            return m_ChildInstances;
        }

        // Property access. HasParameter, the typed getters and GetOverrideAsVec4 read the GAME copy (what the
        // setters wrote); GetPropertySet and GetParameterRevision read the RENDER copy.
        bool HasParameter( const std::string& name ) const;

        // THE RENDER COPY (UE FMaterialInstanceResource).
        // The game thread writes m_Properties; the views read m_RenderProperties, which only a publish writes.
        // A write while no render job is in flight (serial mode, the replay after a view's Join, a preview, a
        // bake) or from the render task itself publishes at once: today's order. A write while a job is in flight
        // (the tick of frame N+1 against frame N's views: a script, a sequencer track, an editor panel) joins
        // nothing: the instance goes on the pending list and PublishPending publishes it at the next hand-off
        // (Scene::OnUpdate after its Join, SceneRenderer::BeginScene after its Join), before any view reads it.
        // So a view records with the values of the last write before ITS hand-off, as in serial mode.
        //
        // RENDER SIDE: the views (Material::ApplyInstanceOverrides, MeshRenderer, MeshRendererForward) and the
        // replay after a view's Join. Never read this from the tick: it lags the setters by up to one hand-off.
        const MaterialPropertySet& GetPropertySet() const
        {
            return m_RenderProperties;
        }
        // The revision of the render copy (MaterialPropertySet::GetRevision): every publish takes a fresh one
        // (CopyFrom), so a row derived from it is re-derived exactly when the values the views read moved.
        [[nodiscard]] uint64_t GetParameterRevision() const
        {
            return m_RenderProperties.GetRevision();
        }
        // GAME THREAD, at a hand-off: after a Join, before the next view is enqueued. Publishes every instance
        // written while a job was in flight. Returns at once while a job is in flight (a view recorded inside a
        // job: the game thread may be writing the game copies), the next hand-off on the game thread publishes.
        static void PublishPending();

        // Drops every instance override so the instance renders with the parent material's values
        // again — the inverse of SetParamFromVec4. NOT what a script's clearMaterialParams() does: that drops the
        // mesh's slot instances so they rebuild, keeping a material-instance asset's own overrides.
        void ResetOverrides()
        {
            m_Properties.ResetToDefaults();
            ParametersWritten();
        }

        // GPU update. RENDER SIDE: m_bNeedsApply is set by a publish and cleared here.
        void Apply();
        bool NeedsApply() const
        {
            return m_bNeedsApply;
        }

        // Debug
        const std::string& GetName() const
        {
            return m_Name;
        }
        void SetName( const std::string& name )
        {
            m_Name = name;
        }

    private:
        // After every write of m_Properties: publishes now, or puts this instance on the pending list while a
        // render job is in flight (see THE RENDER COPY).
        void ParametersWritten();
        // m_Properties -> m_RenderProperties; marks this instance and its children for Apply. Only where no job
        // reads the render copies, or on the render task itself.
        void                  Publish();
        MaterialPropertyValue ResolveProperty( const std::string& name ) const;

        Material*                        m_ParentMaterial;
        std::string                      m_Name;
        MaterialPropertySet              m_Properties;       // GAME copy: the setters write it
        MaterialPropertySet              m_RenderProperties; // RENDER copy: a publish writes it, the views read it
        std::weak_ptr<MaterialInstance>  m_ParentInstance;
        std::optional<bool>              m_TwoSidedOverride;
        std::vector<MaterialInstancePtr> m_ChildInstances;
        bool                             m_bNeedsApply    = true;  // RENDER side (Publish sets, Apply clears)
        bool                             m_bRenderPending = false; // on the pending list (under its mutex)
    };
} // namespace Desert::Graphic