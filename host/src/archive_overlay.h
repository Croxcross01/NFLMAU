// Archive entry overrides: the game reads an archive (qkl_*.ast, BGFA) as if some entries had been replaced, without
// any file being rewritten. The replacement data is appended past the end of the real file (aligned like the
// archive's own entries) and the entry's table record points there; reads of the table and of the appended part are
// served from memory. Used for the edited playbooks (playbook_mod.cpp).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Replace entry `entry` of every archive named `archive` (file name, any folder, case-insensitive) with `data`
// (stored uncompressed). Call before the game opens the archive.
void OverlaySetEntry(const std::string& archive, uint32_t entry, std::vector<uint8_t> data);
// The replacement for an entry, if one was set (for this program's own readers of the archive).
const std::vector<uint8_t>* OverlayEntry(const std::string& archive, uint32_t entry);

struct ArchiveOverlay;
// The overlay for a file the game opened (nullptr: not an archive with replaced entries).
ArchiveOverlay* OverlayFor(const std::string& hostPath);
uint64_t OverlaySize(ArchiveOverlay* o, uint64_t realSize);
// Reads len bytes at off as the game should see them (real file + replaced table + appended entries).
size_t OverlayRead(ArchiveOverlay* o, FILE* fp, uint64_t off, uint8_t* dst, size_t len);
