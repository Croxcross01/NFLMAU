// Playbook editing core, shared by the game (madden_host) and the dev tools' Playbooks tab.
//
// The game's playbooks are EA TDB databases (big-endian, bit-packed records):
//   offense: qkl_boot.ast entry 82 (front end) and qkl_fe2ig.ast entry 261 (in a match; the same data)
//   defense: qkl_fe2ig.ast entry 260
// Tables (Madden 08 era format): PBFM formations / PBST sets / PBPL playbook entries (name, set, order) -> PLYL plays
// (type, set layout SETL) -> PLYS one row per player slot (poso 0-10: PSAL script, ARTL chalkboard art) -> PSAL script
// steps (code, val1..3, step order; code 255 ends). SETP/SETG = where each slot lines up, PLPD = pass icon receivers,
// PBAI = which plays the CPU calls.
//
// Edits are kept as a small text file (mods/playbook_edits.txt) of operations applied to the player's own game data,
// so no game data is ever stored or shipped. One operation per line (# starts a comment):
//   book off|def                                     which playbook the following lines change
//   copy <PBPL> <newPLYL> <newPBPL> <PBST> <name>    new play: a copy of an existing one, in set PBST, called name
//   name <PBPL> <name>                               rename a play (what the play-call screen shows)
//   move <PBPL> <PBST> <ord>                         put a play in another set / position
//   remove <PBPL>                                    take a play out of the playbook (and out of the CPU's choices)
//   steps <PLYL> <poso> c,v1,v2,v3;c,v1,v2,v3;...    a player's assignment (the 255 end step is added)
//   art <PLYL> <poso> x,y,ct;x,y,ct;...              his chalkboard line (points relative to him; ct 0 line, 1 block, 2 arrow)
//   field <TABLE> <key>=<v>[,<key>=<v>] <f>=<v> ...  any other field of matching rows (e.g. PLYL PLYT, PLPD icons)
// Names are quoted ("Short Pass 2"). Steps and art get new script / art ids, so plays sharing a script are not changed.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace pb {

struct Field { std::string name; uint32_t type = 0, bit = 0, bits = 0; };

struct Table {
    std::string name;                 // 4 characters (as shown, e.g. "PLYL"; "SGF" has a trailing NUL)
    uint8_t header[40] = {};          // kept as read (capacity / count rewritten on build)
    std::vector<uint8_t> fieldDefs;   // kept as read
    std::vector<Field> fields;
    uint32_t rb = 0;                  // record bytes
    uint32_t capacity = 0;
    std::vector<std::vector<uint8_t>> rows;
    std::vector<uint8_t> after;       // bytes between this table's records and the next table (normally none)

    int F(const std::string& n) const;                       // field index, -1 if none
    uint32_t Get(size_t row, int f) const;
    uint32_t Get(size_t row, const char* n) const { int f = F(n); return f < 0 ? 0 : Get(row, f); }
    std::string Str(size_t row, int f) const;
    bool Set(size_t row, int f, uint32_t v);                 // false if v does not fit the field
    bool Set(size_t row, const char* n, uint32_t v) { int f = F(n); return f >= 0 && Set(row, f, v); }
    bool SetStr(size_t row, int f, const std::string& s);
    size_t AddRow() { rows.emplace_back(rb, 0); return rows.size() - 1; }
};

struct Db {
    std::vector<uint8_t> head;        // 24-byte header
    std::vector<Table> tables;        // in the order they are stored
    std::vector<int> indexOrder;      // table index entries (positions into tables), as in the file
    std::vector<uint8_t> trailer;     // bytes after the last table (before the final checksum)
    bool Parse(const std::vector<uint8_t>& d, std::string& err);
    std::vector<uint8_t> Build() const;   // with fresh checksums
    Table* T(const char* n);
    const Table* T(const char* n) const;
};

// header u32 @20 = crc(bytes 0..20); each table header u32 @36 = crc(its bytes 4..36); table[0] leading u32 =
// crc(table index); table[i] leading u32 = crc(table[i-1] from +40 to table[i]); after the last table its crc.
void FixChecksums(std::vector<uint8_t>& db);

// One step of a player's script.
struct Step { uint32_t code = 255, v1 = 0, v2 = 0, v3 = 0; };
// One point of a chalkboard line, relative to the player's spot on the chalkboard (8-bit signed units).
struct ArtPt { int x = 0, y = 0; uint32_t ct = 0; };

// Applies an edits file to the offense and defense playbooks. log gets one line per problem (bad line, unknown id).
// Returns the number of operations applied.
int ApplyEdits(Db& off, Db& def, const std::string& text, std::string& log);
// The same for one book (the lines after "book off" / "book def" that match it).
int ApplyOne(Db& db, const std::string& line, std::string& log);

// Reading helpers used by the editor and the play list.
std::vector<Step> StepsOf(const Db& db, uint32_t psal);
std::vector<ArtPt> ArtOf(const Db& db, uint32_t artl);
int SignedField(uint32_t v, uint32_t bits);   // two's complement value of a bits-wide field (art uses 8 bits)

// Names of the step codes (Madden 08 PSAL codes; the arcade game uses the same numbers).
const char* StepName(uint32_t code);

}   // namespace pb
