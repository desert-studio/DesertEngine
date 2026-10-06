#include <Engine/Graphic/View/SceneViewState.hpp>

#include <utility>

namespace Desert::Graphic
{
    glm::mat4 MotionHistory::PreviousTransform( const MotionKey key, const glm::mat4& current )
    {
        // try_emplace keeps the first record of the frame: a second call for the same key changes nothing.
        (void)m_CurTransforms.try_emplace( key, current );

        const auto previous = m_PrevTransforms.find( key );
        return previous != m_PrevTransforms.end() ? previous->second : current;
    }

    std::span<const glm::mat4> MotionHistory::PreviousBones( const MotionKey            key,
                                                             std::span<const glm::mat4> current )
    {
        const auto record = m_CurBones.try_emplace( key, current.begin(), current.end() ).first;

        // Compare against the palette recorded this frame (the first one), so a second call returns what the
        // first returned.
        const std::vector<glm::mat4>& recorded = record->second;
        const auto                    previous = m_PrevBones.find( key );
        if ( previous != m_PrevBones.end() && previous->second.size() == recorded.size() )
            return previous->second;
        // Not drawn last frame, or another bone count (a mesh swap): no object motion. The returned span points
        // into this frame's record, which lives until EndFrame like the previous palettes do.
        return recorded;
    }

    void MotionHistory::EndFrame()
    {
        m_PrevTransforms = std::move( m_CurTransforms );
        m_CurTransforms.clear();
        m_PrevBones = std::move( m_CurBones );
        m_CurBones.clear();
    }

    void MotionHistory::DiscardCurrent()
    {
        m_CurTransforms.clear();
        m_CurBones.clear();
    }

    void MotionHistory::Clear()
    {
        m_PrevTransforms.clear();
        m_CurTransforms.clear();
        m_PrevBones.clear();
        m_CurBones.clear();
    }

    std::size_t MotionHistory::TrackedTransforms() const
    {
        return m_PrevTransforms.size();
    }

    std::size_t MotionHistory::TrackedPalettes() const
    {
        return m_PrevBones.size();
    }
} // namespace Desert::Graphic
