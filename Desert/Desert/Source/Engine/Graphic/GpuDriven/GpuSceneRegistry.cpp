#include <Engine/Graphic/GpuDriven/GpuSceneRegistry.hpp>

#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Geometry/MeshBounds.hpp>

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

        bool SameInstances( const std::span<const GpuSceneInstance> held,
                            const std::span<const glm::mat4>        transforms )
        {
            if ( held.size() != std::max<size_t>( transforms.size(), 1u ) )
                return false;
            for ( size_t i = 0; i < transforms.size(); ++i )
                if ( held[i].InstanceToPrimitive != transforms[i] )
                    return false;
            return true;
        }
    } // namespace

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

    void GpuSceneRegistry::DiscardFrame()
    {
        m_Scene.DiscardFrame();
        for ( auto it = m_Entries.begin(); it != m_Entries.end(); )
        {
            Entry& entry = it->second;
            if ( entry.BonesFrame == m_Frame )
            {
                entry.Bones.swap( entry.PrevBones );
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
        m_Frame         = m_PreviousFrame;
        m_PreviousFrame = m_FrameBeforePrevious;
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
        m_FrameBeforePrevious = m_PreviousFrame;
        m_PreviousFrame       = m_Frame;
        m_Frame               = frame;
        m_Started             = true;
        m_Committed           = false;
        m_Scene.BeginFrame();
        m_FramePalettes.clear();
    }

    GpuSceneRegistry::Entry& GpuSceneRegistry::Touch( const MotionKey key, const MotionRecord& record,
                                                      bool& fresh )
    {
        const auto [it, inserted] = m_Entries.try_emplace( key );
        Entry& entry              = it->second;
        fresh                     = inserted || entry.Frame != m_Frame;
        if ( !fresh )
            return entry; // an earlier view of this frame synced it
        if ( inserted || entry.Mesh != record.Mesh || entry.Flags != record.Flags ||
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
            entry.Flags     = record.Flags;
            entry.ForcedLod = record.ForcedLod;
            entry.LodBias   = record.LodBias;
        }
        else
            m_Scene.SetTransform( entry.Primitive, record.World );
        Queued( entry );
        return entry;
    }

    void GpuSceneRegistry::Sync( const std::span<const MotionRecord>      rigid,
                                 const std::span<const MotionRecord>      skinned,
                                 const std::span<const MotionFieldRecord> fields, GpuSceneRecordRows& out )
    {
        out.RigidPrimitives.clear();
        out.RigidInstances.clear();
        out.SkinnedPrimitives.clear();
        out.SkinnedInstances.clear();
        out.FieldInstances.clear();
        out.RigidPrimitives.reserve( rigid.size() );
        out.RigidInstances.reserve( rigid.size() );
        out.SkinnedPrimitives.reserve( skinned.size() );
        out.SkinnedInstances.reserve( skinned.size() );
        out.FieldInstances.reserve( fields.size() );

        // Per entity, the primitives it owns in this view's submission order: index in the list == Slot.
        std::unordered_map<uint32_t, std::vector<PrimitiveIndex>> entityPrimitives;
        entityPrimitives.reserve( rigid.size() + skinned.size() );
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
            auto&          owned     = entityPrimitives[record.Entity];
            PrimitiveIndex primitive = kInvalidIndex;
            for ( const PrimitiveIndex held : owned ) // only rigid primitives exist yet: skinned run last
                if ( m_Scene.Primitive( held ).LocalToWorld == record.World )
                {
                    primitive = held;
                    break;
                }
            if ( primitive == kInvalidIndex )
            {
                bool fresh = false;
                primitive =
                     Touch( MotionKey{ record.Entity, static_cast<uint32_t>( owned.size() ) }, record, fresh )
                          .Primitive;
                owned.push_back( primitive );
            }
            emit( out.RigidPrimitives, out.RigidInstances, primitive );
        }

        bool palettesChanged = false;
        for ( const MotionRecord& record : skinned )
        {
            auto&  owned = entityPrimitives[record.Entity];
            bool   fresh = false;
            Entry& entry =
                 Touch( MotionKey{ record.Entity, static_cast<uint32_t>( owned.size() ) }, record, fresh );
            owned.push_back( entry.Primitive );
            if ( entry.BonesFrame != m_Frame )
            {
                // Both palettes go into the ONE buffer every pass that draws the primitive skins it from. The
                // previous one: what this primitive was drawn with in the last synced frame, unless it was not
                // drawn then or its bone count changed (a mesh swap: the old palette indexes other bones).
                const bool history = entry.BonesFrame == m_PreviousFrame && entry.BonesFrame != kNoFrame &&
                                     entry.Bones.size() == record.Bones.size();
                entry.BoneOffset = static_cast<uint32_t>( m_FramePalettes.size() );
                m_FramePalettes.insert( m_FramePalettes.end(), record.Bones.begin(), record.Bones.end() );
                entry.PrevOffset = static_cast<uint32_t>( m_FramePalettes.size() );
                if ( history )
                    m_FramePalettes.insert( m_FramePalettes.end(), entry.Bones.begin(), entry.Bones.end() );
                else
                    m_FramePalettes.insert( m_FramePalettes.end(), record.Bones.begin(), record.Bones.end() );
                entry.PrevBones.swap( entry.Bones );
                entry.Bones.assign( record.Bones.begin(), record.Bones.end() );
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
        std::unordered_map<uint32_t, std::vector<const glm::mat4*>> entityFields;
        for ( const MotionFieldRecord& field : fields )
        {
            auto&      sets  = entityFields[field.Entity];
            const auto found = std::find( sets.begin(), sets.end(), field.Transforms.data() );
            const auto slot  = static_cast<uint32_t>( found - sets.begin() );
            if ( found == sets.end() )
                sets.push_back( field.Transforms.data() );

            const MotionKey key{ field.Entity, kMotionFieldSlotBit | slot };
            const auto [it, inserted] = m_Entries.try_emplace( key );
            Entry& entry              = it->second;
            if ( inserted || entry.Frame != m_Frame )
            {
                const bool changed = inserted || entry.Mesh != field.Mesh || entry.Flags != field.Flags ||
                                     entry.BoundsPad != field.BoundsPad || entry.CullStart != field.CullDistanceStart ||
                                     entry.CullEnd != field.CullDistanceEnd ||
                                     !SameInstances( m_Scene.Instances( entry.Primitive ), field.Transforms );
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
                    entry.Flags     = field.Flags;
                    entry.BoundsPad = field.BoundsPad;
                    entry.CullStart = field.CullDistanceStart;
                    entry.CullEnd   = field.CullDistanceEnd;
                }
                Queued( entry );
            }
            out.FieldInstances.push_back( m_Scene.Primitive( entry.Primitive ).FirstInstance );
        }
    }
} // namespace Desert::Graphic::GpuDriven
