#pragma once

#include <Engine/Graphic/Shader.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBindingDecl.hpp>
#include <Engine/Graphic/RDG/RDGRecordPrerequisite.hpp>

#include <Common/Core/Memory/Buffer.hpp>

#include <Engine/Graphic/Materials/Properties/UniformBufferProperty.hpp>
#include <Engine/Graphic/Materials/Properties/StorageBufferProperty.hpp>
#include <Engine/Graphic/Materials/Properties/Texture2DProperty.hpp>
#include <Engine/Graphic/Materials/Properties/TextureCubeProperty.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Desert::Graphic
{
    class Material;

    // A pass that applies the material in its exec declares it in setup (PassBuilder::Prepares).
    class MaterialExecutor : public RDG::IRecordPrerequisite
    {
    public:
        // @p parameterSchema is the program whose `Properties` block lists this material's OWN textures
        // (Core::Formats::MaterialTextureParameters): only those become Texture2D/TextureCube properties,
        // each holding its declared default from creation. It is read here and not kept. A pass program
        // that deliberately declares no Properties (StandardSurface's Vertex_Skinned cell, the GBuffer and glass variants) is
        // given the schema of the program that owns them (StaticMeshLit) by its creator.
        // @p pushBlockSize is the shader's reflected push block (ShaderLayout::PushBlockSize), the same bytes
        // its pipeline's layout range is built with: the material holds exactly that block, never more.
        MaterialExecutor( std::string&& debugName, const std::shared_ptr<Shader>& shader,
                          const Core::Formats::ShaderProgramMeta& parameterSchema,
                          std::unique_ptr<MaterialBackend>&& materialBackend, uint32_t pushBlockSize );

        virtual ~MaterialExecutor() = default;

        const auto& GetUniformBufferProperties() const
        {
            return m_UniformBufferPropertiesLookup;
        }
        // The storage-buffer half of the lookup above. It had no accessor while the uniform-buffer one
        // did, which is the asymmetry that made a whole class of device allocation invisible to anything
        // walking a material's resources.
        const auto& GetStorageBufferProperties() const
        {
            return m_StorageBufferPropertiesLookup;
        }
        const auto& GetTexture2DProperties() const
        {
            return m_Texture2DPropertiesLookup;
        }
        const auto& GetTextureCubeProperties() const
        {
            return m_TextureCubePropertiesLookup;
        }

        const auto& GetPushConstantBuffer() const
        {
            return m_PushConstantBuffer;
        }

        const auto& GetDubugName()const
        {
            return m_DebugName;
        }

        // Writes into the shader's push block; offset lets a caller place several sub-blocks in it (the
        // per-submesh transform at offset 0 and per-object values after it). A write past the block the
        // shader declares is refused and logged (once per material): the shader has no field there, so the
        // bytes could only be pushed past the pipeline's range.
        void PushConstant( const void* buffer, uint32_t bufferSize, uint32_t offset = 0 );

        std::shared_ptr<UniformBufferProperty> GetUniformBufferProperty( const std::string& name ) const;
        std::shared_ptr<StorageBufferProperty> GetStorageBufferProperty( const std::string& name ) const;
        std::shared_ptr<Texture2DProperty>     GetTexture2DProperty( const std::string& name ) const;
        std::shared_ptr<TextureCubeProperty>   GetTextureCubeProperty( const std::string& name ) const;

        // CPU-MAT-VIEW-UB (UE: FShaderParameter resolves to an index, a draw takes no reference): the property
        // itself, owned by this executor for as long as its properties are not re-initialised - no shared_ptr
        // copy (two atomic operations) and no std::string per lookup. Null when the shader declares no such name.
        [[nodiscard]] UniformBufferProperty* FindUniformBufferProperty( std::string_view name ) const;
        [[nodiscard]] StorageBufferProperty* FindStorageBufferProperty( std::string_view name ) const;
        [[nodiscard]] Texture2DProperty*     FindTexture2DProperty( std::string_view name ) const;
        [[nodiscard]] TextureCubeProperty*   FindTextureCubeProperty( std::string_view name ) const;

        // DL-6 (UE: FShaderParameter is bound once per shader, a draw reads the bound index): the scene frame's
        // blocks, which SceneFrameBinding::ApplyTo writes into every drawn material every frame, resolved ONCE
        // where the properties are built (InitializeProperties). The property set is fixed from then on and a
        // rebuilt material is a new executor, so no pointer here can outlive its property. Null = the shader
        // declares no such block (the writers say a miss of a block the template must have, as Get did).
        struct SceneBlocks
        {
            UniformBufferProperty* Camera             = nullptr;
            UniformBufferProperty* Time               = nullptr;
            StorageBufferProperty* PointLights        = nullptr;
            StorageBufferProperty* SpotLights         = nullptr;
            UniformBufferProperty* DirectionLights    = nullptr;
            UniformBufferProperty* LightsMetadata     = nullptr;
            UniformBufferProperty* Shadow             = nullptr;
            UniformBufferProperty* SkyLook            = nullptr;
            UniformBufferProperty* CloudShadow        = nullptr;
            UniformBufferProperty* RainOcclusion      = nullptr;
            StorageBufferProperty* GpuScenePrimitives = nullptr;
            StorageBufferProperty* ObjectBones        = nullptr;
            StorageBufferProperty* WorldParameters    = nullptr;
        };
        [[nodiscard]] const SceneBlocks& GetSceneBlocks() const noexcept
        {
            return m_SceneBlocks;
        }

        // The dense property arrays, in the order the lookups index them: a walk over every block of the material
        // (the prepare, the field flush) reads these and never goes back through a name.
        [[nodiscard]] const std::vector<std::shared_ptr<UniformBufferProperty>>& GetUniformBufferStorage() const
        {
            return m_UniformBufferPropertiesStorage;
        }
        [[nodiscard]] const std::vector<std::shared_ptr<StorageBufferProperty>>& GetStorageBufferStorage() const
        {
            return m_StorageBufferPropertiesStorage;
        }

        // RENDER-THREADS M1 + M3 (UE: a pass's uniform buffers and descriptor sets are created from its parameters
        // in its setup, the lambda only binds them). Called by the graph once per declaring pass's graph, after
        // every setup and before the first pass records: makes the active view's descriptor sets and block copies
        // (MaterialBackend::PrepareViewResources), then uploads what is still pending - the owning Material's
        // dirty registered properties (MPROPERTY -> their uniform-buffer fields) and every FIELD-filled uniform
        // buffer whose active (view x frame) copy is behind its fields - and writes the descriptors. A draw that
        // submits the executor (DrawFullscreen / DrawPrimitive / a mesh path) therefore reads the values set on
        // the material in setup, Bind or no Bind (CLOUD-PREEXP-BLACK: SetExposure wrote CloudCompositeUB's
        // property, the composite never called Bind, the UB stayed zero and the clouds were black). Unclaimed and
        // whole-filled buffers are not touched (their contents are the filler's).
        [[nodiscard]] Common::BoolResultStr PrepareForRecording() const override;
        // The recording side (VulkanRendererAPI::BindGraphicsPassState / SubmitPulled): the material may be bound
        // only as its prepare left it. Refused, naming the material, when it was not prepared this frame (its
        // pass did not declare it: PassBuilder::Prepares) or when a property or registered value was written
        // after the prepare - a recording pass may not write a shared material; the write belongs in the pass's
        // setup, or travels by value with the draw (DrawPushFields).
        [[nodiscard]] Common::BoolResultStr CheckRecordable() const;
        // The Material this executor is the GPU half of (set once by Material's constructor; null for an
        // executor created on its own). Its pending properties are what PrepareForRecording uploads first.
        void SetOwner( Material* owner )
        {
            m_Owner = owner;
        }
        // RDG-FAULT1. What this material fills for a draw (the other route of a pass's binding block, declared in
        // the pass's SETUP): every property it owns except a storage buffer nothing ever wrote
        // (StorageBufferProperty:: IsWritten - the pass's setup refuses that block) - each one always holds a
        // resource, its own or its schema default (Texture2DProperty::Apply restores the default, a cube writes
        // the fallback) - and the push constants, which the executor always supplies (its buffer is allocated at
        // creation).
        // ROUTEFILL: the fill is kept and handed out shared. It is rebuilt only when what it is made of changed
        // (RouteFillInputs: which storage buffers are written, which textures are row textures or carry their
        // own sampler, whether there are push constants; the names change only in InitializeProperties), so a
        // pass's setup copies, formats and sorts no name. Safe from several declaring threads.
        [[nodiscard]] std::shared_ptr<const RDG::OtherRouteFill> GetRouteFill() const;
        std::shared_ptr<Shader> GetShader() const
        {
            return m_Shader;
        }

        const std::unique_ptr<MaterialBackend>& GetMaterialBackend() const
        {
            return m_MaterialBackend;
        }

        // A null @p parameterSchema means the shader's OWN ProgramMeta.
        static std::unique_ptr<MaterialExecutor>
        Create( std::string&& debugName, const std::string& shaderName,
                const Core::Formats::ShaderProgramMeta* parameterSchema = nullptr );
        static std::unique_ptr<MaterialExecutor>
        Create( std::string&& debugName, const std::shared_ptr<Shader>& shader,
                const Core::Formats::ShaderProgramMeta* parameterSchema = nullptr );

    protected:
        void InitializeProperties( const Core::Formats::ShaderProgramMeta& parameterSchema );

    private:
        std::string                      m_DebugName;
        std::unique_ptr<MaterialBackend> m_MaterialBackend;
        std::shared_ptr<Shader>          m_Shader;

    protected:
        template <typename T>
        using PropertyStorage = std::vector<std::shared_ptr<T>>;

        // CPU-MAT-VIEW-UB: transparent, so a Find by std::string_view hashes the parameter's name in place - no
        // std::string is built per lookup (Material::Find ran one per scene block per material per frame).
        struct PropertyNameHash
        {
            using is_transparent = void;
            size_t operator()( std::string_view name ) const noexcept
            {
                return std::hash<std::string_view>{}( name );
            }
        };

        template <typename T>
        using PropertyLookup = std::unordered_map<std::string, size_t, PropertyNameHash, std::equal_to<>>;

        Common::Memory::Buffer m_PushConstantBuffer;
        Material*              m_Owner                = nullptr;
        bool                   m_ReportedPushOverflow = false;
        // What the last PrepareForRecording applied: the absolute frame and the newest property write it saw
        // (PropertyVersion). CheckRecordable compares the material against them.
        mutable uint64_t m_PreparedFrame   = std::numeric_limits<uint64_t>::max();
        mutable uint64_t m_PreparedVersion = 0;

    private:
        // The upload and descriptor writes of the prepare (above).
        void ApplyForRecording() const;
        // Everything GetRouteFill reads besides the property names, one byte per property in the lookups' own
        // order (fixed while InitializeProperties does not run): two equal results give the same fill.
        void RouteFillInputs( std::vector<uint8_t>& inputs ) const;
        mutable std::mutex                                 m_RouteFillLock;
        mutable std::vector<uint8_t>                       m_RouteFillInputs;
        mutable std::shared_ptr<const RDG::OtherRouteFill> m_RouteFill;
        // The newest write of any property this executor owns.
        [[nodiscard]] uint64_t NewestPropertyWrite() const;

    protected:
        // Compact storage
        PropertyStorage<UniformBufferProperty> m_UniformBufferPropertiesStorage;
        PropertyStorage<StorageBufferProperty> m_StorageBufferPropertiesStorage;
        PropertyStorage<Texture2DProperty>     m_Texture2DPropertiesStorage;
        PropertyStorage<TextureCubeProperty>   m_TextureCubePropertiesStorage;

        // Fast lookup
        PropertyLookup<UniformBufferProperty> m_UniformBufferPropertiesLookup;
        PropertyLookup<StorageBufferProperty> m_StorageBufferPropertiesLookup;
        PropertyLookup<Texture2DProperty>     m_Texture2DPropertiesLookup;
        PropertyLookup<TextureCubeProperty>   m_TextureCubePropertiesLookup;

    private:
        // GetSceneBlocks: filled by ResolveSceneBlocks at the end of InitializeProperties.
        SceneBlocks m_SceneBlocks;
        void        ResolveSceneBlocks() noexcept;
    };
} // namespace Desert::Graphic