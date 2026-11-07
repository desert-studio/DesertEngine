#include <Engine/Graphic/GpuDriven/GpuSceneRegistry.hpp>

#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Geometry/MeshBounds.hpp>

#include <Common/Core/Profiler.hpp>

#include <algorithm>
#include <cstring>

namespace Desert::Graphic::GpuDriven
{
    namespace
    {
        void FillBounds( PrimitiveDesc& desc, const Mesh* mesh )
        {
            if ( mesh == nullptr )
                return;
            const Common::Math::AABB box = Geometry::LocalBounds( mesh->GetSubmeshes() );
            if ( Geometry::IsEmpty( box ) )
                return;
            desc.LocalBoundsMin = box.Min;
            desc.LocalBoundsMax = box.Max;
            desc.LodScreenSizes = mesh->GetLODScreenSizes();
        }

        MeshGeometryBinding GeometryOf( const Mesh* mesh )
        {
            return mesh == nullptr ? MeshGeometryBinding{} : mesh->GetGeometry().MeshBinding();
        }

        // A Content word spread over 64 bits (splitmix64's finaliser): the sum of a primitive's records' mixes does
        // not depend on their order and two records do not cancel as an xor of equal words would.
        uint64_t MixContent( uint64_t word )
        {
            word += 0x9E3779B97F4A7C15ull;
            word = ( word ^ ( word >> 30u ) ) * 0xBF58476D1CE4E5B9ull;
            word = ( word ^ ( word >> 27u ) ) * 0x94D049BB133111EBull;
            return word ^ ( word >> 31u );
        }

        bool SameInstances( const std::span<const GpuSceneInstance> held,
                            const std::span<const glm::mat4>        transforms )
        {
            if ( held.size() != std::max<size_t>( transforms.size(), 1u ) )
                return false;
            for ( size_t i = 0; i < transforms.size(); ++i )
                if ( held[i].InstanceToPrimitive != PackAffine( transforms[i] ) )
                    return false;
            return true;
        }
    } // namespace

    void GpuSceneRegistry::BeginContent( Entry& entry, const bool inserted, const uint64_t content )
    {
        entry.ContentSync  = m_SyncSerial;
        entry.FrameContent = MixContent( content );
        m_ContentTouched.emplace_back( &entry, inserted );
    }

    void GpuSceneRegistry::AddContent( Entry& entry, const uint64_t content ) const
    {
        if ( entry.ContentSync == m_SyncSerial ) // else an earlier view of the frame summed it
            entry.FrameContent += MixContent( content );
    }

    void GpuSceneRegistry::PublishContent()
    {
        for ( const auto& [entry, inserted] : m_ContentTouched )
        {
            if ( !inserted && entry->FrameContent != entry->Content )
                m_Scene.MarkContent( entry->Primitive );
            entry->Content = entry->FrameContent;
        }
        m_ContentTouched.clear();
    }

    void GpuSceneRegistry::CommitFrame( const uint64_t frame )
    {
        if ( m_Started && frame == m_Frame )
            m_Committed = true;
    }

    void GpuSceneRegistry::Queued( Entry& entry ) const
    {
        entry.PrevFrame = entry.Frame;
        entry.Frame     = m_Frame;
    }

    void GpuSceneRegistry::AskedThisFrame( MaterialEntry& entry ) const
    {
        if ( entry.Frame != m_Frame ) // the first view of this frame to ask
        {
            entry.PrevFrame = entry.Frame;
            entry.Frame     = m_Frame;
        }
    }

    std::optional<uint32_t> GpuSceneRegistry::ReuseMaterialRow( const MaterialRowKey&     key,
                                                                const MaterialRowRevision revision )
    {
        if ( revision.Template == 0 )
            return std::nullopt;
        const auto found = m_MaterialRows.find( key );
        if ( found == m_MaterialRows.end() || found->second.Revision != revision )
            return std::nullopt;
        AskedThisFrame( found->second );
        return found->second.Row;
    }

