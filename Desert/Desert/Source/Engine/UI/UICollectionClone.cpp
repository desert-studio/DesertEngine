#include "UIDataStore.hpp"

namespace Desert::UI
{
    // Alone in this file on purpose: see the declaration. Copying the records copies each record's store,
    // whose own collections come back here through UIDataStore's copy constructor (UIDataStore.cpp).
    std::unique_ptr<UICollection> UICollection::Clone() const
    {
        return std::make_unique<UICollection>( *this );
    }
} // namespace Desert::UI
