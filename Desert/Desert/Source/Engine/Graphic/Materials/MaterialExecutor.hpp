#pragma once

#include <Engine/Graphic/Shader.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBindingDecl.hpp>

#include <Common/Core/Memory/Buffer.hpp>

#include <Engine/Graphic/Materials/Properties/UniformBufferProperty.hpp>
#include <Engine/Graphic/Materials/Properties/StorageBufferProperty.hpp>
#include <Engine/Graphic/Materials/Properties/Texture2DProperty.hpp>
#include <Engine/Graphic/Materials/Properties/TextureCubeProperty.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic
{
    class MaterialExecutor
    {
    public:
        // @p parameterSchema is the program whose `Properties` block lists this material's OWN textures
        // (Core::Formats::MaterialTextureParameters): only those become Texture2D/TextureCube properties,
        // each holding its declared default from creation. It is read here and not kept. A pass program
        // that deliberately declares no Properties (SkinnedMeshLit, the GBuffer and glass variants) is
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

        void                    Apply() const;
        // RDG-FAULT1. What this material fills for a draw (the other route of a pass's binding block, declared in
        // the pass's SETUP): every property it owns except a storage buffer nothing ever wrote
        // (StorageBufferProperty:: IsWritten - the pass's setup refuses that block) - each one always holds a
        // resource, its own or its schema default (Texture2DProperty::Apply restores the default, a cube writes
        // the fallback) - and the push constants, which the executor always supplies (its buffer is allocated at
        // creation).
        [[nodiscard]] RDG::OtherRouteFill GetRouteFill() const;
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

        template <typename T>
        using PropertyLookup = std::unordered_map<std::string, size_t>;

        Common::Memory::Buffer m_PushConstantBuffer;
        bool                   m_ReportedPushOverflow = false;

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
    };
} // namespace Desert::Graphic