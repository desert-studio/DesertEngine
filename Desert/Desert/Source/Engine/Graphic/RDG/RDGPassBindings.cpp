#include <Engine/Graphic/RDG/RDGPassBindings.hpp>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string_view>
#include <vector>
#include <format>

namespace Desert::Graphic::RDG
{
    namespace
    {
        bool IsSampledAccess( Access access )
        {
            return access == Access::SampledCompute || access == Access::SampledGraphics;
        }

        bool IsStorageAccess( Access access )
        {
            return access == Access::StorageRead || access == Access::StorageWrite;
        }

        // CPU-RDG-ALLOC: the slot name a declared entry views - "<Name>" or "<Name>[<Element>]" - written once
        // into the graph's frame arena (UE: names in the FRDGAllocator, not heap strings). Copied even for a whole
        // parameter: a parameter's Name is a view whose source the graph does not own.
        std::string_view ArenaSlotName( Common::Memory::FrameArena& arena, std::string_view name,
                                        uint32_t element )
        {
            std::array<char, 16> digits{};
            size_t               digitCount = 0;
            if ( element != ~0u )
            {
                const std::to_chars_result written =
                     std::to_chars( digits.data(), digits.data() + digits.size(), element );
                digitCount = static_cast<size_t>( written.ptr - digits.data() );
            }
            const size_t length = digitCount == 0 ? name.size() : name.size() + digitCount + 2;
            if ( length == 0 )
                return {};
            char* const storage = static_cast<char*>( arena.Allocate( length, alignof( char ) ) );
            if ( storage == nullptr )
                std::abort(); // the arena refuses only when out of memory - no name to view
            std::memcpy( storage, name.data(), name.size() );
            if ( digitCount != 0 )
            {
                storage[name.size()] = '[';
                std::memcpy( storage + name.size() + 1, digits.data(), digitCount );
                storage[length - 1] = ']';
            }
            return std::string_view( storage, length );
        }
    } // namespace

    // Every failure goes through here so the message has one shape: "<pass>: '<slot>' <- '<resource>': <why>".
    // Only the first one is kept; a later entry never overwrites it.
    // RDG-PARALLEL-7 (RDG-REC-2): the resource an error names ("texture #3"), formatted only when an error is
    // kept - the success path of every entry formatted it before.
    namespace
    {
        struct ResourceLabel
        {
            std::string_view Kind;
            uint64_t         Index = 0;
        };
    } // namespace

    // RDG-PARALLEL-9 (RDG-REC-2; UE: pass parameters come from the graph's allocator, never the heap per pass):
    // the entry vectors of the blocks this thread closed, cleared with their capacity kept. A block opened on
    // the thread takes one of each (PassBindings::PassBindings) and gives them back when it closes
    // (~PassBindings), so a steady frame opens its blocks with no allocation. The lists grow only to the most
    // blocks alive at once on the thread.
    namespace
    {
        struct EntryStorage
        {
            std::vector<std::vector<BoundTexture>> Textures;
            std::vector<std::vector<BoundBuffer>>  Buffers;
            std::vector<std::vector<std::byte>>    Push;
        };
        thread_local EntryStorage t_EntryStorage;

        template <typename T>
        std::vector<T> TakeVector( std::vector<std::vector<T>>& pool )
        {
            if ( pool.empty() )
                return {};
            std::vector<T> taken = std::move( pool.back() );
            pool.pop_back();
            return taken;
        }

        template <typename T>
        void GiveVector( std::vector<std::vector<T>>& pool, std::vector<T>& given )
        {
            if ( given.capacity() == 0 )
                return;
            given.clear();
            pool.push_back( std::move( given ) );
        }
    } // namespace

    static void KeepFirstError( std::string& firstError, std::string_view pass, std::string_view shaderName,
                                std::string_view resource, std::string_view why )
    {
        if ( firstError.empty() )
            firstError = fmt::format( "{}: '{}' <- '{}': {}", pass, shaderName, resource, why );
    }

