#pragma once

#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/MaterialBinder.hpp>
#include <Engine/Graphic/Materials/MaterialOverrides.hpp>
#include <Engine/Graphic/Materials/MaterialParamHandle.hpp>
#include <Engine/Graphic/Materials/Mesh/MeshVertexPath.hpp>
#include <Engine/Core/Formats/MaterialParamRow.hpp>
#include <Engine/Core/Formats/ShaderProgramMeta.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Graphic/Shader.hpp>

#include <Engine/Graphic/Materials/MaterialProperty.hpp> // NextMaterialRevision

#include <glm/glm.hpp>

#include <cstring>
#include <mutex>
#include <vector>

namespace Desert::Graphic
{
    class Image2D;

    // Generic, data-driven material built from ANY shader by name — no per-shader C++ class. Parameters
    // come from the shader's `Properties` schema (UI metadata + defaults), and the cell's MaterialLayout
    // (MaterialBinder) says where each lands in the row the shader reads. This is what makes
    // arbitrary shaders assignable with dynamic params.
    //
    // WHAT THIS CLASS NO LONGER IS. It used to write its parameters into a per-material `uniform
    // MaterialUB` block by reflected field name. A block IS the parameters, so the material held exactly
    // one set of values — and the renderer keys ONE material per shader, so several objects drawn with
    // one graph shader all rendered the values of whichever draw wrote last. It now holds a ROW instead,
    // and the renderer packs every draw's row into one `Materials[]` storage buffer and names each draw's
    // row with a push constant, exactly as the old lit class did. See
    // Engine/Core/Formats/MaterialParamRow.hpp for the measurement, the probe scene and the layout rule.
    //
    // A CONSEQUENCE WORTH STATING: the row is plain CPU memory, so none of the frame-in-flight machinery
    // that guarded the old block applies to it. The renderer uploads every row of a draw group before
    // recording the group's first draw, every frame — there is no value that was written once and owes
    // itself to a copy nobody served, which is what `FlushParameterBuffers` existed to pay for.
    class DataDrivenMaterial : public Material
    {
    public:
        explicit DataDrivenMaterial( const std::string& shaderName )
             : Material( "DDM_" + shaderName, std::string( shaderName ) ), m_ShaderName( shaderName )
        {
            if ( auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( shaderName ) )
                m_Schema = shader->GetProgramMeta();

            ApplyDefaults();
        }

        // Over one compiled permutation of a program (ShaderService::AcquireVariant): its layout is the
        // permutation's own, so the material fills exactly the bindings that permutation declares.
        explicit DataDrivenMaterial( const std::shared_ptr<Shader>& shader )
             : Material( "DDM_" + shader->GetName(), shader ), m_ShaderName( shader->GetName() ),
               m_Schema( shader->GetProgramMeta() )
        {
            ApplyDefaults();
        }

        const Core::Formats::ShaderProgramMeta& GetSchema() const
        {
            return m_Schema;
        }
        const std::string& GetShaderName() const
        {
            return m_ShaderName;
        }
        /// MeshCellPath( GetShaderName() ), classified once at construction: the shader name never changes
        /// after it, and the mesh queues ask this per slot per frame (UE: the vertex factory a material
        /// was compiled for, known to the proxy without re-deriving it).
        [[nodiscard]] const std::optional<MeshVertexPath>& GetVertexPath() const noexcept
        {
            return m_VertexPath;
        }

        [[nodiscard]] DataDrivenMaterial* AsDataDriven() noexcept override
        {
            return this;
        }
        [[nodiscard]] const DataDrivenMaterial* AsDataDriven() const noexcept override
        {
            return this;
        }

        // The bytes one draw reads, in schema order. The renderer copies this into the shared
        // `Materials[]` buffer at the row it then names on the push constant.
        //
        // The texture slots are written HERE, from each row texture's property, at the moment the row is copied:
        // the table slot of the image the property draws now (the schema default while nothing is assigned or
        // the texture is still Pending) and the sampler of its slot state. A texture assigned, a default restored
        // or a sampler state changed (MaterialService writes the property directly) reaches the next copy with no
        // write path of its own to forget.
        const Core::Formats::MaterialParamRow& GetParamRow() const
        {
            WriteTextureSlots();
            return m_Row;
        }

