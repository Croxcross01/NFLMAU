"""Reader for EA "TDB" databases (header 'DB' 0x08) as used by Madden NFL Arcade's roster files.

Layout (big-endian on Xbox 360):
  header 24 bytes: 'DB' u16 ver, u16 flags, u32 size, u32 0, u32 table count, u32 crc
  table index: count x {char[4] name (byte-reversed), u32 offset from end of index}
  table: 40-byte header {u32 crc, u32 ?, u32 record bytes, u32 record bits, u32 0, u16 capacity, u16 used,
         u32 ?, u8 field count, ...}, then field count x 16-byte field defs {u32 type, u32 bit offset,
         char[4] name (reversed), u32 bit count}, then records.
Field types: 0 = string (bits/8 bytes, NUL padded); integer types are unsigned bit fields.
Player conventions: PWGT is weight - 160 lb, PHGT is height in inches, PPOS indexes POSITIONS below.
Only reads the user's own game files; no data from them is stored in this repository.
"""
import struct, sys

POSITIONS = ['QB', 'HB', 'FB', 'WR', 'TE', 'LT', 'LG', 'C', 'RG', 'RT', 'LE', 'RE', 'DT', 'LOLB', 'MLB', 'ROLB',
             'CB', 'FS', 'SS', 'K', 'P']


