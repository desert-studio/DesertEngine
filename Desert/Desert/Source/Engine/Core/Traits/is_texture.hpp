#pragma once

#include <optional>
#include <type_traits>

template <typename T>
struct is_texture : std::false_type
{
};

template <typename T>
inline constexpr bool is_texture_v = is_texture<T>::value;

// optional unwrap
template <typename T>
struct is_texture<std::optional<T>> : is_texture<T>
{
};