        // The revision of GetParamRow's bytes: moves with every value written (SetParam, ApplyDefaults) and with
        // every texture slot that reads differently now (the slots are pulled from the texture properties, so
        // they are refreshed here first, as GetParamRow does), at no other time. Process-unique
        // (NextMaterialRevision). MeshRenderer::RegisterMaterialRow rebuilds a row only when it moved.
        [[nodiscard]] uint64_t GetParamRowRevision() const
        {
            WriteTextureSlots();
            return m_RowRevision;
        }

        // GetParamRow and GetParamRowRevision with ONE texture-slot refresh: the row's bytes and the revision they
        // are at, for a consumer that caches what it derives from the row by that revision (TerrainRenderer's
        // row values) and must not refresh the slots twice per draw.
        struct ParamRowView
        {
            const Core::Formats::MaterialParamRow* Row      = nullptr;
            uint64_t                               Revision = 0;
        };
        [[nodiscard]] ParamRowView GetParamRowView() const
        {
            WriteTextureSlots();
            return { &m_Row, m_RowRevision };
        }

        // Write a scalar/vector param by name. The whole slot is written whatever the parameter's
        // declared width: the components past it are the generated struct's own padding, so there is
        // nothing there to damage, and the alternative — a per-type byte count — is a second statement of
        // a layout that the cell's MaterialLayout deliberately has only one of.
        bool SetParam( const std::string& name, const glm::vec4& value )
        {
            const bool written = MaterialBinder::WriteRowParam( GetMaterialLayout(), m_Row, name, value );
            if ( written )
                m_RowRevision = NextMaterialRevision();
            return written;
        }

        // THE HANDLE ROUTE (UE: FMaterialRenderProxy's uniform expression cache resolves a parameter's offset once
        // per shader map, a draw writes by index). The stamp of the layout this material writes through now: the
        // program's serial and code generation (MaterialLayoutStamp), {0, 0} without a program.
        [[nodiscard]] MaterialLayoutStamp GetLayoutStamp() const noexcept
        {
            const Shader* shader = m_MaterialExecutor ? m_MaterialExecutor->GetShader().get() : nullptr;
            if ( shader == nullptr )
                return {};
            return { .Serial = shader->GetSerial(), .Generation = shader->GetCodeGeneration() };
        }

        // @p name's row slot in the current layout; IsValue() false when the row has no such numeric parameter
        // (a texture slot is written from the material's texture, never as a value - WriteRowParam's rule).
        [[nodiscard]] MaterialParamHandle ResolveParam( std::string_view name ) const
        {
            MaterialParamHandle handle;
            handle.Stamp       = GetLayoutStamp();
            const auto& params = GetMaterialLayout().Params;
            for ( size_t index = 0; index < params.size(); ++index )
            {
                const auto& p = params[index];
                if ( p.Name != name )
                    continue;
                const uint32_t slot = p.Offset / Core::Formats::kMaterialParamSlotSize;
                if ( p.IsTexture || slot >= m_Row.size() )
                    break;
                handle.Slot  = slot;
                handle.Param = static_cast<uint32_t>( index );
                break;
            }
            return handle;
        }

        // SetParam through a handle the caller keeps: re-resolved by @p name only when the layout's stamp moved
        // (a hot reload bumps the generation, another program has another serial), else written by slot.
        bool SetParam( MaterialParamHandle& handle, std::string_view name, const glm::vec4& value )
        {
            if ( handle.Stamp != GetLayoutStamp() )
                handle = ResolveParam( name );
            if ( !handle.IsValue() || handle.Slot >= m_Row.size() )
                return false;
            const auto& params = GetMaterialLayout().Params;
            if ( handle.Param >= params.size() )
                return false;
            m_Row[handle.Slot] = Core::Formats::MaterialRowSlotValue( params[handle.Param].Type, value );
            m_RowRevision      = NextMaterialRevision();
            return true;
        }

