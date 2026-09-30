#pragma once

#include <Engine/Graphic/Materials/MaterialExecutor.hpp>
#include <Engine/Graphic/InstanceWind.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Graphic/Materials/Properties/UniformBufferProperty.hpp>
#include <Engine/Graphic/Materials/Properties/FieldProperty.hpp>
#include <Engine/Graphic/Materials/Properties/TProperty.hpp>
#include <Common/Core/TemplateHelpers.hpp>

#include "MaterialInstance.hpp"

#include <unordered_set>

namespace Desert::Graphic
{
    class Material : public IPropertyOwner
    {
    public:
        explicit Material( std::string&& debugName, std::string&& shaderName );

        virtual ~Material() = default;

        MaterialInstancePtr CreateInstance( const std::string& name = "" );

        virtual const MaterialExecutor* GetMaterialExecutor() const final
        {
            return m_MaterialExecutor.get();
        }

        // THE ONLY `Bind` IN THIS HIERARCHY, and it has to stay that way. Nineteen subclasses — every
        // post-process, deferred and skybox material — used to declare their own `void Bind( image... )`
        // to point themselves at their input attachments, which is a different operation with the same
        // name: C++ then HIDES this one behind it, so `postMaterial->Bind( instance )` stops compiling
        // through the derived type while still working through a `Material*`. Nothing dispatched wrongly,
        // but the two meanings were one word and the compiler had been saying so 498 times into `-w`.
        // Those methods are now `BindInputs`; a new material that binds attachments spells it that way.
        virtual void Bind( const MaterialInstance* instance );

        // The asset's TwoSided (MaterialData::TwoSided): the renderer draws this material through the Cull None
        // permutation of the pass's pipeline (MeshRenderer::CullPermutation). An instance may override it.
        void SetTwoSided( const bool twoSided )
        {
            m_TwoSided = twoSided;
        }
        [[nodiscard]] bool IsTwoSided() const
        {
            return m_TwoSided;
        }

        // Name the row of the shared `Materials[]` storage buffer that the NEXT recorded draw reads.
        //
        // ON `Material` AND NOT ON ITS SUBCLASSES BECAUSE THERE IS ONE TRANSPORT. A PBR surface, a shader
        // graph, the terrain and the SDF text all deliver their parameters as a row indexed by a push
        // constant — the push field `MaterialIndex`, found BY NAME in the shader's reconciled MaterialLayout
        // (Graphic/Materials/MaterialBinder.hpp) — so this writes that one field for all of them, and there
        // is no second place a second offset could be written. (There used to be a second
        // transport, a uniform block per material, and it could not give two objects different values at
        // all; MaterialParamRow.hpp records what that cost and how it was measured.)
        //
        // Written STRAIGHT INTO THE PUSH BUFFER rather than stored for a later Bind: a push constant is
        // snapshotted by Vulkan when the draw is recorded, so this is already per-draw state and holding
        // it on the material would be one more thing the next object could clobber. Generic draws never
        // call Bind at all — they submit an executor — so a stored value would never be pushed for them.
        void SetMaterialIndex( uint32_t index );

        // The MATRIX half of that same push block — the slot Common/MaterialTransport.glslh declares as
        // `mat4 Transform`, written by name like the index. What the matrix MEANS belongs
        // to the drawing path, not to the material: a model matrix on the mesh path, the batcher's
        // pixel -> clip projection on the UI path (Common/UIVertex.glslh).
        //
        // It sits beside SetMaterialIndex because the two write ONE 68-byte block between them, and a
        // caller holding only the index setter has to write the other half by hand at a literal offset —
        // which is the second copy of a layout this header exists to have only one of. Renderer::RenderMesh
        // writes the same slot for the mesh path; Render2D, which submits an executor rather than a mesh,
        // had nowhere else to write it from.
        void SetPushMatrix( const glm::mat4& matrix );

        // The instanced vertex stages' wind tail (Graphic/InstanceWind.hpp): the push fields WindA/WindB.
        // Every instanced draw writes it, a still one with zeros (FO-7); a cell without them writes nothing.
        void SetInstancedWind( const InstanceWindPush& wind );

        // The SKINNED vertex path's two inputs — the vertex factory's, not the surface's, so they live here
        // on every material and not on a surface class: the packed bone palette of every skinned draw this
        // frame, in the path's `Bones` storage buffer (MeshPathOwnBinding(Skinned)), and where THIS draw's
        // bones start in it, the push field `BoneOffset`. The offset is a PUSH value, written straight into
        // the push block like the index: Vulkan snapshots it at record time, so the next draw's offset
        // cannot clobber this one before the GPU runs it. A cell without the buffer or the field (static,
        // instanced) is a caller bug and says so — a pose uploaded there would vanish without a trace.
        void UploadSkinnedBones( const glm::mat4* matrices, size_t count );
        void SetSkinnedBoneOffset( uint32_t firstBone );

        // The cell's reconciled layout (Graphic::Shader::GetMaterialLayout) — what the row, the textures and
        // the push fields are placed by (MaterialBinder). An empty layout when the shader failed to load.
        [[nodiscard]] const Core::Formats::MaterialLayout& GetMaterialLayout() const;