    static void KeepFirstError( std::string& firstError, std::string_view pass, std::string_view shaderName,
                                ResourceLabel resource, std::string_view why )
    {
        if ( firstError.empty() )
            firstError = fmt::format( "{}: '{}' <- '{} #{}': {}", pass, shaderName, resource.Kind, resource.Index,
                                      why );
    }

    void PassBindings::ResolveSampled( std::string_view shaderName, ShaderResourceKind kind, TextureRef texture,
                                       Access declared, SubresourceRange range,
                                       std::optional<SamplerDesc> sampler )
    {
        if ( !m_FirstError.empty() )
            return;
        const std::string_view pass   = m_Context.GetPassName();
        const ResourceLabel    handle{ "texture", texture.Index };
        if ( !IsSampledAccess( declared ) )
        {
            KeepFirstError( m_FirstError, pass, shaderName, handle,
                            fmt::format( "a sampled entry is declared as {}, not SampledCompute / SampledGraphics",
                                         GetAccessName( declared ) ) );
            return;
        }
        const Common::ResultStr<TextureBinding> resolved = m_Context.GetTexture( texture, declared, range );
        if ( !resolved.IsSuccess() )
        {
            KeepFirstError( m_FirstError, pass, shaderName, handle, resolved.GetError() );
            return;
        }
        m_Textures.push_back( BoundTexture{
             .ShaderName = shaderName,
             .Kind       = kind,
             .Texture    = resolved.GetValue(),
             // A FaultDefault read binds the 1x1 system texture whole.
             .Range    = resolved.GetValue().Resource == texture.Index ? range : SubresourceRange::All(),
             .Declared = declared,
             .Sampler  = sampler } );
    }

    void PassBindings::ResolveSampler( std::string_view shaderName, SamplerDesc sampler )
    {
        if ( !m_FirstError.empty() )
            return;
        // No resource: the graph neither orders nor barriers a sampler state, and Texture stays empty.
        m_Textures.push_back( BoundTexture{ .ShaderName = shaderName,
                                            .Kind       = ShaderResourceKind::Sampler,
                                            .Texture    = {},
                                            .Range      = SubresourceRange::All(),
                                            .Declared   = Access::None,
                                            .Sampler    = sampler } );
    }

    void PassBindings::ResolveStorageImage( std::string_view shaderName, TextureRef texture, Access declared,
                                            uint32_t mip )
    {
        if ( !m_FirstError.empty() )
            return;
        const std::string_view pass   = m_Context.GetPassName();
        const ResourceLabel    handle{ "texture", texture.Index };
        if ( !IsStorageAccess( declared ) )
        {
            KeepFirstError( m_FirstError, pass, shaderName, handle,
                            fmt::format( "a storage image entry is declared as {}, not StorageRead / StorageWrite",
                                         GetAccessName( declared ) ) );
            return;
        }
        const SubresourceRange                  range    = SubresourceRange::Mip( mip );
        const Common::ResultStr<TextureBinding> resolved = m_Context.GetTexture( texture, declared, range );
        if ( !resolved.IsSuccess() )
        {
            KeepFirstError( m_FirstError, pass, shaderName, handle, resolved.GetError() );
            return;
        }
        m_Textures.push_back( BoundTexture{ .ShaderName = shaderName,
                                            .Kind       = ShaderResourceKind::StorageTexture,
                                            .Texture    = resolved.GetValue(),
                                            .Range      = range,
                                            .Declared   = declared,
                                            .Sampler    = std::nullopt } );
    }