        // The name the override producers use. Identical to SetParam now — it was a separate entry point
        // only because the old transport needed a different byte count and a different flush for it.
        bool SetParamRaw( const std::string& name, const glm::vec4& value )
        {
            return SetParam( name, value );
        }

        // Bind a texture by its sampler name (the Properties texture2D name).
        //
        // A NULL IMAGE IS "NONE", NOT A REFUSAL, and that is the whole of М9's second half. This used to
        // `return false` on null, which made an empty slot INEXPRESSIBLE: the editor could take the
        // handle out of the `.demat` and the descriptor went on pointing at the last texture assigned, so
        // the file said one thing and the surface drew another with nothing in between to notice. Null
        // now means the shader's own `Properties … = "white"` default, which is what an empty slot looks
        // like by the shader author's definition.
        //
        // A ROW texture (Core::Formats::IsRowTexture: every 2D texture of a template with a row and no
        // TextureBinding) is a row slot read through the bindless table, so it is per draw like any value. A
        // TextureBinding template's texture is still a descriptor of the material's set, shared by every draw
        // the material records; for those the RENDERER keys materials apart by texture set
        // (MeshRenderer::DrawGenericMeshes).
        //
        // @return false only when @p name is not a Texture2D of this shader at all.
        bool SetTexture( const std::string& name, const Image2D* image )
        {
            if ( !image )
                return BindSchemaDefaultTexture( name );

            // The name is the asset's (its schema property), so the lookup is by that name, not a parameter.
            if ( auto* tex = m_MaterialExecutor->GetTexture2DProperty( name ).get() )
            {
                tex->SetImage( image, RDG::Access::SampledGraphics );
                return true;
            }
            return false;
        }

        // THE TEXTURE HANDLE ROUTE: @p name's Texture2D property in the current layout and executor (Property null
        // when the shader declares no such texture - SetTexture's "false").
        [[nodiscard]] MaterialTextureHandle ResolveTexture( std::string_view name ) const
        {
            MaterialTextureHandle handle;
            handle.Stamp    = GetLayoutStamp();
            handle.Executor = m_MaterialExecutor.get();
            handle.Name.assign( name );
            handle.Resolved = true;
            if ( m_MaterialExecutor )
                handle.Property = m_MaterialExecutor->FindTexture2DProperty( name );
            return handle;
        }

        // SetTexture through a handle: re-resolved by @p name only when the layout, the executor or the name at
        // the handle's position moved, else bound through the kept property. A null image is "none", as above.
        bool SetTexture( MaterialTextureHandle& handle, std::string_view name, const Image2D* image )
        {
            if ( !handle.Resolved || handle.Stamp != GetLayoutStamp() ||
                 handle.Executor != static_cast<const void*>( m_MaterialExecutor.get() ) || handle.Name != name )
                handle = ResolveTexture( name );
            if ( !image )
                return BindSchemaDefaultTexture( handle.Name );
            if ( handle.Property == nullptr )
                return false;
            handle.Property->SetImage( image, RDG::Access::SampledGraphics );
            return true;
        }

        // The handle this material keeps for the texture override at @p index of the set it is keyed by (the
        // renderer keys one material per texture set, so every draw through it names the same samplers in the
        // same places). Grown on first use, never shrunk: the capacity is the material's.
        [[nodiscard]] MaterialTextureHandle& OverrideTextureHandle( const size_t index )
        {
            if ( m_OverrideTextures.size() <= index )
                m_OverrideTextures.resize( index + 1 );
            return m_OverrideTextures[index];
        }

        // ONCE PER MATERIAL PER FRAME (UE: a material's descriptor set is built once, every draw of it binds it):
        // whether the override textures were bound under @p epoch (the renderer's frame of the queue) and the
        // current layout. Epoch 0 is "no frame": never answered as bound.
        // The caller holds LockOverrideTextures() across the check, the binds and the mark: two views' walks may
        // record one shared material at once, and the handles above grow and re-resolve in place.
        [[nodiscard]] std::unique_lock<std::mutex> LockOverrideTextures()
        {
            return std::unique_lock<std::mutex>( m_OverrideTexturesLock );
        }
        [[nodiscard]] bool OverrideTexturesBound( const uint64_t epoch ) const noexcept
        {
            return epoch != 0 && m_OverrideTexturesEpoch == epoch && m_OverrideTexturesStamp == GetLayoutStamp();
        }
        void MarkOverrideTexturesBound( const uint64_t epoch ) noexcept
        {
            m_OverrideTexturesEpoch = epoch;
            m_OverrideTexturesStamp = GetLayoutStamp();
        }