class Table:
    def __init__(self, name, data, at):
        self.name = name
        h = data[at:at + 40]
        self.record_bytes = struct.unpack('>I', h[8:12])[0]
        self.capacity, self.used = struct.unpack('>HH', h[20:24])
        nfields = h[28]
        self.fields = []
        p = at + 40
        for _ in range(nfields):
            ftype, bitoff, nm, bits = struct.unpack('>I I 4s I', data[p:p + 16])
            self.fields.append((nm[::-1].decode('latin1'), ftype, bitoff, bits))
            p += 16
        self.fields.sort(key=lambda f: f[2])
        self.records_at = p
        self.data = data

    def record(self, i):
        rb = self.data[self.records_at + i * self.record_bytes: self.records_at + (i + 1) * self.record_bytes]
        val = int.from_bytes(rb, 'big')
        total = len(rb) * 8
        out = {}
        for name, ftype, bitoff, bits in self.fields:
            if ftype == 0:
                s = rb[bitoff // 8: bitoff // 8 + bits // 8]
                out[name] = s.split(b'\0', 1)[0].decode('latin1')
            else:
                out[name] = (val >> (total - bitoff - bits)) & ((1 << bits) - 1)
        return out

    def rows(self):
        return [self.record(i) for i in range(self.used)]


_CRC_TABLE = []
for _i in range(256):
    _c = _i << 24
    for _ in range(8):
        _c = ((_c << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if _c & 0x80000000 else (_c << 1) & 0xFFFFFFFF
    _CRC_TABLE.append(_c)


def crc32_msb(b):
    """CRC-32, MSB-first, polynomial 0x04C11DB7, init 0xFFFFFFFF, no final xor (EA TDB)."""
    c = 0xFFFFFFFF
    for x in b:
        c = ((c << 8) & 0xFFFFFFFF) ^ _CRC_TABLE[((c >> 24) ^ x) & 0xFF]
    return c


def fix_checksums(data):
    """Recompute the TDB checksum chain in place (bytearray). The game rejects a database whose checksums are stale:
      header u32 @20         = crc(bytes 0..20)
      table[0] leading u32   = crc(table index)
      table[i] leading u32   = crc(table[i-1] from +40 to the start of table[i])
      last 4 bytes of file   = crc(last table from +40 to len-4)"""
    n = struct.unpack('>I', data[16:20])[0]
    base = 24 + n * 8
    offs = [base + struct.unpack('>I', data[28 + i * 8: 32 + i * 8])[0] for i in range(n)]
    struct.pack_into('>I', data, 20, crc32_msb(data[0:20]))
    struct.pack_into('>I', data, offs[0], crc32_msb(data[24:base]))
    for i in range(1, n):
        struct.pack_into('>I', data, offs[i], crc32_msb(data[offs[i - 1] + 40: offs[i]]))
    struct.pack_into('>I', data, len(data) - 4, crc32_msb(data[offs[-1] + 40: len(data) - 4]))
    return data


def set_int(data, table, row, field, value):
    """Write an integer field of one record in place (bytearray); call fix_checksums afterwards."""
    f = next(f for f in table.fields if f[0] == field)
    name, ftype, bitoff, bits = f
    if not 0 <= value < (1 << bits):
        raise ValueError(f'{field} value {value} out of range 0..{(1 << bits) - 1}')
    base = (table.records_at + row * table.record_bytes) * 8 + bitoff
    for b in range(bits):
        pos = base + b
        if (value >> (bits - 1 - b)) & 1:
            data[pos >> 3] |= 0x80 >> (pos & 7)
        else:
            data[pos >> 3] &= ~(0x80 >> (pos & 7)) & 0xFF


def set_str(data, table, row, field, text):
    f = next(f for f in table.fields if f[0] == field)
    name, ftype, bitoff, bits = f
    raw = text.encode('latin1')
    if len(raw) > bits // 8:   # the game's own data fills some fields completely (no terminator)
        raise ValueError(f'{field} too long (max {bits // 8} characters)')
    at = table.records_at + row * table.record_bytes + bitoff // 8
    data[at: at + bits // 8] = raw.ljust(bits // 8, b'\0')


def append_rows(data, table_name, records):
    """Appends raw records (bytes of record_bytes each) to a table and returns the new database (bytearray).
    If the table is full, its capacity grows: space is inserted after its records, the following tables' offsets
    and the database size are updated. Call fix_checksums afterwards."""
    data = bytearray(data)
    n = struct.unpack('>I', data[16:20])[0]
    base = 24 + n * 8
    t = load(bytes(data))
    T = t[table_name]
    rb = T.record_bytes
    need = T.used + len(records)
    idx = list(t).index(table_name)
    th = base + struct.unpack('>I', data[28 + idx * 8: 32 + idx * 8])[0]   # table header
    if need > T.capacity:
        extra = (need - T.capacity) * rb
        end = T.records_at + T.capacity * rb
        data[end:end] = bytes(extra)
        for i in range(n):
            o = struct.unpack('>I', data[28 + i * 8: 32 + i * 8])[0]
            if base + o > th:
                struct.pack_into('>I', data, 28 + i * 8, o + extra)
        struct.pack_into('>I', data, 8, struct.unpack('>I', data[8:12])[0] + extra)
        struct.pack_into('>H', data, th + 20, need)
    for k, rec in enumerate(records):
        if len(rec) != rb:
            raise ValueError('record size mismatch')
        at = T.records_at + (T.used + k) * rb
        data[at: at + rb] = rec
    struct.pack_into('>H', data, th + 22, need)
    return data


def set_records(data, table_name, records):
    """Replaces a table's records with the given raw records (must fit its capacity) and sets its row count."""
    n = struct.unpack('>I', data[16:20])[0]
    base = 24 + n * 8
    t = load(bytes(data))
    T = t[table_name]
    if len(records) > T.capacity:
        raise ValueError(f'{table_name}: {len(records)} rows do not fit capacity {T.capacity}')
    idx = list(t).index(table_name)
    th = base + struct.unpack('>I', data[28 + idx * 8: 32 + idx * 8])[0]
    rb = T.record_bytes
    data[T.records_at: T.records_at + T.capacity * rb] = bytes(T.capacity * rb)
    for k, rec in enumerate(records):
        data[T.records_at + k * rb: T.records_at + (k + 1) * rb] = rec
    struct.pack_into('>H', data, th + 22, len(records))
    return data


def record_bytes(table, row):
    return bytes(table.data[table.records_at + row * table.record_bytes: table.records_at + (row + 1) * table.record_bytes])


def load(data):
    assert data[:2] == b'DB', 'not a TDB database'
    n = struct.unpack('>I', data[16:20])[0]
    base = 24 + n * 8
    tables = {}
    for i in range(n):
        nm = data[24 + i * 8: 28 + i * 8][::-1].decode('latin1')
        off = struct.unpack('>I', data[28 + i * 8: 32 + i * 8])[0]
        tables[nm] = Table(nm, data, base + off)
    return tables


if __name__ == '__main__':
    t = load(open(sys.argv[1], 'rb').read())
    for name, tab in t.items():
        print(f'{name}: {tab.used}/{tab.capacity} records, {tab.record_bytes} bytes, {len(tab.fields)} fields')
        if len(sys.argv) > 2 and name == sys.argv[2]:
            for f in tab.fields:
                print('   ', f)