    uint32_t GpuSceneRegistry::SyncMaterialRow( const MaterialRowKey& key, const std::span<const glm::vec4> row,
                                                const MaterialRowRevision revision )
    {
        if ( row.empty() )
            return 0u;
        const auto [it, inserted] = m_MaterialRows.try_emplace( key );
        MaterialEntry& entry      = it->second;
        const bool sameLayout = !inserted && entry.Values.size() == row.size();
        const bool sameValues = sameLayout && std::equal( row.begin(), row.end(), entry.Values.begin() );
        if ( !sameLayout )
        {
            if ( !inserted ) // the template's layout changed (hot reload): a row at the new stride
                m_Scene.RemoveMaterialRow( entry.Row, static_cast<uint32_t>( entry.Values.size() ) );
            entry.Row = m_Scene.AddMaterialRow( row );
        }
        else if ( !sameValues )
            m_Scene.UpdateMaterialRow( entry.Row, row );
        if ( !sameValues )
            entry.Values.assign( row.begin(), row.end() );
        entry.Revision = revision;
        AskedThisFrame( entry );
        return entry.Row;
    }

    void GpuSceneRegistry::DiscardFrame()
    {
        m_Scene.DiscardFrame();
        for ( auto it = m_Entries.begin(); it != m_Entries.end(); )
        {
            Entry& entry = it->second;
            if ( entry.BonesFrame == m_Frame )
            {
                entry.BoneOffset = entry.PrevBoneOffset;
                entry.BoneCount  = entry.PrevBoneCount;
                entry.BonesFrame = entry.PrevBonesFrame;
            }
            if ( entry.Frame == m_Frame )
                entry.Frame = entry.PrevFrame;
            if ( entry.Frame == kNoFrame ) // added by the discarded frame
            {
                m_Scene.RemovePrimitive( entry.Primitive );
                it = m_Entries.erase( it );
                continue;
            }
            ++it;
        }
        // The static slots as the map's entries, only those the discarded frame stamped one by one (written or
        // folded); the list's stamp rolls back as a whole. GpuScene rolled the frame's transforms back, so no
        // list a caller holds is current any more: KeepStatic refuses until the next SyncStatic.
        for ( const uint32_t index : m_StaticWritten )
        {
            StaticSlot& slot = m_StaticSlots[index];
            if ( !slot.Live )
                continue;
            if ( slot.Held.Frame == m_Frame )
                slot.Held.Frame = slot.Held.PrevFrame;
            if ( slot.Held.Frame == kNoFrame ) // added by the discarded frame
                RemoveStatic( slot );
        }
        m_StaticWritten.clear();
        if ( m_StaticListFrame == m_Frame )
        {
            m_StaticListFrame     = m_StaticListPrevFrame;
            m_StaticListPrevFrame = kNoFrame;
        }
        m_StaticToken = 0;
        for ( auto it = m_MaterialRows.begin(); it != m_MaterialRows.end(); )
        {
            MaterialEntry& entry = it->second;
            if ( entry.Frame == m_Frame )
                entry.Frame = entry.PrevFrame;
            if ( entry.Frame == kNoFrame ) // added by the discarded frame
            {
                m_Scene.RemoveMaterialRow( entry.Row, static_cast<uint32_t>( entry.Values.size() ) );
                it = m_MaterialRows.erase( it );
                continue;
            }
            ++it;
        }
        m_Frame         = m_PreviousFrame;
        m_PreviousFrame = m_FrameBeforePrevious;
        // The restored frame's palette is current again; the discarded one is never read (no entry names it).
        m_FramePalettes.swap( m_PreviousPalettes );
    }

    void GpuSceneRegistry::BeginFrame( const uint64_t frame )
    {
        if ( m_Started && frame == m_Frame )
            return;
        if ( m_Started && !m_Committed )
            DiscardFrame();
        if ( m_Started )
            for ( auto it = m_Entries.begin(); it != m_Entries.end(); )
            {
                if ( it->second.Frame == m_Frame )
                {
                    ++it;
                    continue;
                }
                m_Scene.RemovePrimitive( it->second.Primitive );
                it = m_Entries.erase( it );
            }
        if ( m_Started ) // the static slots the frame that ended did not queue (they left the view or the scene)
            RetireStatic();
        m_StaticWritten.clear();
        if ( m_Started ) // the scratch of entities the frame that ended did not name (they left the view or scene)
            std::erase_if( m_EntityScratch, [this]( const auto& held ) { return held.second.Frame != m_Frame; } );
        if ( m_Started )
            for ( auto it = m_MaterialRows.begin(); it != m_MaterialRows.end(); )
            {
                if ( it->second.Frame == m_Frame )
                {
                    ++it;
                    continue;
                }
                m_Scene.RemoveMaterialRow( it->second.Row, static_cast<uint32_t>( it->second.Values.size() ) );
                it = m_MaterialRows.erase( it );
            }
        m_FrameBeforePrevious = m_PreviousFrame;
        m_PreviousFrame       = m_Frame;
        m_Frame               = frame;
        m_Started             = true;
        m_Committed           = false;
        m_Scene.BeginFrame();
        m_PreviousPalettes.swap( m_FramePalettes );
        m_FramePalettes.clear();
    }