        // Seed every numeric param with its `Properties ... = default` value, at the cell layout's offset. The
        // defaults are built once per layout (RefreshDefaultRow) and copied into the row's own storage: no
        // allocation once the row has its size.
        void ApplyDefaults()
        {
            RefreshDefaultRow();
            m_Row.assign( m_DefaultRow.begin(), m_DefaultRow.end() );
            m_RowRevision = NextMaterialRevision();
        }

        // WHAT A DRAW'S ROW STARTS FROM, AS A REVISION (UE: a cached mesh draw command stays valid until its
        // material changes): the layout's defaults and the row textures' bindless slots. Moves when the layout
        // is another one (a hot reload, another program) or a row texture's slot changed; never with a value
        // a draw writes. With SyncOverrideRevision it is the scene row's MaterialRowRevision.
        [[nodiscard]] uint64_t GetBaseRowRevision()
        {
            RefreshDefaultRow();
            WriteTextureSlots();
            return m_BaseRevision;
        }

        // The revision of @p overrides' Params as they stand (MaterialOverrideHandles): kept while the same
        // vector holds the same values bitwise, a new one otherwise. Reads no name.
        static uint64_t SyncOverrideRevision( const MaterialOverrides& overrides )
        {
            MaterialOverrideHandles& resolved = overrides.Resolved;
            const auto&              params   = overrides.Params;
            if ( resolved.Revision == 0 || resolved.ParamsData != static_cast<const void*>( params.data() ) ||
                 resolved.ParamsCount != params.size() )
            {
                resolved.Handles.assign( params.size(), MaterialParamHandle{} );
                resolved.Written.resize( params.size() );
                for ( size_t index = 0; index < params.size(); ++index )
                    resolved.Written[index] = params[index].second;
                resolved.ParamsData  = params.data();
                resolved.ParamsCount = params.size();
                resolved.Revision    = NextMaterialRevision();
                return resolved.Revision;
            }
            bool moved = false;
            for ( size_t index = 0; index < params.size(); ++index )
            {
                // Bitwise, as the texture slots: a NaN a person typed must not read as a change every frame.
                if ( std::memcmp( &resolved.Written[index], &params[index].second, sizeof( glm::vec4 ) ) == 0 )
                    continue;
                resolved.Written[index] = params[index].second;
                moved                   = true;
            }
            if ( moved )
                resolved.Revision = NextMaterialRevision();
            return resolved.Revision;
        }

        // The row of @p overrides: the defaults, then each Params value through its handle (resolved by name only
        // when the layout moved). A parameter the layout lacks is skipped, as SetParamRaw's miss always was.
        void ApplyOverrides( const MaterialOverrides& overrides )
        {
            ApplyDefaults();
            MaterialOverrideHandles& resolved = overrides.Resolved;
            const auto&              params   = overrides.Params;
            if ( resolved.Handles.size() != params.size() )
                resolved.Handles.assign( params.size(), MaterialParamHandle{} );
            for ( size_t index = 0; index < params.size(); ++index )
                SetParam( resolved.Handles[index], params[index].first, params[index].second );
        }

    private:
        // The layout's default row, rebuilt only when the layout is another one (its stamp, its object, its
        // Params); a rebuild moves the base revision (GetBaseRowRevision).
        void RefreshDefaultRow()
        {
            const auto&               layout = GetMaterialLayout();
            const MaterialLayoutStamp stamp  = GetLayoutStamp();
            if ( m_DefaultRowBuilt && m_DefaultRowStamp == stamp && m_DefaultRowLayout == &layout &&
                 m_DefaultRowParams == layout.Params.data() && m_DefaultRowParamCount == layout.Params.size() )
                return;
            m_DefaultRow           = MaterialBinder::DefaultRow( layout );
            m_DefaultRowBuilt      = true;
            m_DefaultRowStamp      = stamp;
            m_DefaultRowLayout     = &layout;
            m_DefaultRowParams     = layout.Params.data();
            m_DefaultRowParamCount = layout.Params.size();
            m_BaseRevision         = NextMaterialRevision();
        }

