#pragma once

#include <cstddef>
#include <functional>
#include <new>
#include <type_traits>
#include <utility>

namespace cyane {

// 小对象内联的可移动 callable：避免任务提交路径上的堆分配
template <typename Signature, std::size_t Capacity = 48>
class InlineFunction;

template <typename R, typename... Args, std::size_t Capacity>
class InlineFunction<R(Args...), Capacity> {
    using InvokeFn = R (*)(void*, Args&&...);
    using MoveFn = void (*)(void*, void*) noexcept;
    using DestroyFn = void (*)(void*) noexcept;

public:
    InlineFunction() noexcept = default;
    InlineFunction(std::nullptr_t) noexcept {}

    template <typename F>
        requires(!std::same_as<std::remove_cvref_t<F>, InlineFunction> &&
                 std::is_invocable_r_v<R, std::remove_cvref_t<F>&, Args...>)
    InlineFunction(F&& f) {
        emplace(std::forward<F>(f));
    }

    InlineFunction(InlineFunction&& other) noexcept { take_over(other); }

    InlineFunction& operator=(InlineFunction&& other) noexcept {
        if (this != &other) {
            destroy();
            take_over(other);
        }
        return *this;
    }

    InlineFunction(const InlineFunction&) = delete;
    InlineFunction& operator=(const InlineFunction&) = delete;

    ~InlineFunction() { destroy(); }

    [[nodiscard]] explicit operator bool() const noexcept { return invoke_ != nullptr; }

    R operator()(Args... args) {
        return invoke_(storage(), std::forward<Args>(args)...);
    }

private:
    template <typename F>
    void emplace(F&& f) {
        using T = std::remove_cvref_t<F>;
        static_assert(sizeof(T) <= Capacity, "callable exceeds inline storage");
        static_assert(alignof(T) <= alignof(std::max_align_t), "callable over-aligned");
        static_assert(std::is_nothrow_move_constructible_v<T>, "callable must be nothrow movable");
        ::new (static_cast<void*>(storage_)) T(std::forward<F>(f));
        invoke_ = [](void* s, Args&&... args) -> R {
            return std::invoke(*static_cast<T*>(s), std::forward<Args>(args)...);
        };
        move_ = [](void* dst, void* src) noexcept { ::new (dst) T(std::move(*static_cast<T*>(src))); };
        destroy_ = [](void* s) noexcept { static_cast<T*>(s)->~T(); };
    }

    void take_over(InlineFunction& other) noexcept {
        if (other.invoke_ == nullptr) {
            return;
        }
        other.move_(storage_, other.storage());
        invoke_ = other.invoke_;
        move_ = other.move_;
        destroy_ = other.destroy_;
        other.destroy();
        other.invoke_ = nullptr;
    }

    void destroy() noexcept {
        if (invoke_ != nullptr) {
            destroy_(storage());
            invoke_ = nullptr;
        }
    }

    [[nodiscard]] void* storage() noexcept { return static_cast<void*>(storage_); }

    alignas(std::max_align_t) std::byte storage_[Capacity]{};
    InvokeFn invoke_{nullptr};
    MoveFn move_{nullptr};
    DestroyFn destroy_{nullptr};
};

}