    // A declared uniform or storage buffer entry: the kind decides which accesses are legal.
    static void AddBuffer( const PassContext& context, std::vector<BoundBuffer>& buffers, std::string& firstError,
                           std::string_view shaderName, BufferRef buffer, Access declared, ShaderResourceKind kind )
    {
        if ( !firstError.empty() )
            return;
        const std::string_view pass   = context.GetPassName();
        const ResourceLabel    handle{ "buffer", buffer.Index };
        const bool             legal  = kind == ShaderResourceKind::UniformBuffer ? declared == Access::UniformRead
                                        : kind == ShaderResourceKind::AccelerationStructure
                                             ? declared == Access::AccelStructRead
                                             : IsStorageAccess( declared );
        if ( !legal )
        {
            KeepFirstError( firstError, pass, shaderName, handle,
                            fmt::format( "a {} entry cannot be declared as {}",
                                         kind == ShaderResourceKind::UniformBuffer ? "uniform buffer"
                                         : kind == ShaderResourceKind::AccelerationStructure
                                              ? "acceleration structure"
                                              : "storage buffer",
                                         GetAccessName( declared ) ) );
            return;
        }
        const Common::ResultStr<BufferBinding> resolved = context.GetBuffer( buffer, declared );
        if ( !resolved.IsSuccess() )
        {
            KeepFirstError( firstError, pass, shaderName, handle, resolved.GetError() );
            return;
        }
        buffers.push_back( BoundBuffer{ .ShaderName = shaderName,
                                        .Kind       = kind,
                                        .Buffer     = resolved.GetValue(),
                                        .Declared   = declared } );
    }

    PassBindings& PassBindings::PushConstants( const void* data, uint32_t size )
    {
        if ( !m_FirstError.empty() )
            return *this;
        if ( !m_PushConstants.empty() )
        {
            KeepFirstError( m_FirstError, m_Context.GetPassName(), "push constants", "block",
                            "the push-constant block is already set in this block" );
            return *this;
        }
        if ( size == 0 || data == nullptr )
        {
            KeepFirstError( m_FirstError, m_Context.GetPassName(), "push constants", "block",
                            "an empty push-constant block" );
            return *this;
        }
        m_PushConstants.resize( size );
        std::memcpy( m_PushConstants.data(), data, size );
        return *this;
    }

    Common::BoolResultStr PassBindings::GetStatus() const
    {
        if ( !m_FirstError.empty() )
            return Common::MakeError( m_FirstError );
        return Common::MakeSuccess( true );
    }

    const PassContext& PassBindings::GetContext() const
    {
        return m_Context;
    }

    BindingBlockRef PassBindings::GetBlock() const
    {
        return m_Block;
    }

    std::span<const BoundTexture> PassBindings::GetTextures() const
    {
        return m_Textures;
    }

    std::span<const BoundBuffer> PassBindings::GetBuffers() const
    {
        return m_Buffers;
    }

    std::span<const std::byte> PassBindings::GetPushConstants() const
    {
        return m_PushConstants;
    }
    // ── FPS-ARCH-BINDINGS: layouts indexed once ────────────────────────────────────────────────────────────
    uint32_t ShaderBindingLayout::FindSlot( const uint64_t hash, const std::string_view name ) const noexcept
    {
        // Equal hashes are adjacent: a 64-bit collision between two slot names of one shader is told by name.
        auto found = std::lower_bound( ByHash.begin(), ByHash.end(), hash,
                                       []( const std::pair<uint64_t, uint32_t>& row, const uint64_t key )
                                       { return row.first < key; } );
        for ( ; found != ByHash.end() && found->first == hash; ++found )
        {
            if ( Slots[found->second].Name == name )
                return found->second;
        }
        return kNoShaderSlot;
    }

    void IndexShaderBindingLayout( ShaderBindingLayout& layout )
    {
        layout.ByHash.clear();
        layout.ByHash.reserve( layout.Slots.size() );
        for ( uint32_t index = 0; index < static_cast<uint32_t>( layout.Slots.size() ); ++index )
            layout.ByHash.emplace_back( HashShaderName( layout.Slots[index].Name ), index );
        // Stable: a layout repeating a name keeps its first slot first, as the declaration-order walk does.
        std::stable_sort( layout.ByHash.begin(), layout.ByHash.end(),
                          []( const std::pair<uint64_t, uint32_t>& a, const std::pair<uint64_t, uint32_t>& b )
                          { return a.first < b.first; } );
    }

    std::shared_ptr<const ShaderBindingLayout> MakeIndexedLayout( ShaderBindingLayout layout )
    {
        IndexShaderBindingLayout( layout );
        return std::make_shared<const ShaderBindingLayout>( std::move( layout ) );
    }

