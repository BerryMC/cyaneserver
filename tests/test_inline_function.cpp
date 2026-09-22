#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <utility>

#include "cyane/core/inline_function.hpp"
#include "test_framework.hpp"

namespace {

std::atomic<std::uint64_t> g_allocations{0};

void* counted_allocate(std::size_t size, std::size_t alignment) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (alignment <= __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
        if (void* block = std::malloc(size == 0 ? 1 : size)) {
            return block;
        }
    } else {
        if (void* block = std::aligned_alloc(alignment, ((size + alignment - 1) / alignment) * alignment)) {
            return block;
        }
    }
    throw std::bad_alloc{};
}

struct AllocationScope {
    std::uint64_t before{0};
    explicit AllocationScope() noexcept : before{g_allocations.load(std::memory_order_relaxed)} {}
    [[nodiscard]] std::uint64_t count() const noexcept {
        return g_allocations.load(std::memory_order_relaxed) - before;
    }
};

struct Payload40 {
    std::uint64_t words[5]{};
};

}

void* operator new(std::size_t size) { return counted_allocate(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__); }
void* operator new[](std::size_t size) { return counted_allocate(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    return counted_allocate(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return counted_allocate(size, static_cast<std::size_t>(alignment));
}
void operator delete(void* block) noexcept { std::free(block); }
void operator delete[](void* block) noexcept { std::free(block); }
void operator delete(void* block, std::size_t) noexcept { std::free(block); }
void operator delete[](void* block, std::size_t) noexcept { std::free(block); }
void operator delete(void* block, std::align_val_t) noexcept { std::free(block); }
void operator delete[](void* block, std::align_val_t) noexcept { std::free(block); }
void operator delete(void* block, std::size_t, std::align_val_t) noexcept { std::free(block); }
void operator delete[](void* block, std::size_t, std::align_val_t) noexcept { std::free(block); }

CYANE_TEST(inline_function_invokes_small_callable_without_allocating) {
    int captured = 41;
    cyane::InlineFunction<int()> function = [captured] { return captured + 1; };

    const AllocationScope scope;
    const int result = function();
    CYANE_CHECK_EQ(result, 42);
    CYANE_CHECK_EQ(scope.count(), std::uint64_t{0});
    CYANE_CHECK(static_cast<bool>(function));
}

CYANE_TEST(inline_function_keeps_40_byte_capture_inline) {
    Payload40 payload{};
    payload.words[0] = 7;
    payload.words[4] = 11;

    cyane::InlineFunction<std::uint64_t()> function = [payload] { return payload.words[0] + payload.words[4]; };

    const AllocationScope scope;
    CYANE_CHECK_EQ(function(), std::uint64_t{18});
    CYANE_CHECK_EQ(scope.count(), std::uint64_t{0});
}

CYANE_TEST(inline_function_forwards_arguments) {
    cyane::InlineFunction<int(int, int)> add = [](int a, int b) { return a + b; };
    CYANE_CHECK_EQ(add(20, 22), 42);
}

CYANE_TEST(inline_function_returns_void) {
    int target = 0;
    cyane::InlineFunction<void(int)> assign = [&target](int value) { target = value; };
    assign(5);
    CYANE_CHECK_EQ(target, 5);
}

CYANE_TEST(inline_function_carries_move_only_state) {
    cyane::InlineFunction<std::size_t()> function = [owner = std::make_unique<std::string>("cyane")] {
        return owner->size();
    };
    CYANE_CHECK_EQ(function(), std::size_t{5});

    cyane::InlineFunction<std::size_t()> moved = std::move(function);
    CYANE_CHECK_EQ(moved(), std::size_t{5});
    CYANE_CHECK(!static_cast<bool>(function));

    cyane::InlineFunction<std::size_t()> assigned;
    assigned = std::move(moved);
    CYANE_CHECK_EQ(assigned(), std::size_t{5});
    CYANE_CHECK(!static_cast<bool>(moved));
}

CYANE_TEST(inline_function_default_is_empty) {
    cyane::InlineFunction<void()> function;
    CYANE_CHECK(!static_cast<bool>(function));

    function = [] {};
    CYANE_CHECK(static_cast<bool>(function));

    function = nullptr;
    CYANE_CHECK(!static_cast<bool>(function));
}

CYANE_TEST(inline_function_replaces_existing_callable) {
    cyane::InlineFunction<int()> function = [] { return 1; };
    CYANE_CHECK_EQ(function(), 1);
    function = [] { return 2; };
    CYANE_CHECK_EQ(function(), 2);
}
