#include <Engine/Graphic/View/ObjectMotionRows.hpp>

#include <unordered_map>

namespace Desert::Graphic
{
    void BuildObjectMotionRows( MotionHistory& motion, const std::span<const MotionRecord> rigid,
                                const std::span<const MotionRecord> skinned, ObjectMotionRows& out )
    {
        out.Clear();
        out.RecordRows.reserve( rigid.size() + skinned.size() );

        // Per entity, the rows it owns this frame in submission order: index in the list == MotionKey::Slot.
        std::unordered_map<uint32_t, std::vector<uint32_t>> entityRows;
        entityRows.reserve( rigid.size() + skinned.size() );

        const auto newRow = [&]( const uint32_t entity, const glm::mat4& world ) -> uint32_t
        {
            auto&           owned = entityRows[entity];
            const MotionKey key{ entity, static_cast<uint32_t>( owned.size() ) };
            GpuObjectMotion row;
            row.World     = world;
            row.PrevWorld = motion.PreviousTransform( key, world );
            owned.push_back( static_cast<uint32_t>( out.Rows.size() ) );
            out.Rows.push_back( row );
            return owned.back();
        };

        for ( const MotionRecord& record : rigid )
        {
            uint32_t index = UINT32_MAX;
            if ( const auto it = entityRows.find( record.Entity ); it != entityRows.end() )
                for ( const uint32_t owned : it->second )
                    if ( out.Rows[owned].World == record.World ) // only rigid rows exist yet: skinned run last
                    {
                        index = owned;
                        break;
                    }
            out.RecordRows.push_back( index != UINT32_MAX ? index : newRow( record.Entity, record.World ) );
        }

        for ( const MotionRecord& record : skinned )
        {
            const uint32_t index = newRow( record.Entity, record.World );
            const uint32_t slot  = static_cast<uint32_t>( entityRows[record.Entity].size() - 1 );
            out.RecordRows.push_back( index );
            // Both palettes go into the ONE buffer every pass that draws the primitive skins it from.
            out.Rows[index].BoneOffset = static_cast<uint32_t>( out.Palettes.size() );
            out.Palettes.insert( out.Palettes.end(), record.Bones.begin(), record.Bones.end() );
            const std::span<const glm::mat4> previous =
                 motion.PreviousBones( MotionKey{ record.Entity, slot }, record.Bones );
            out.Rows[index].PrevBoneOffset = static_cast<uint32_t>( out.Palettes.size() );
            out.Palettes.insert( out.Palettes.end(), previous.begin(), previous.end() );
        }
    }
} // namespace Desert::Graphic