        // One row texture of the layout: its row slot, its parameter's index in the layout's Params (for the name
        // the report says) and the property its slot is written from.
        struct TextureSlot
        {
            uint32_t                 Slot     = 0;
            uint32_t                 Param    = 0;
            const Texture2DProperty* Property = nullptr;
            // The bindless value last written from it (WriteTextureSlots): another one moves the base revision.
            glm::vec4 Value   = glm::vec4( 0.0f );
            bool      Written = false;
        };

        // THE SLOT TABLE, RESOLVED ONCE PER LAYOUT (UE: FMaterialRenderProxy's uniform expression cache resolves
        // a parameter's offset once per shader map, a draw reads it by index): the layout's row textures and their
        // properties, found by name only when the layout is another one - the program's identity and code
        // generation (a recompile replaces the layout, Shader::GetSerial), the layout object and its Params - not
        // on every GetParamRow. The properties are the executor's for its whole life (InitializeProperties runs
        // once, in its constructor, and the material keeps its executor), so a pointer here never outlives its
        // property. Caller holds m_TextureSlotsLock.
        void ResolveTextureSlots() const
        {
            const auto&    layout     = GetMaterialLayout();
            const Shader*  shader     = m_MaterialExecutor ? m_MaterialExecutor->GetShader().get() : nullptr;
            const uint64_t serial     = shader != nullptr ? shader->GetSerial() : 0u;
            const uint64_t generation = shader != nullptr ? shader->GetCodeGeneration() : 0u;
            if ( m_TextureSlotsResolved && m_TextureSlotsSerial == serial &&
                 m_TextureSlotsGeneration == generation && m_TextureSlotsLayout == &layout &&
                 m_TextureSlotsParams == layout.Params.data() && m_TextureSlotsParamCount == layout.Params.size() )
                return;
            m_TextureSlots.clear();
            for ( size_t index = 0; index < layout.Params.size(); ++index )
            {
                const auto& p = layout.Params[index];
                if ( !p.IsTexture )
                    continue;
                const Texture2DProperty* texture = m_MaterialExecutor->FindTexture2DProperty( p.Name );
                if ( texture == nullptr )
                    continue;
                m_TextureSlots.push_back( { .Slot     = p.Offset / Core::Formats::kMaterialParamSlotSize,
                                            .Param    = static_cast<uint32_t>( index ),
                                            .Property = texture } );
            }
            m_TextureSlotsResolved   = true;
            m_TextureSlotsSerial     = serial;
            m_TextureSlotsGeneration = generation;
            m_TextureSlotsLayout     = &layout;
            m_TextureSlotsParams     = layout.Params.data();
            m_TextureSlotsParamCount = layout.Params.size();
        }