    GpuSceneRegistry::EntityScratch& GpuSceneRegistry::ScratchOf( const uint32_t entity )
    {
        EntityScratch& scratch = m_EntityScratch[entity];
        if ( scratch.Sync != m_SyncSerial ) // the first record of this entity in this Sync: capacity kept
        {
            scratch.Sync  = m_SyncSerial;
            scratch.Frame = m_Frame;
            scratch.Primitives.clear();
            scratch.FieldSets.clear();
        }
        return scratch;
    }

    GpuSceneRegistry::Entry& GpuSceneRegistry::Touch( const MotionKey key, const MotionRecord& record,
                                                      bool& fresh )
    {
        const auto [it, inserted] = m_Entries.try_emplace( key );
        Entry& entry              = it->second;
        fresh                     = inserted || entry.Frame != m_Frame;
        if ( !fresh )
            return entry; // an earlier view of this frame synced it
        Write( entry, inserted, record );
        return entry;
    }

    void GpuSceneRegistry::Write( Entry& entry, const bool inserted, const MotionRecord& record )
    {
        if ( inserted || entry.Mesh != record.Mesh || entry.Geometry != GeometryOf( record.Mesh ) ||
             entry.Flags != record.Flags ||
             entry.ForcedLod != record.ForcedLod || entry.LodBias != record.LodBias )
        {
            PrimitiveDesc desc;
            desc.Mesh         = record.Mesh;
            desc.LocalToWorld = record.World;
            desc.Flags        = record.Flags;
            desc.Entity       = record.Entity;
            desc.ForcedLod    = record.ForcedLod;
            desc.LodBias      = record.LodBias;
            FillBounds( desc, record.Mesh );
            if ( inserted )
                entry.Primitive = m_Scene.AddPrimitive( desc );
            else
                m_Scene.UpdatePrimitive( entry.Primitive, desc );
            entry.Mesh      = record.Mesh;
            entry.Geometry  = GeometryOf( record.Mesh );
            entry.Flags     = record.Flags;
            entry.ForcedLod = record.ForcedLod;
            entry.LodBias   = record.LodBias;
        }
        else
            m_Scene.SetTransform( entry.Primitive, record.World );
        BeginContent( entry, inserted, record.Content );
        Queued( entry );
    }

    void GpuSceneRegistry::RemoveStatic( StaticSlot& slot )
    {
        m_Scene.RemovePrimitive( slot.Held.Primitive );
        slot.Held     = Entry{};
        slot.Live     = false;
        m_StaticToken = 0; // the token's list no longer names the rows its caller holds
    }

    void GpuSceneRegistry::FoldListFrame( const uint32_t index )
    {
        StaticSlot& slot = m_StaticSlots[index];
        if ( !slot.Live || slot.List != m_StaticListSerial || m_StaticListFrame == kNoFrame ||
             slot.Held.Frame == m_StaticListFrame )
            return;
        // Within one list a member's own frame is never later than the list's stamp (SyncStatic writes both).
        slot.Held.PrevFrame = m_StaticListPrevFrame;
        slot.Held.Frame     = m_StaticListFrame;
        if ( slot.Held.Frame == m_Frame )
            m_StaticWritten.push_back( index );
    }

    void GpuSceneRegistry::OrphanStatic( const uint32_t index )
    {
        StaticSlot& slot = m_StaticSlots[index];
        FoldListFrame( index );
        if ( !slot.Live || slot.Orphan )
            return;
        slot.Orphan = true;
        m_StaticOrphans.push_back( index );
    }

