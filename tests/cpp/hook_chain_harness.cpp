// Tests for following another mod's hook jump (game/hook_chain.h), on fake memory.
#include "game/hook_chain.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

// Sparse fake memory: a few small regions at fake addresses.
struct FakeMemory {
    struct Region {
        uintptr_t base;
        std::vector<uint8_t> bytes;
    };
    std::vector<Region> regions;

    void map(uintptr_t base, size_t size) { regions.push_back({ base, std::vector<uint8_t>(size, 0xCC) }); }

    void write(uintptr_t address, const std::vector<uint8_t>& data) {
        for (auto& r : regions) {
            if (address >= r.base && address + data.size() <= r.base + r.bytes.size()) {
                std::memcpy(r.bytes.data() + (address - r.base), data.data(), data.size());
                return;
            }
        }
        assert(false && "write outside mapped memory");
    }

    void jmpRel32(uintptr_t at, uintptr_t to) {
        const int32_t rel = static_cast<int32_t>(static_cast<intptr_t>(to) - static_cast<intptr_t>(at + 5));
        std::vector<uint8_t> code{ 0xE9, 0, 0, 0, 0 };
        std::memcpy(code.data() + 1, &rel, sizeof(rel));
        write(at, code);
    }

    // MinHook's x64 relay: jmp [rip+0] followed by the absolute destination.
    void jmpAbsolute(uintptr_t at, uint64_t to) {
        std::vector<uint8_t> code{ 0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        std::memcpy(code.data() + 6, &to, sizeof(to));
        write(at, code);
    }

    auto reader() const {
        return [this](uintptr_t address, void* out, size_t size) {
            for (const auto& r : regions) {
                if (address >= r.base && address + size <= r.base + r.bytes.size()) {
                    std::memcpy(out, r.bytes.data() + (address - r.base), size);
                    return true;
                }
            }
            return false;
        };
    }
};

constexpr uintptr_t kGame = 0x140000000;
constexpr uintptr_t kFunction = kGame + 0x3DC4730;
constexpr uintptr_t kRelay = kGame + 0x7FFF0000;
constexpr uintptr_t kSeasons = 0x7FF800000000;  // Seasons.asi, far from the game
constexpr size_t kSeasonsSize = 0x200000;
constexpr uintptr_t kSeasonsHook = kSeasons + 0x1234;
constexpr uintptr_t kOtherMod = 0x7FF900000000;

}  // namespace

int main() {
    FakeMemory game;
    game.map(kFunction, 0x100);
    game.map(kRelay, 0x100);
    const auto read = game.reader();

    // The game's original bytes: not a hook.
    game.write(kFunction, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57 });
    assert(FollowHookJump(kFunction, read) == 0);
    assert(!HookJumpLeadsInto(kFunction, kSeasons, kSeasonsSize, read));

    // MinHook as Seasons installs it: jmp to a relay near the game, relay to Seasons.asi.
    game.jmpRel32(kFunction, kRelay);
    game.jmpAbsolute(kRelay, kSeasonsHook);
    assert(FollowHookJump(kFunction, read) == kSeasonsHook);
    assert(HookJumpLeadsInto(kFunction, kSeasons, kSeasonsSize, read));

    // The same shape leading into some other module is not Seasons' hook.
    game.jmpAbsolute(kRelay, kOtherMod + 0x10);
    assert(!HookJumpLeadsInto(kFunction, kSeasons, kSeasonsSize, read));

    // Just past the end of the module is outside it.
    game.jmpAbsolute(kRelay, kSeasons + kSeasonsSize);
    assert(!HookJumpLeadsInto(kFunction, kSeasons, kSeasonsSize, read));

    // A relay whose destination slot cannot be read leads nowhere.
    {
        FakeMemory small;
        small.map(kFunction, 16);
        small.jmpRel32(kFunction, kFunction + 8);  // relay at +8, its 8-byte slot runs past the end
        small.write(kFunction + 8, { 0xFF, 0x25, 0, 0, 0, 0 });
        assert(FollowHookJump(kFunction, small.reader()) == 0);
    }

    // A direct jmp into the module (no relay) also counts.
    {
        FakeMemory near;
        near.map(kSeasons - 0x1000, 0x2000);
        near.jmpRel32(kSeasons - 0x100, kSeasons + 0x40);
        assert(HookJumpLeadsInto(kSeasons - 0x100, kSeasons, kSeasonsSize, near.reader()));
    }

    // A jump to itself ends after a bounded number of hops.
    game.jmpRel32(kFunction, kFunction);
    assert(FollowHookJump(kFunction, read) == kFunction);
    assert(!HookJumpLeadsInto(kFunction, kSeasons, kSeasonsSize, read));

    // A null module (Seasons not loaded) never matches.
    game.jmpRel32(kFunction, kRelay);
    game.jmpAbsolute(kRelay, kSeasonsHook);
    assert(!HookJumpLeadsInto(kFunction, 0, kSeasonsSize, read));

    std::cout << "hook chain harness passed\n";
    return 0;
}
