#pragma once
#ifndef BATTERIES_SMALL_FUNCTION_HPP
#define BATTERIES_SMALL_FUNCTION_HPP

#include <batteries/assert.hpp>
#include <batteries/cpu_align.hpp>
#include <batteries/hint.hpp>
#include <batteries/int_types.hpp>
#include <batteries/type_traits.hpp>
#include <batteries/utility.hpp>
#include <type_traits>

namespace batt {

template <typename Signature, usize kMaxSize = kCpuCacheLineSize,
          bool kMoveOnly = false>
class SmallFn;

template <typename Signature, usize kMaxSize = kCpuCacheLineSize>
using UniqueSmallFn = SmallFn<Signature, kMaxSize, /*kMoveOnly=*/true>;

template <typename Result, typename... Args, std::size_t kMaxSize,
          bool kMoveOnly>
class SmallFn<auto(Args...)->Result, kMaxSize, kMoveOnly>
{
    // Base class for abstract function types.  Defines the common
    // interface/concept for move-only and copyable functions:
    // - Invocable (with the signature `auto(Args...)->Result`)
    // - Move-Constructible/Assignable
    // - Destructible
    //
    class AbstractMoveFn;

    // Extends `AbstractMoveFn` with type-erased copy semantics.
    //
    class AbstractCopyFn;

    // The common implementation code for copyable and move-only function type
    // erasures.
    //
    template <typename Fn>
    class MoveFnImpl;

    // Adds copy operations to `MoveFnImpl`.
    //
    template <typename Fn>
    class CopyFnImpl;

    // The type-erased interface to use, depending on the value of `kMoveOnly`.
    //
    using AbstractFn =
        std::conditional_t<kMoveOnly, AbstractMoveFn, AbstractCopyFn>;

    // The concrete type erasure, depending on the value of `kMoveOnly`.
    //
    template <typename Fn>
    using FnImpl =
        std::conditional_t<kMoveOnly, MoveFnImpl<Fn>, CopyFnImpl<Fn>>;

#define BATT_REQUIRE_COPYABLE                                                  \
    static_assert(!kMoveOnly, "This kind of SmallFn is move-only!")

   public:
    using self_type = SmallFn;

    SmallFn() = default;

    template <typename Fn, typename = EnableIfNoShadow<SmallFn, Fn>>
    SmallFn(Fn&& fn) noexcept
        : impl_{new (&storage_) FnImpl<std::decay_t<Fn>>(BATT_FORWARD(fn))}
    {
    }

    SmallFn(const self_type& that) noexcept
        : impl_{[&] {
              BATT_REQUIRE_COPYABLE;
              return BATT_HINT_TRUE(that.impl_ != nullptr)
                         ? that.impl_->copy(&storage_)
                         : nullptr;
          }()}
    {
    }

    SmallFn(self_type&& that) noexcept
        : impl_{[&] {
              auto impl = BATT_HINT_TRUE(that.impl_ != nullptr)
                              ? that.impl_->move(&storage_)
                              : nullptr;
              that.clear();
              return impl;
          }()}
    {
    }

    // TODO [tastolfi 2020-05-29] - Allow a SmallFn to be copied/moved into a
    // UniqueSmallFn.

    ~SmallFn() noexcept
    {
        clear();
    }

    auto clear() noexcept -> void
    {
        if (BATT_HINT_TRUE(impl_ != nullptr)) {
            impl_->~AbstractFn();
            impl_ = nullptr;
        }
    }

    template <typename Fn, typename = EnableIfNoShadow<SmallFn, Fn>>
    auto operator=(Fn&& fn) noexcept -> self_type&
    {
        clear();
        impl_ = new (&storage_) FnImpl<std::decay_t<Fn>>(std::forward<Fn>(fn));
        return *this;
    }

    auto operator=(const self_type& that) noexcept -> self_type&
    {
        BATT_REQUIRE_COPYABLE;

        if (BATT_HINT_TRUE(this != &that)) {
            clear();
            impl_ = BATT_HINT_TRUE(that.impl_ != nullptr)
                        ? that.impl_->copy(&storage_)
                        : nullptr;
        }
        return *this;
    }

    auto operator=(self_type&& that) noexcept -> self_type&
    {
        if (BATT_HINT_TRUE(this != &that)) {
            clear();
            impl_ = BATT_HINT_TRUE(that.impl_ != nullptr)
                        ? that.impl_->move(&storage_)
                        : nullptr;
            that.clear();
        }
        return *this;
    }

    explicit operator bool() const noexcept
    {
        return impl_ != nullptr;
    }

    auto operator()(Args... args) const -> Result
    {
        BATT_ASSERT_NOT_NULLPTR(impl_);
        return impl_->invoke(BATT_FORWARD(args)...);
    }

   private:
    class AbstractMoveFn
    {
       public:
        AbstractMoveFn() = default;

        AbstractMoveFn(const AbstractMoveFn&) = delete;
        AbstractMoveFn& operator=(const AbstractMoveFn&) = delete;

        virtual ~AbstractMoveFn() = default;

        virtual auto invoke(Args... args) noexcept -> Result = 0;
        virtual auto move(void*) noexcept -> AbstractFn* = 0;
    };

    class AbstractCopyFn : public AbstractMoveFn
    {
       public:
        using AbstractMoveFn::AbstractMoveFn;

        virtual auto copy(void*) const noexcept -> AbstractFn* = 0;
    };

    template <typename Fn>
    class MoveFnImpl : public AbstractFn
    {
       public:
        template <typename FnRef>
        explicit MoveFnImpl(FnRef&& ref) : fn_(BATT_FORWARD(ref))
        {
            static_assert(sizeof(MoveFnImpl) <= kMaxSize,
                          "Passed function is not small!");
        }

        Result invoke(Args... args) noexcept override
        {
            return fn_(BATT_FORWARD(args)...);
        }

        AbstractFn* move(void* memory) noexcept override
        {
            return new (memory) FnImpl<Fn>(std::move(fn_));
        }

       protected:
        Fn fn_;
    };

    template <typename Fn>
    class CopyFnImpl : public MoveFnImpl<Fn>
    {
       public:
        using MoveFnImpl<Fn>::MoveFnImpl;

        AbstractFn* copy(void* memory) const noexcept override
        {
            return new (memory) FnImpl<Fn>(this->fn_);
        }
    };

    std::aligned_storage_t<kMaxSize> storage_;  // must come before `impl_`
    AbstractFn* impl_ = nullptr;

#undef BATT_MOVE_ONLY

};  // class SmallFn

}  // namespace batt

#endif  // BATTERIES_SMALL_FUNCTION_HPP