    namespace
    {
        // A declared entry placed at its layout slot once, when the pass declares it.
        void PushEntry( DeclaredBindingBlock& block, DeclaredBindingEntry entry )
        {
            entry.Slot = block.Layout ? block.Layout->FindSlot( entry.NameHash, entry.ShaderName ) : kNoShaderSlot;
            block.Entries.push_back( entry );
        }
    } // namespace

    // ── RDG-FAULT1: setup-time binding blocks ─────────────────────────────────────────────────────────────────
    namespace
    {
        std::string_view GetShaderResourceKindName( ShaderResourceKind kind )
        {
            switch ( kind )
            {
                case ShaderResourceKind::StorageTexture:
                    return "storage texture";
                case ShaderResourceKind::Texture:
                    return "separate texture";
                case ShaderResourceKind::Sampler:
                    return "sampler";
                case ShaderResourceKind::UniformBuffer:
                    return "uniform buffer";
                case ShaderResourceKind::StorageBuffer:
                    return "storage buffer";
                case ShaderResourceKind::AccelerationStructure:
                    return "acceleration structure";
            }
            return "unknown";
        }

        constexpr std::string_view kNotInShaderFormat = "'{}' is not a resource of shader '{}'";
        constexpr std::string_view kKindFormat        = "'{}' is a {} in shader '{}', bound as {}";
        constexpr std::string_view kTwiceFormat       = "'{}' of shader '{}' is bound twice by the pass";
        constexpr std::string_view kBothRoutesFormat =
             "'{}' of shader '{}' is bound by the pass and by the material";
        constexpr std::string_view kNeitherRouteFormat =
             "'{}' of shader '{}' is filled by neither the pass nor the "
             "material";
        constexpr std::string_view kPushFormat =
             "push constants: shader '{}' declares {} bytes, the pass gives {}";

        // CPU-FRAME-5: the linear proof that a block's slots are valid, with no string compare per pair of names.
        // The layout's slot names go into an open-addressing table (a repeated name keeps its first slot, as
        // find_if does); every entry must land on a slot of its own of its kind, the other route on no entry's
        // slot, and the two routes together on every slot. True proves the reference walk in
        // ValidatePassBindings finds no slot fault; false proves nothing, and that walk then names the first
        // fault in declaration order exactly as before (a layout repeating a slot name ends there too).
        // Per-thread scratch: a frame's blocks allocate nothing once it has grown.
        bool SlotsProvenValid( const DeclaredBindingBlock& block )
        {
            const ShaderBindingLayout& layout = *block.Layout;
            const size_t               slots  = layout.Slots.size();
            if ( block.Entries.size() > slots )
                return false;
            // FPS-ARCH-BINDINGS: every entry carries its slot index (found when it was declared) and the layout
            // its hash index (built when it was made), so no name is hashed or compared here per block.
            thread_local std::vector<uint8_t> covered; // 0 open, 1 by the block, 2 by the other route
            covered.assign( slots, 0 );
            size_t filled = 0;
            for ( const DeclaredBindingEntry& entry : block.Entries )
            {
                const uint32_t slot = entry.Slot;
                if ( slot >= slots || covered[slot] != 0 || layout.Slots[slot].Kind != entry.Kind )
                    return false;
                covered[slot] = 1;
                ++filled;
            }
            for ( const std::string& name : block.Other->Slots )
            {
                const uint32_t slot = layout.FindSlot( HashShaderName( name ), name );
                if ( slot == kNoShaderSlot )
                    continue; // the walk asks the other route only about the shader's own slots
                if ( covered[slot] == 1 )
                    return false;
                if ( covered[slot] == 0 )
                {
                    covered[slot] = 2;
                    ++filled;
                }
            }
            return filled == slots;
        }
    } // namespace

