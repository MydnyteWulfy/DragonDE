#pragma once

#include <type_traits>

// Operators for protocol enums marked bitfield="true" in the XML.
// dragon-scanner specializes IsBitfield for each one.
namespace dragon::proto {

template <typename E>
struct IsBitfield : std::false_type {};

template <typename E>
concept BitfieldEnum = std::is_enum_v<E> && IsBitfield<E>::value;

template <BitfieldEnum E>
constexpr E operator|(E a, E b)
{
    using U = std::underlying_type_t<E>;
    return E(U(a) | U(b));
}

template <BitfieldEnum E>
constexpr E operator&(E a, E b)
{
    using U = std::underlying_type_t<E>;
    return E(U(a) & U(b));
}

template <BitfieldEnum E>
constexpr E operator~(E a)
{
    using U = std::underlying_type_t<E>;
    return E(~U(a));
}

template <BitfieldEnum E>
constexpr E& operator|=(E& a, E b)
{
    return a = a | b;
}

template <BitfieldEnum E>
constexpr E& operator&=(E& a, E b)
{
    return a = a & b;
}

// True if every bit of `flags` is set in `set`.
template <BitfieldEnum E>
constexpr bool hasFlags(E set, E flags)
{
    using U = std::underlying_type_t<E>;
    return (U(set) & U(flags)) == U(flags);
}

} // namespace dragon::proto
