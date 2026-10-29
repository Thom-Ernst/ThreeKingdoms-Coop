#pragma once
// #105: dismissal storage for the 3rd and 4th human. Pure bookkeeping, no engine access, so the
// offline test compiles exactly this. eventdismiss.cpp owns the hooks, the lock and the reads.
//
// The engine keeps DISMISSED as two bits (11-12) in each 16-bit event-log word, one per registry
// index, and its handler FUN_1414CEDF0 returns early for index 2 or 3. The table stores what that
// handler refused, keyed by event-manager ownership, FACTION pointer (never seat position) and
// log index. Log indices are only stable between erases, so every erase is replayed onto the
// table, and a message is answered only while its generation still names the current numbering.
//
// A sorted fixed array rather than std containers: no allocation on the game thread, and no
// <cmath> dragged into a file the release-log audit scans for `logf(` calls.
#include <cstdint>
#include <cstddef>

namespace eventdismiss {

// Far more than many hundreds of turns of 3rd/4th-seat dismissals that compaction never pruned.
// Past it the table refuses new entries: the event stays dismissable-but-returning (vanilla).
constexpr size_t maxEntries = 8192;

// One entry per (faction, log index); the flags say what the engine refused to store for it.
// READ is the same two-human gate as DISMISSED (FUN_1414CEEB0, bits 13-14), and the #56 auto-open
// check needs it: without a stored read, a dismissed event that left the feed list reopens.
enum : uint32_t { kDismissed = 1u, kRead = 2u };
struct Entry { uintptr_t faction; uint32_t index; uint32_t flags; };

struct Table {
    uintptr_t mgr = 0;
    bool eraseSeen = false;
    uint32_t eraseGeneration = 0;   // manager generation during the latest erase
    size_t entries = 0;
    Entry items[maxEntries] = {};   // sorted by (faction, index)

    void reset() { mgr = 0; eraseSeen = false; eraseGeneration = 0; entries = 0; }
    bool empty() const { return entries == 0; }

    size_t lowerBound(uintptr_t faction, uint32_t index) const {
        size_t lo = 0, hi = entries;
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            const Entry& e = items[mid];
            if (e.faction < faction || (e.faction == faction && e.index < index)) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }
    bool contains(uintptr_t faction, uint32_t index, uint32_t mask = kDismissed) const {
        const size_t at = lowerBound(faction, index);
        return at < entries && items[at].faction == faction && items[at].index == index &&
               (items[at].flags & mask) != 0;
    }

    // A READ or DISMISSED the engine accepted the COMMAND for but refused to store. `generation`
    // is the command's; the engine itself only applies one whose generation is current.
    enum class Record { Stored, Duplicate, Stale, Full };
    Record record(uintptr_t manager, uintptr_t faction, uint32_t generation, uint32_t index,
                  uint32_t currentGeneration, uint32_t flag = kDismissed) {
        if (!manager || !faction || generation != currentGeneration) return Record::Stale;
        if (manager != mgr) { reset(); mgr = manager; }
        // An erase during this generation renumbered the log after the command was built.
        if (eraseSeen && eraseGeneration == currentGeneration) return Record::Stale;
        const size_t at = lowerBound(faction, index);
        if (at < entries && items[at].faction == faction && items[at].index == index) {
            if (items[at].flags & flag) return Record::Duplicate;
            items[at].flags |= flag;
            return Record::Stored;
        }
        if (entries >= maxEntries) return Record::Full;
        for (size_t i = entries; i > at; --i) items[i] = items[i - 1];
        items[at] = Entry{ faction, index, flag };
        ++entries;
        return Record::Stored;
    }

    // FUN_14149F970(log, start, len): [start, start+len) removed, everything after shifted down.
    // Every survivor of one faction moves by the same amount, so the order is preserved.
    void erase(uintptr_t manager, uint32_t start, uint32_t len, uint32_t currentGeneration) {
        if (!manager || manager != mgr || !len) return;
        eraseSeen = true;
        eraseGeneration = currentGeneration;
        const uint64_t end = uint64_t(start) + len;
        size_t kept = 0;
        for (size_t i = 0; i < entries; ++i) {
            Entry e = items[i];
            if (e.index >= start && uint64_t(e.index) < end) continue;
            if (uint64_t(e.index) >= end) e.index -= len;
            items[kept++] = e;
        }
        entries = kept;
    }

    // A materialized message stamps the generation it was built in (+0x88) beside its log index
    // (+0x8C). Answer only for the current numbering; anything else falls back to the engine.
    bool query(uintptr_t manager, uintptr_t faction, uint32_t generation, uint32_t index,
               uint32_t currentGeneration, uint32_t mask = kDismissed) const {
        if (!manager || manager != mgr || generation != currentGeneration) return false;
        if (eraseSeen && eraseGeneration == currentGeneration) return false;
        return contains(faction, index, mask);
    }

    bool knowsFaction(uintptr_t faction) const {
        const size_t at = lowerBound(faction, 0);
        return at < entries && items[at].faction == faction;
    }
};

}
