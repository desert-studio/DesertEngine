#pragma once

#include <optional>
#include <type_traits>

template <typename T>
struct is_image : std::false_type
{
};

template <typename T>
inline constexpr bool is_texture_v = is_image<T>::value;

// optional unwrap
template <typename T>
struct is_image<std::optional<T>> : is_image<T>
{
};