    void GpuSceneRegistry::RetireStatic()
    {
        // m_Frame is the frame that ended. A current list it did not queue is dissolved: each member stands on
        // its own frame (an earlier list's stamp folded into it), the ones still queued become orphans.
        if ( !m_StaticTokenSlots.empty() && m_StaticListFrame != m_Frame )
        {
            for ( const uint32_t index : m_StaticTokenSlots )
                OrphanStatic( index );
            m_StaticTokenSlots.clear();
            ++m_StaticListSerial;
            m_StaticListFrame     = kNoFrame;
            m_StaticListPrevFrame = kNoFrame;
            m_StaticToken         = 0;
        }
        std::size_t kept = 0;
        for ( const uint32_t index : m_StaticOrphans )
        {
            StaticSlot& slot = m_StaticSlots[index];
            if ( slot.Live && slot.List != m_StaticListSerial && slot.Held.Frame != m_Frame )
                RemoveStatic( slot );
            if ( !slot.Live || slot.List == m_StaticListSerial ) // gone, or the current list holds it again
            {
                slot.Orphan = false;
                continue;
            }
            m_StaticOrphans[kept++] = index; // queued by the frame that ended outside the list: may lapse later
        }
        m_StaticOrphans.resize( kept );
    }

    bool GpuSceneRegistry::KeepStatic( const uint64_t token )
    {
        if ( token == 0 || token != m_StaticToken )
            return false;
        if ( m_StaticListFrame != m_Frame ) // the first view of this frame to keep it: one stamp, no slot visited
        {
            m_StaticListPrevFrame = m_StaticListFrame;
            m_StaticListFrame     = m_Frame;
        }
        return true;
    }

    void GpuSceneRegistry::SyncStatic( const std::span<const StaticPrimitiveRecord> records, const uint64_t token,
                                       GpuSceneRecordRows& out )
    {
        DESERT_PROFILE_SCOPE( "GpuSceneRegistry::SyncStatic" );
        ++m_SyncSerial;
        out.StaticPrimitives.clear();
        out.StaticInstances.clear();
        out.StaticPrimitives.reserve( records.size() );
        out.StaticInstances.reserve( records.size() );
        // The list this one replaces: each member's frame brought up to the list's stamp, and each a candidate
        // for removal (the new list takes back the ones it names; BeginFrame drops the others no view queued).
        for ( const uint32_t index : m_StaticTokenSlots )
            OrphanStatic( index );
        m_StaticTokenSlots.clear();
        ++m_StaticListSerial;
        m_StaticListFrame     = m_Frame;
        m_StaticListPrevFrame = kNoFrame;
        for ( const StaticPrimitiveRecord& record : records )
        {
            if ( record.Primitive >= m_StaticSlots.size() )
                m_StaticSlots.resize( size_t( record.Primitive ) + 1u );
            StaticSlot& slot = m_StaticSlots[record.Primitive];
            if ( slot.Live && slot.Entity != record.Record.Entity ) // the slot was given to another entity
                RemoveStatic( slot );
            Entry& entry = slot.Held;
            if ( slot.List != m_StaticListSerial ) // the slot's first record in this list
            {
                slot.List = m_StaticListSerial;
                m_StaticTokenSlots.push_back( record.Primitive );
            }
            if ( !slot.Live )
            {
                Write( entry, true, record.Record );
                slot.Live   = true;
                slot.Entity = record.Record.Entity;
                m_StaticWritten.push_back( record.Primitive );
            }
            else if ( entry.Frame != m_Frame ) // the slot's first record this frame
            {
                Write( entry, false, record.Record );
                m_StaticWritten.push_back( record.Primitive );
            }
            else // another section of the slot (AddContent ignores a slot an earlier view of the frame synced)
                AddContent( entry, record.Record.Content );
            out.StaticPrimitives.push_back( entry.Primitive );
            out.StaticInstances.push_back( m_Scene.Primitive( entry.Primitive ).FirstInstance );
        }
        PublishContent();
        m_StaticToken = token;
    }