        // Writes one push field BY NAME through the shader's reconciled layout (MaterialBinder). False when
        // the cell does not declare the field — nothing is written then — or when `size` is not its size.
        bool WritePushField( std::string_view field, const void* value, uint32_t size );

        // Public for editor introspection (PropertyEditorBuilder reads reflected properties to build UI).
        const std::vector<IProperty*>& GetRegisteredProperties() const
        {
            return m_RegisteredProperties;
        }

        template <typename T>
        T* Get( const std::string& name ) const
        {
            if constexpr ( std::is_same_v<T, UniformBufferProperty> )
            {
                return m_MaterialExecutor->GetUniformBufferProperty( name ).get();
            }

            else if constexpr ( std::is_same_v<T, StorageBufferProperty> )
            {
                return m_MaterialExecutor->GetStorageBufferProperty( name ).get();
            }

            else if constexpr ( std::is_same_v<T, Texture2DProperty> )
            {
                return m_MaterialExecutor->GetTexture2DProperty( name ).get();
            }

            else if constexpr ( std::is_same_v<T, TextureCubeProperty> )
            {
                return m_MaterialExecutor->GetTextureCubeProperty( name ).get();
            }

            DESERT_VERIFY( false, "Unsupported MaterialProperty type" );
            return nullptr;
        }

        const std::vector<std::string>& GetPropertyNames() const
        {
            return m_PropertyNames;
        }

        /**
         * @brief Point @p sampler at its SHADER SCHEMA's own default texture — the operation "this slot
         *        is empty" consists of.
         *
         * THE MISSING HALF OF SetImage, AND WHY IT IS ON `Material`. A texture property can be pointed at
         * an image and, until М9, at nothing else: passing null left `m_Texture` null, `Apply()` skipped
         * the write, and the descriptor went on holding the LAST image assigned. So a material could be
         * given a texture and never have it taken away — clearing the slot in the editor emptied the
         * `.demat` while the surface kept drawing the old map, a file and a picture disagreeing with
         * nothing in between to notice. This is the same operation for a PBR material and for a
         * data-driven one, so it lives once, on the base both of them are.
         *
         * The default comes from the shader's `Properties … = "white"` (`ShaderParam::DefaultTexture`),
         * whose FIRST reader this is. A sampler the schema does not mention gets White, which is the
         * colour the backend's unbound-descriptor fallback already held — the picture does not move for
         * anything nobody has authored a default for.
         *
         * @return false when @p sampler is not a Texture2D of this material's shader (and then nothing is
         *         written), true when the default was bound.
         */
        bool BindSchemaDefaultTexture( const std::string& sampler );

        /// Say who holds this material AND the uniform/storage buffers its shader declares — see
        /// Engine/Graphic/ResourceLedger.hpp. `MaterialService` claims the ones built from a `.demat`; a
        /// render system's own materials stay `SceneRenderer`.
        void ClaimOwnership( ResourceOwner owner, Common::AssetHandle asset = Common::AssetHandle{} );

    protected:
        // Uploads all dirty TProperty members to the matching FieldProperty/Texture slot.
        // Exposed as protected so non-instance Bind() overrides (JFA, etc.) can flush manually.
        //
        // It used to hand back the set of uniform buffers it had touched, and all four callers threw
        // that set away -- an out-parameter nobody read, which is a dead parameter by any reading of
        // the contract. It is gone rather than plumbed: the flush below deliberately does NOT use "the
        // buffers touched this frame" (TProperty::Set skips unchanged values, so a constant parameter
        // would stop being reported and its other frame copies would go stale -- that was the outline
        // flicker), so the set had no possible consumer.
        void UploadRegisteredProperties();

        // Push every FIELD-FILLED uniform buffer that still owes a copy into that copy. The one
        // implementation of a loop that existed in four copies (here, tonemap, JFA composite, deferred
        // lighting) and had to be right about which buffers were safe to flush in each of them.
        //
        // Whole-filled buffers are skipped, and not by a list: they answer false to HasDirtyFields()
        // because their field shadow copies are not a source of truth. See
        // ShaderResources::BufferFillKind.hpp for what flushing one of them did to a frame.
        void FlushFieldFilledUniformBuffers();

        // Searches all UniformBufferProperties in the executor for a field named fieldName.
        std::pair<UniformBufferProperty*, FieldProperty*> FindFieldInAnyUB( std::string_view fieldName ) const;

    private:
        // Applies MaterialInstance overrides on top of TProperty defaults.
        void ApplyInstanceOverrides( const MaterialInstance* instance );

    protected:
        void RegisterProperty( IProperty* prop ) override;

        virtual void OnBind( MaterialInstance* /*instance*/ )
        {
        }
        void CachePropertyNames();

        std::vector<std::string>          m_PropertyNames;
        std::unique_ptr<MaterialExecutor> m_MaterialExecutor;
        std::vector<IProperty*>           m_RegisteredProperties;

    private:
        ResourceOwnership m_Accounting;
        bool              m_TwoSided = false;
    };
} // namespace Desert::Graphic