        void WriteTextureSlots() const
        {
            // Serialised: the table and the row are rewritten from a const read, and one material's row may be
            // read by several threads' draw walks.
            const std::lock_guard<std::mutex> lock( m_TextureSlotsLock );
            ResolveTextureSlots();
            const auto& params = GetMaterialLayout().Params;
            for ( TextureSlot& entry : m_TextureSlots )
            {
                const uint32_t           slot    = entry.Slot;
                const Texture2DProperty* texture = entry.Property;
                const auto&              p       = params[entry.Param];
                if ( slot >= m_Row.size() )
                    continue;
                const BindlessTextureSlot image = texture->GetBindlessSlot();
                // Slot 0 is the table's magenta "missing" texture: an image with no slot (a render target, a
                // storage image, a multisampled one) or one sampled through its stored encoding is not
                // something the table holds, and the surface shows it rather than something plausible.
                if ( ( !image.IsValid() || texture->SamplesEncoded() ) && !m_ReportedTextureSlot )
                {
                    m_ReportedTextureSlot = true;
                    LOG_ERROR(
                         "[Materials] a material of shader '{}': row texture '{}' {}; it draws the "
                         "bindless table's missing texture (reported once per material)",
                         m_ShaderName, p.Name,
                         texture->SamplesEncoded()
                              ? "asks for the image's stored encoding, which the bindless table does not hold"
                              : "has no bindless slot: only sampled (Image2DUsage::Image2D), single-sample, "
                                "non-storage images are in the table" );
                }
                // z: the extent of the image the slot names (Texture2DProperty::GetBindlessExtent; slot 0, the
                // missing texture, is 1 x 1): a surface tests "a real map is bound" from its row, not with a
                // textureSize query per pixel (FPS-GBUFFER; UE: a material's texture set is static per material).
                const uint32_t  extent = image.IsValid() ? texture->GetBindlessExtent() : ( 1u | ( 1u << 16u ) );
                const glm::vec4 value( glm::uintBitsToFloat( image.Index ),
                                       glm::uintBitsToFloat( texture->GetBindlessSampler().Index ),
                                       glm::uintBitsToFloat( extent ), 0.0f );
                // Bitwise: the slot holds index bits, and a NaN pattern must not read as a change every frame.
                if ( !entry.Written || std::memcmp( &entry.Value, &value, sizeof( value ) ) != 0 )
                {
                    entry.Value    = value;
                    entry.Written  = true;
                    m_BaseRevision = NextMaterialRevision();
                }
                if ( std::memcmp( &m_Row[slot], &value, sizeof( value ) ) != 0 )
                {
                    m_Row[slot]   = value;
                    m_RowRevision = NextMaterialRevision();
                }
            }
        }

        std::string                      m_ShaderName;
        // Derived from m_ShaderName (declared before it, so it is initialised first) and never written again.
        std::optional<MeshVertexPath>    m_VertexPath = MeshCellPath( m_ShaderName );
        Core::Formats::ShaderProgramMeta m_Schema;
        // Mutable for the texture slots only: they are a projection of the texture properties, refreshed each
        // time the row is read (GetParamRow).
        mutable Core::Formats::MaterialParamRow m_Row;
        mutable uint64_t                        m_RowRevision = NextMaterialRevision(); // GetParamRowRevision
        mutable bool                            m_ReportedTextureSlot = false;
        // ResolveTextureSlots' table and the layout identity it was resolved for.
        mutable std::mutex                                m_TextureSlotsLock;
        mutable std::vector<TextureSlot>                  m_TextureSlots;
        mutable bool                                      m_TextureSlotsResolved   = false;
        mutable uint64_t                                  m_TextureSlotsSerial     = 0;
        mutable uint64_t                                  m_TextureSlotsGeneration = 0;
        mutable const Core::Formats::MaterialLayout*      m_TextureSlotsLayout     = nullptr;
        mutable const Core::Formats::MaterialLayoutParam* m_TextureSlotsParams     = nullptr;
        mutable size_t                                    m_TextureSlotsParamCount = 0;
        // GetBaseRowRevision: moved by RefreshDefaultRow's rebuild and by a row texture's new bindless value.
        mutable uint64_t m_BaseRevision = NextMaterialRevision();
        // RefreshDefaultRow's row and the layout identity it was built for.
        Core::Formats::MaterialParamRow           m_DefaultRow;
        bool                                      m_DefaultRowBuilt = false;
        MaterialLayoutStamp                       m_DefaultRowStamp;
        const Core::Formats::MaterialLayout*      m_DefaultRowLayout     = nullptr;
        const Core::Formats::MaterialLayoutParam* m_DefaultRowParams     = nullptr;
        size_t                                    m_DefaultRowParamCount = 0;
        // OverrideTextureHandle's handles and MarkOverrideTexturesBound's frame and layout.
        std::mutex                         m_OverrideTexturesLock;
        std::vector<MaterialTextureHandle> m_OverrideTextures;
        uint64_t                           m_OverrideTexturesEpoch = 0;
        MaterialLayoutStamp                m_OverrideTexturesStamp;
    };
} // namespace Desert::Graphic