    void GpuSceneRegistry::Sync( const std::span<const MotionRecord>      rigid,
                                 const std::span<const MotionRecord>      skinned,
                                 const std::span<const MotionFieldRecord> fields, GpuSceneRecordRows& out )
    {
        DESERT_PROFILE_SCOPE( "GpuSceneRegistry::Sync" );
        out.RigidPrimitives.clear();
        out.RigidInstances.clear();
        out.SkinnedPrimitives.clear();
        out.SkinnedInstances.clear();
        out.FieldInstances.clear();
        ++m_SyncSerial;
        out.FieldPrimitives.clear();
        out.RigidPrimitives.reserve( rigid.size() );
        out.RigidInstances.reserve( rigid.size() );
        out.SkinnedPrimitives.reserve( skinned.size() );
        out.SkinnedInstances.reserve( skinned.size() );
        out.FieldInstances.reserve( fields.size() );

        // Per entity, the primitives it owns in this view's submission order: index in the list == Slot
        // (ScratchOf: persistent per-entity scratch, emptied by this Sync's first record of the entity).
        const auto emit = [&]( std::vector<PrimitiveIndex>& primitives, std::vector<InstanceIndex>& instances,
                               const PrimitiveIndex primitive )
        {
            primitives.push_back( primitive );
            instances.push_back( m_Scene.Primitive( primitive ).FirstInstance );
        };

        for ( const MotionRecord& record : rigid )
        {
            if ( record.Part != 0 )
            {
                bool         fresh = false;
                const Entry& entry =
                     Touch( MotionKey{ record.Entity, kMotionPartSlotBit | record.Part }, record, fresh );
                emit( out.RigidPrimitives, out.RigidInstances, entry.Primitive );
                continue;
            }
            auto&          owned     = ScratchOf( record.Entity ).Primitives;
            PrimitiveIndex primitive = kInvalidIndex;
            for ( const Owned& held : owned ) // only rigid primitives exist yet: skinned run last
                if ( m_Scene.Primitive( held.Primitive ).LocalToWorld == PackAffine( record.World ) )
                {
                    primitive = held.Primitive;
                    AddContent( *held.Held, record.Content ); // a section of the same primitive
                    break;
                }
            if ( primitive == kInvalidIndex )
            {
                bool   fresh = false;
                Entry& entry =
                     Touch( MotionKey{ record.Entity, static_cast<uint32_t>( owned.size() ) }, record, fresh );
                primitive = entry.Primitive;
                owned.push_back( Owned{ .Primitive = primitive, .Held = &entry } );
            }
            emit( out.RigidPrimitives, out.RigidInstances, primitive );
        }

        bool palettesChanged = false;
        for ( const MotionRecord& record : skinned )
        {
            auto&  owned = ScratchOf( record.Entity ).Primitives;
            bool   fresh = false;
            Entry& entry =
                 Touch( MotionKey{ record.Entity, static_cast<uint32_t>( owned.size() ) }, record, fresh );
            owned.push_back( Owned{ .Primitive = entry.Primitive, .Held = &entry } );
            if ( entry.BonesFrame != m_Frame )
            {
                // Both palettes go into the ONE buffer every pass that draws the primitive skins it from. The
                // previous one: what this primitive was drawn with in the last synced frame, unless it was not
                // drawn then or its bone count changed (a mesh swap: the old palette indexes other bones).
                // The history is read where the last frame wrote it (m_PreviousPalettes), not from a copy.
                const size_t count   = record.Bones.size();
                const bool   history = entry.BonesFrame == m_PreviousFrame && entry.BonesFrame != kNoFrame &&
                                     entry.BoneCount == count &&
                                     size_t( entry.BoneOffset ) + count <= m_PreviousPalettes.size();
                const uint32_t written = static_cast<uint32_t>( m_FramePalettes.size() );
                m_FramePalettes.insert( m_FramePalettes.end(), record.Bones.begin(), record.Bones.end() );
                entry.PrevOffset = static_cast<uint32_t>( m_FramePalettes.size() );
                if ( history )
                {
                    const auto first =
                         m_PreviousPalettes.begin() + static_cast<std::ptrdiff_t>( entry.BoneOffset );
                    const auto last  = first + static_cast<std::ptrdiff_t>( count );
                    m_FramePalettes.insert( m_FramePalettes.end(), first, last );
                }
                else
                    m_FramePalettes.insert( m_FramePalettes.end(), record.Bones.begin(), record.Bones.end() );
                entry.PrevBoneOffset = entry.BoneOffset;
                entry.PrevBoneCount  = entry.BoneCount;
                entry.BoneOffset     = written;
                entry.BoneCount      = static_cast<uint32_t>( count );
                entry.PrevBonesFrame = entry.BonesFrame;
                entry.BonesFrame     = m_Frame;
                palettesChanged  = true;
                // The skin cache's address is written after Sync (RayTracingSkinCache::PrepareRows).
                m_Scene.SetSkinning( entry.Primitive, PrimitiveSkinning{ .BoneOffset     = entry.BoneOffset,
                                                                         .PrevBoneOffset = entry.PrevOffset } );
            }
            emit( out.SkinnedPrimitives, out.SkinnedInstances, entry.Primitive );
        }
        if ( palettesChanged )
            m_Scene.SetBonePalettes( m_FramePalettes );

        // Per entity, its distinct transform sets in submission order: index == the field's slot.
        for ( const MotionFieldRecord& field : fields )
        {
            auto&      sets  = ScratchOf( field.Entity ).FieldSets;
            const auto found = std::find( sets.begin(), sets.end(), field.Transforms.data() );
            const auto slot  = static_cast<uint32_t>( found - sets.begin() );
            if ( found == sets.end() )
                sets.push_back( field.Transforms.data() );

            const MotionKey key{ field.Entity, kMotionFieldSlotBit | slot };
            const auto [it, inserted] = m_Entries.try_emplace( key );
            Entry& entry              = it->second;
            if ( inserted || entry.Frame != m_Frame )
            {
                // The held snapshot is the instance rows' contents: the same object needs no per-instance compare.
                const bool sameSnapshot =
                     !inserted && field.Snapshot != nullptr && entry.Instances == field.Snapshot;
                const bool changed =
                     inserted || entry.Mesh != field.Mesh || entry.Geometry != GeometryOf( field.Mesh ) ||
                     entry.Flags != field.Flags || entry.BoundsPad != field.BoundsPad ||
                     entry.CullStart != field.CullDistanceStart || entry.CullEnd != field.CullDistanceEnd ||
                     ( !sameSnapshot && !SameInstances( m_Scene.Instances( entry.Primitive ), field.Transforms ) );
                if ( changed )
                {
                    PrimitiveDesc desc;
                    desc.Mesh                = field.Mesh;
                    desc.InstanceToPrimitive = field.Transforms;
                    desc.CullDistanceStart   = field.CullDistanceStart;
                    desc.CullDistanceEnd     = field.CullDistanceEnd;
                    desc.Flags               = field.Flags;
                    desc.Entity              = field.Entity;
                    desc.BoundsPad           = field.BoundsPad; // ISM rows: ForcedLod -1, LodBias 0 (defaults)
                    FillBounds( desc, field.Mesh );
                    if ( inserted )
                        entry.Primitive = m_Scene.AddPrimitive( desc );
                    else
                        m_Scene.UpdatePrimitive( entry.Primitive, desc );
                    entry.Mesh      = field.Mesh;
                    entry.Geometry  = GeometryOf( field.Mesh );
                    entry.Flags     = field.Flags;
                    entry.BoundsPad = field.BoundsPad;
                    entry.CullStart = field.CullDistanceStart;
                    entry.CullEnd   = field.CullDistanceEnd;
                }
                // Written or verified equal just now: the rows hold this snapshot's contents.
                entry.Instances = field.Snapshot != nullptr && field.Snapshot->data() == field.Transforms.data() &&
                                            field.Snapshot->size() == field.Transforms.size()
                                       ? field.Snapshot
                                       : nullptr;
                BeginContent( entry, inserted, field.Content );
                m_Scene.SetWind( entry.Primitive, field.Wind );
                Queued( entry );
            }
            else
                AddContent( entry, field.Content ); // another material slot of the same field
            out.FieldInstances.push_back( m_Scene.Primitive( entry.Primitive ).FirstInstance );
            out.FieldPrimitives.push_back( entry.Primitive );
        }
        PublishContent();
    }
} // namespace Desert::Graphic::GpuDriven
