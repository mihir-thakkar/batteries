#pragma once

#include <iterator>
#include <tuple>
#include <type_traits>
#include <variant>

namespace batt {

// =============================================================================
// IsCallable<Fn, Args...>
//
//  Type alias for std::true_type if `Fn` is callable with `Args...`.
//  Type alias for std::false_type otherwise.
//
namespace detail {

template <typename Fn, typename... Args, typename Result = std::result_of_t<Fn(Args...)>>
std::true_type is_callable_impl(void*);

template <typename Fn, typename... Args>
std::false_type is_callable_impl(...);

}  // namespace detail

template <typename Fn, typename... Args>
using IsCallable = decltype(detail::is_callable_impl<Fn, Args...>(nullptr));

// =============================================================================
// IsRange<T>
//
//  Type alias for std::true_type if `T` is a range type.
//  Type alias for std::false_type otherwise.
//
namespace detail {

template <typename T, typename BeginIter = decltype(std::begin(std::declval<T>())),
          typename EndIter = decltype(std::end(std::declval<T>())),
          typename = std::enable_if_t<std::is_same<BeginIter, EndIter>{}>>
std::true_type is_range_impl(void*);

template <typename T>
std::false_type is_range_impl(...);

}  // namespace detail

template <typename T>
using IsRange = decltype(detail::is_range_impl<T>(nullptr));

// =============================================================================
// IsVariant<T>
//
//  Derives std::true_type if `T` is a std::variant type.
//  Derives std::false_type otherwise.
//
template <typename T>
struct IsVariant : std::false_type {
};

template <typename... Ts>
struct IsVariant<std::variant<Ts...>> : std::true_type {
};

// =============================================================================
// IsTuple<T>
//
//  Derives std::true_type if `T` is a std::tuple type.
//  Derives std::false_type otherwise.
//
template <typename T>
struct IsTuple : std::false_type {
};

template <typename... Ts>
struct IsTuple<std::tuple<Ts...>> : std::true_type {
};

// =============================================================================
// Enables/disables a constructor template when the argments do not cause it to
// shadow a built-in method.
//
// Usage:
//
// ```
// class MyType {
// public:
//   template <typename... Args, typename = batt::EnableIfNoShadow<MyType, Args...>>
//   MyType(Args&&... args) { /* something other than copy/move/default */ }
// };
// ```
//
template <typename T, typename... Args>
using EnableIfNoShadow = std::enable_if_t<
    !std::is_same<std::tuple<T>, std::tuple<std::decay_t<Args>...>>{}    // Copy or move ctor
    && !std::is_same<std::tuple<>, std::tuple<std::decay_t<Args>...>>{}  // Default ctor
    >;

}  // namespace batt
