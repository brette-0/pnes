#pragma once
#include <type_traits>

template <typename T>
constexpr auto abs(const T num) -> std::make_unsigned_t<T> {
    using U = std::make_unsigned_t<T>;
    if constexpr (!std::is_signed_v<T>) return num;
    else return num < 0 ? static_cast<U>(0) - static_cast<U>(num) : static_cast<U>(num);
}