    Common::BoolResultStr ValidatePassBindings( const DeclaredBindingBlock& block )
    {
        if ( !block.Layout )
            return Common::MakeError( std::string( "the block declares no shader binding layout" ) );
        if ( !block.Other )
            return Common::MakeError( std::format( "the block of shader '{}' declares no other-route fill "
                                                   "(RDG::NoOtherRouteFill when the other route fills nothing)",
                                                   block.Layout->ShaderName ) );
        const ShaderBindingLayout& layout  = *block.Layout;
        const OtherRouteFill&      other   = *block.Other;
        // CPU-FRAME-5: the linear proof first; this walk runs only when it cannot prove the slots valid,
        // and names the first fault in declaration order.
        if ( !SlotsProvenValid( block ) )
        {
            const auto                 byOther = [&]( std::string_view name ) {
                return std::find( other.Slots.begin(), other.Slots.end(), name ) != other.Slots.end();
            };
            for ( const DeclaredBindingEntry& entry : block.Entries )
            {
                const auto sameName = [&]( const DeclaredBindingEntry& e )
                { return e.ShaderName == entry.ShaderName; };
                if ( std::count_if( block.Entries.begin(), block.Entries.end(), sameName ) > 1 )
                    return Common::MakeError( std::format( kTwiceFormat, entry.ShaderName, layout.ShaderName ) );
                const auto slot = std::find_if( layout.Slots.begin(), layout.Slots.end(),
                                                [&]( const ShaderSlot& s ) { return s.Name == entry.ShaderName; } );
                if ( slot == layout.Slots.end() )
                    return Common::MakeError( std::format( kNotInShaderFormat, entry.ShaderName, layout.ShaderName ) );
                if ( slot->Kind != entry.Kind )
                    return Common::MakeError( std::format( kKindFormat, entry.ShaderName,
                                                           GetShaderResourceKindName( slot->Kind ), layout.ShaderName,
                                                           GetShaderResourceKindName( entry.Kind ) ) );
                if ( byOther( entry.ShaderName ) )
                    return Common::MakeError( std::format( kBothRoutesFormat, entry.ShaderName, layout.ShaderName ) );
            }
            for ( const ShaderSlot& slot : layout.Slots )
            {
                const bool byBlock =
                     std::any_of( block.Entries.begin(), block.Entries.end(),
                                  [&]( const DeclaredBindingEntry& e ) { return e.ShaderName == slot.Name; } );
                if ( !byBlock && !byOther( slot.Name ) )
                    return Common::MakeError( std::format( kNeitherRouteFormat, slot.Name, layout.ShaderName ) );
            }
        }
        const uint32_t declared = layout.PushConstantBytes;
        const uint32_t given    = block.PushConstantBytes;
        const bool     refused =
             declared == 0 ? given != 0 || other.PushConstants
                               : ( given == 0 ) == !other.PushConstants || ( given != 0 && given != declared );
        if ( refused )
            return Common::MakeError( std::format( kPushFormat, layout.ShaderName, declared, given ) );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr ValidateShaderParameters( const ShaderBindingLayout&      layout,
                                                    std::span<const ShaderParameter> parameters )
    {
        constexpr std::string_view kMissingFormat = "parameter '{}' is not a resource of shader '{}'";
        constexpr std::string_view kKindFormat    = "parameter '{}' is a {} in shader '{}', declared as a {}";
        for ( const ShaderParameter& parameter : parameters )
        {
            // An arrayed parameter is present when its first element is (the layout lists each element).
            const std::string slotName =
                 parameter.Arrayed ? std::format( "{}[0]", parameter.Name ) : std::string( parameter.Name );
            const auto slot = std::find_if( layout.Slots.begin(), layout.Slots.end(),
                                            [&]( const ShaderSlot& s ) { return s.Name == slotName; } );
            if ( slot == layout.Slots.end() )
                return Common::MakeError( std::format( kMissingFormat, parameter.Name, layout.ShaderName ) );
            if ( slot->Kind != parameter.Kind )
                return Common::MakeError( std::format( kKindFormat, parameter.Name,
                                                       GetShaderResourceKindName( slot->Kind ), layout.ShaderName,
                                                       GetShaderResourceKindName( parameter.Kind ) ) );
        }
        return Common::MakeSuccess( true );
    }

    BindingBlockBuilder PassBuilder::Bindings( ShaderBindingLayout layout, std::shared_ptr<const OtherRouteFill> other )
    {
        return Bindings( MakeIndexedLayout( std::move( layout ) ), std::move( other ) );
    }

    BindingBlockBuilder PassBuilder::Bindings( const std::shared_ptr<const ShaderBindingLayout>& layout,
                                               std::shared_ptr<const OtherRouteFill>             other )
    {
        // FPS-DEVLOST18: what the other route binds from the graph (a material's lent scene tables) is a read of
        // this pass, declared here once for every route-bound pass (Builder::LendToRoute).
        if ( other && !other->Slots.empty() && !m_Builder.m_RouteLent.empty() )
            DeclareRouteLentReads( *other );
        auto& blocks = m_Builder.m_Passes[m_Pass].Blocks; // CPU-FRAME-ARENA: an arena vector
        // CPU-RDG-ALLOC: the entries live in the graph's arena too - no heap vector per block per frame.
        blocks.push_back( DeclaredBindingBlock{
             layout, std::move( other ),
             Common::Memory::ArenaVector<DeclaredBindingEntry>(
                  Common::Memory::ArenaAllocator<DeclaredBindingEntry>( m_Builder.m_Arena ) ),
             0 } );
        return BindingBlockBuilder( *this, BindingBlockRef{ m_Pass, static_cast<uint32_t>( blocks.size() - 1 ) } );
    }

    BindingBlockBuilder& BindingBlockBuilder::Texture( TextureParameter parameter, TextureRef texture,
                                                       Access declared, SubresourceRange range )
    {
        const std::string_view shaderName =
             ArenaSlotName( m_Pass.m_Builder.m_Arena, parameter.Name, parameter.Element );
        m_Pass.DeclareTexture( texture, declared, range, false, "Bindings.Texture" );
        PushEntry( m_Pass.m_Builder.m_Passes[m_Ref.Pass].Blocks[m_Ref.Block],
             { shaderName, parameter.SlotNameHash(),
               ShaderResourceKind::Texture, ResourceKind::Texture, texture.Index, declared, range,
               std::nullopt } );
        return *this;
    }

    BindingBlockBuilder& BindingBlockBuilder::Sampler( SamplerParameter parameter, SamplerDesc sampler )
    {
        const std::string_view shaderName =
             ArenaSlotName( m_Pass.m_Builder.m_Arena, parameter.Name, parameter.Element );
        // No resource and no access: nothing for the graph to declare, only the slot to fill. The same shared
        // state named again by another input of the block is the entry already there.
        DeclaredBindingBlock& block = m_Pass.m_Builder.m_Passes[m_Ref.Pass].Blocks[m_Ref.Block];
        Common::Memory::ArenaVector<DeclaredBindingEntry>& entries = block.Entries;
        const bool already = std::any_of( entries.begin(), entries.end(),
                                          [&]( const DeclaredBindingEntry& e ) {
                                              return e.Kind == ShaderResourceKind::Sampler &&
                                                     e.ShaderName == shaderName && e.Sampler == sampler;
                                          } );
        if ( already )
            return *this;
        PushEntry( block, { shaderName, parameter.SlotNameHash(), ShaderResourceKind::Sampler,
                             ResourceKind::Texture,
                             kInvalidResource,
                             Access::None, SubresourceRange::All(), sampler } );
        return *this;
    }

    BindingBlockBuilder& BindingBlockBuilder::Storage( StorageTextureParameter parameter, TextureRef texture,
                                                       Access declared, uint32_t mip )
    {
        const std::string_view shaderName =
             ArenaSlotName( m_Pass.m_Builder.m_Arena, parameter.Name, parameter.Element );
        m_Pass.DeclareTexture( texture, declared, SubresourceRange::Mip( mip ), IsWriteAccess( declared ),
                               "Bindings.Storage" );
        PushEntry( m_Pass.m_Builder.m_Passes[m_Ref.Pass].Blocks[m_Ref.Block],
             { shaderName, parameter.SlotNameHash(),
               ShaderResourceKind::StorageTexture, ResourceKind::Texture, texture.Index, declared,
               SubresourceRange::Mip( mip ), std::nullopt } );
        return *this;
    }

    BindingBlockBuilder& BindingBlockBuilder::StorageLayer( const StorageTextureParameter parameter, TextureRef texture,
                                                            Access declared, uint32_t mip, uint32_t layer )
    {
        m_Pass.DeclareTexture( texture, declared, SubresourceRange::MipLayer( mip, layer ), IsWriteAccess( declared ),
                               "Bindings.StorageLayer" );
        PushEntry( m_Pass.m_Builder.m_Passes[m_Ref.Pass].Blocks[m_Ref.Block],
             { ArenaSlotName( m_Pass.m_Builder.m_Arena, parameter.Name, parameter.Element ),
               parameter.SlotNameHash(),
               ShaderResourceKind::StorageTexture, ResourceKind::Texture, texture.Index,
               declared, SubresourceRange::MipLayer( mip, layer ), std::nullopt } );
        return *this;
    }

    BindingBlockBuilder& BindingBlockBuilder::Uniform( UniformBufferParameter parameter, BufferRef buffer )
    {
        const std::string_view shaderName =
             ArenaSlotName( m_Pass.m_Builder.m_Arena, parameter.Name, parameter.Element );
        m_Pass.DeclareBuffer( buffer, Access::UniformRead, false, "Bindings.Uniform" );
        PushEntry( m_Pass.m_Builder.m_Passes[m_Ref.Pass].Blocks[m_Ref.Block],
             { shaderName, parameter.SlotNameHash(), ShaderResourceKind::UniformBuffer, ResourceKind::Buffer,
               buffer.Index,
               Access::UniformRead, SubresourceRange::All(), std::nullopt } );
        return *this;
    }

    BindingBlockBuilder& BindingBlockBuilder::Storage( StorageBufferParameter parameter, BufferRef buffer,
                                                       Access declared )
    {
        const std::string_view shaderName =
             ArenaSlotName( m_Pass.m_Builder.m_Arena, parameter.Name, parameter.Element );
        // The call names the shader binding, so an invalid handle says WHICH input the pass was never given.
        // CPU-RDG-ALLOC: formatted on the stack (the label is read only by an error), not a heap string per bind.
        std::array<char, 192> label{};
        const auto            written = fmt::format_to_n( label.data(), label.size(), "Bindings.Storage[{}]",
                                                          shaderName );
        const size_t          length  = std::min( static_cast<size_t>( written.size ), label.size() );
        m_Pass.DeclareBuffer( buffer, declared, IsWriteAccess( declared ),
                              std::string_view( label.data(), length ) );
        PushEntry( m_Pass.m_Builder.m_Passes[m_Ref.Pass].Blocks[m_Ref.Block],
             { shaderName, parameter.SlotNameHash(),
               ShaderResourceKind::StorageBuffer, ResourceKind::Buffer, buffer.Index, declared,
               SubresourceRange::All(), std::nullopt } );
        return *this;
    }

    BindingBlockBuilder& BindingBlockBuilder::AccelerationStructure( AccelerationStructureParameter parameter,
                                                                     BufferRef                      tlas )
    {
        const std::string_view shaderName =
             ArenaSlotName( m_Pass.m_Builder.m_Arena, parameter.Name, parameter.Element );
        m_Pass.DeclareBuffer( tlas, Access::AccelStructRead, false, "Bindings.AccelerationStructure" );
        PushEntry( m_Pass.m_Builder.m_Passes[m_Ref.Pass].Blocks[m_Ref.Block],
             { shaderName, parameter.SlotNameHash(),
               ShaderResourceKind::AccelerationStructure, ResourceKind::Buffer, tlas.Index,
               Access::AccelStructRead, SubresourceRange::All(), std::nullopt } );
        return *this;
    }

    BindingBlockBuilder& BindingBlockBuilder::PushConstantBytes( uint32_t bytes )
    {
        m_Pass.m_Builder.m_Passes[m_Ref.Pass].Blocks[m_Ref.Block].PushConstantBytes = bytes;
        return *this;
    }

    BindingBlockRef BindingBlockBuilder::GetRef() const
    {
        return m_Ref;
    }

    PassBindings::PassBindings( const PassContext& context, BindingBlockRef block )
         : m_Context( context ), m_Block( block ), m_Textures( TakeVector( t_EntryStorage.Textures ) ),
           m_Buffers( TakeVector( t_EntryStorage.Buffers ) ), m_PushConstants( TakeVector( t_EntryStorage.Push ) )
    {
        const auto& passes = context.m_Builder.m_Passes;
        if ( block.Pass != context.m_Pass || block.Block >= passes[block.Pass].Blocks.size() )
        {
            m_FirstError = std::format( "{}: binding block {} of pass #{} is not a block this pass declared",
                                        context.GetPassName(), block.Block, block.Pass );
            return;
        }
        const DeclaredBindingBlock& declared = passes[block.Pass].Blocks[block.Block];
        m_Layout                             = declared.Layout.get();
        // FPS-CPU2-RDG2 (UE: FRDGBuilder validates the pass parameters once, not again at execute): no slot is
        // checked for a second bind here. Every block of a pass that executes passed ValidatePassBindings in
        // Compile (FaultMalformedPasses), which refuses a slot twice; a pass whose block failed never runs.
        for ( const DeclaredBindingEntry& entry : declared.Entries )
        {
            const size_t texturesBefore = m_Textures.size();
            const size_t buffersBefore  = m_Buffers.size();
            switch ( entry.Kind )
            {
                case ShaderResourceKind::Texture:
                    ResolveSampled( entry.ShaderName, entry.Kind, TextureRef{ entry.Index }, entry.Declared,
                                    entry.Range, std::nullopt );
                    break;
                case ShaderResourceKind::Sampler:
                    // BindingBlockBuilder::Sampler always carries the state; an entry without one is hand-built.
                    if ( !entry.Sampler )
                    {
                        if ( m_FirstError.empty() )
                            KeepFirstError( m_FirstError, context.GetPassName(), entry.ShaderName, "sampler",
                                            "a Sampler entry carries no sampler state" );
                        break;
                    }
                    ResolveSampler( entry.ShaderName, *entry.Sampler );
                    break;
                case ShaderResourceKind::StorageTexture:
                    ResolveStorageImage( entry.ShaderName, TextureRef{ entry.Index }, entry.Declared,
                                         entry.Range.BaseMip );
                    break;
                case ShaderResourceKind::UniformBuffer:
                    AddBuffer( m_Context, m_Buffers, m_FirstError, entry.ShaderName,
                               BufferRef{ entry.Index }, Access::UniformRead, ShaderResourceKind::UniformBuffer );
                    break;
                case ShaderResourceKind::StorageBuffer:
                    AddBuffer( m_Context, m_Buffers, m_FirstError, entry.ShaderName,
                               BufferRef{ entry.Index }, entry.Declared, ShaderResourceKind::StorageBuffer );
                    break;
                case ShaderResourceKind::AccelerationStructure:
                    AddBuffer( m_Context, m_Buffers, m_FirstError, entry.ShaderName,
                               BufferRef{ entry.Index }, Access::AccelStructRead,
                               ShaderResourceKind::AccelerationStructure );
                    break;
            }
            // FPS-ARCH-BINDINGS: the entry resolved (one texture or one buffer added) carries its declared slot.
            if ( m_Textures.size() != texturesBefore )
            {
                m_Textures.back().Slot     = entry.Slot;
                m_Textures.back().NameHash = entry.NameHash;
            }
            else if ( m_Buffers.size() != buffersBefore )
            {
                m_Buffers.back().Slot     = entry.Slot;
                m_Buffers.back().NameHash = entry.NameHash;
            }
        }
    }

    PassBindings::~PassBindings()
    {
        EntryStorage& storage = t_EntryStorage;
        GiveVector( storage.Textures, m_Textures );
        GiveVector( storage.Buffers, m_Buffers );
        GiveVector( storage.Push, m_PushConstants );
    }
} // namespace Desert::Graphic::RDG
