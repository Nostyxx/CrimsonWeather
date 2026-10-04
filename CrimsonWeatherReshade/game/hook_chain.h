#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Following another mod's hook on a game function.
//
// A hook library such as MinHook replaces the first instruction of the hooked
// function with `E9 rel32` (jmp). On x64 that jump usually lands on a relay
// stub next to the game, `FF 25 00000000` followed by the absolute address of
// the hook (jmp [rip+0]), because the hook's module is too far away for rel32.
//
// FollowHookJump returns where those jumps end up, or 0 when `at` does not
// start with a jump. `read(address, out, size)` must return false instead of
// crashing on unreadable memory.
template <typename ReadFn>
uintptr_t FollowHookJump(uintptr_t at, ReadFn read) {
    constexpr int kMaxHops = 4;
    uintptr_t current = at;
    for (int hop = 0; hop < kMaxHops; ++hop) {
        uint8_t code[6] = {};
        if (!read(current, code, sizeof(code))) {
            return hop ? current : 0;
        }
        if (code[0] == 0xE9) {
            int32_t rel = 0;
            std::memcpy(&rel, code + 1, sizeof(rel));
            current = current + 5 + static_cast<intptr_t>(rel);
        } else if (code[0] == 0xFF && code[1] == 0x25) {
            int32_t disp = 0;
            std::memcpy(&disp, code + 2, sizeof(disp));
            uint64_t destination = 0;
            if (!read(current + 6 + static_cast<intptr_t>(disp), &destination, sizeof(destination))) {
                return 0;
            }
            current = static_cast<uintptr_t>(destination);
        } else {
            return hop ? current : 0;
        }
    }
    return current;
}

// Whether `at` starts with a hook jump that ends inside [moduleBase, moduleBase + moduleSize).
template <typename ReadFn>
bool HookJumpLeadsInto(uintptr_t at, uintptr_t moduleBase, size_t moduleSize, ReadFn read) {
    const uintptr_t destination = FollowHookJump(at, read);
    return destination != 0 && moduleBase != 0 && destination >= moduleBase && destination - moduleBase < moduleSize;
}
