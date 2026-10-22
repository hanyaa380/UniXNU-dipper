#!/usr/bin/env python3
"""Build the fajita Data volume seed (/private/var) on Linux from data-seed.json, packed to DBRDISK1.

  make-data-seed-linux.py --packer PATH/packed_ramdisk.py [--apfsprogs DIR | --fetch-apfsprogs]
                          [--volume-uuid UUID] [--container-uuid UUID] [--timestamp UNIX_SECONDS]
                          [--spec data-seed.json] [--out DIR] [--keep-raw] [--apfs-dump DIR]

No root, no kernel module, no FUSE. mkapfs (apfsprogs) formats a raw image of size_mib MiB as one APFS container at
offset 0 (no partition map) holding one case-sensitive, unencrypted volume with no role (APFS_VOL_ROLE_NONE, which
mkapfs writes; iOS APFS panics on an unencrypted volume with the Data role, so spec role must be "none"). mkapfs
cannot create directories, so this script then edits the fresh container in place:

  * the catalog (the volume's file-system B-tree) is rebuilt with the root and private directories plus every
    directory of the spec, with the spec's owner, group and mode (root and private-dir are 0:0 0755); the leaves go
    in newly allocated blocks, mapped through the volume object map, marked in the space manager's bitmap and
    counted in the chunk, device and volume block counts;
  * the volume superblock gets the directory count and next inode number (its role field stays 0);
  * nx_next_oid in the container superblock and its checkpoint copy moves past the new virtual object ids;
  * every rewritten object gets its Fletcher-64 checksum recomputed.

All timestamps come from --timestamp (default SOURCE_DATE_EPOCH, else now), so the same UUIDs, timestamp and mkapfs
commit give a bit-identical image. The result is checked with apfsck, then read back by an independent walker in
this script (container -> omap -> volume -> omap -> catalog) that compares name, role, UUIDs, case sensitivity,
encryption and the directory tree with owners and modes against the spec; with --apfs-dump, apfs-fuse's apfsutil
and apfs-dump-quick (a second implementation) are checked too. The raw image is packed with packed_ramdisk.pack and
the packed file is unpacked with packed_ramdisk.regions and compared with the raw image byte for byte.

Writes data-linux.dbr and data-linux.json (volume UUID for CONFIG_XNU_OWNER_DATA_VOLUME_UUID) to --out.
Standard library only (runs under python3 -I); external tools: mkapfs and apfsck (git, make and a C compiler for
--fetch-apfsprogs)."""
import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import struct
import subprocess
import sys
import time
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APFSPROGS_URL = 'https://github.com/linux-apfs/apfsprogs.git'
APFSPROGS_COMMIT = '1bd1c1c2140d13d1b6d2e03379316572bee70aa3'   # the layout this script edits was checked against it

BLOCK = 4096
NODE_HEADER = 56            # obj_phys_t + btree_node_phys_t fields before btn_data
INFO_SIZE = 40              # btree_info_t footer of a root node
NX_MAGIC, APFS_MAGIC = b'NXSB', b'APSB'

# Object types (low 16 bits of o_type) and storage flags
OBJ_NX_SUPERBLOCK, OBJ_BTREE, OBJ_BTREE_NODE, OBJ_SPACEMAN, OBJ_CIB = 0x1, 0x2, 0x3, 0x5, 0x7
OBJ_OMAP, OBJ_CHECKPOINT_MAP, OBJ_FS, OBJ_FSTREE = 0xb, 0xc, 0xd, 0xe
OBJ_VIRTUAL, OBJ_EPHEMERAL, OBJ_PHYSICAL = 0, 0x80000000, 0x40000000
BTNODE_ROOT, BTNODE_LEAF, BTNODE_FIXED_KV = 1, 2, 4
BTREE_KV_NONALIGNED = 0x40
BTOFF_INVALID = 0xffff

# Volume fields
VOL_ROLE_NONE = 0
INCOMPAT_CASE_INSENSITIVE, INCOMPAT_NORMALIZATION_INSENSITIVE = 0x1, 0x8
FS_UNENCRYPTED = 0x1
ROOT_DIR_PARENT, ROOT_DIR_INO, PRIV_DIR_INO, MIN_USER_INO = 1, 2, 3, 16
TYPE_INODE, TYPE_DIR_REC = 3, 9
OBJ_ID_MASK = (1 << 60) - 1
INODE_NO_RSRC_FORK = 0x8000
INO_EXT_TYPE_NAME, XF_DO_NOT_COPY = 4, 2
S_IFDIR, DT_DIR = 0o040000, 4


# ---------------------------------------------------------------- primitives

def fletcher64(block):
    """APFS object checksum: Fletcher-64 over the 32-bit words after the 8-byte checksum field."""
    words = struct.unpack_from(f'<{(len(block) - 8) // 4}I', block, 8)
    count = len(words)
    modulus = 0xffffffff
    sum1 = sum(words) % modulus
    sum2 = sum(word * (count - index) for index, word in enumerate(words)) % modulus
    check1 = modulus - ((sum1 + sum2) % modulus)
    check2 = modulus - ((sum1 + check1) % modulus)
    return (check2 << 32) | check1


def seal(obj):
    struct.pack_into('<Q', obj, 0, fletcher64(obj))
    return obj


def checksum_ok(obj):
    return struct.unpack_from('<Q', obj)[0] == fletcher64(obj)


def _crc32c_table():
    table = []
    for byte in range(256):
        crc = byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
        table.append(crc)
    return table


CRC32C = _crc32c_table()


def drec_hash(name):
    """Hash of a directory-record key on a normalization-insensitive volume: CRC-32C (seed ~0, no final xor) over
    the UTF-32LE code points of the normalized name. ASCII names are already normalized; case is not folded on a
    case-sensitive volume."""
    crc = 0xffffffff
    for byte in name.encode('utf-32-le'):
        crc = CRC32C[(crc ^ byte) & 0xff] ^ (crc >> 8)
    return (crc & 0x3fffff) << 10


def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def u64(b, o): return struct.unpack_from('<Q', b, o)[0]


class Image:
    def __init__(self, path, writable=False):
        self.stream = open(path, 'r+b' if writable else 'rb')

    def read(self, bno, count=1):
        self.stream.seek(bno * BLOCK)
        data = self.stream.read(count * BLOCK)
        if len(data) != count * BLOCK:
            raise SystemExit(f'short read at block {bno}')
        return bytearray(data)

    def write(self, bno, data):
        assert len(data) % BLOCK == 0
        self.stream.seek(bno * BLOCK)
        self.stream.write(data)

    def close(self):
        self.stream.close()


def obj_header(obj):
    """(oid, xid, type, subtype) of an object."""
    return struct.unpack_from('<QQII', obj, 8)


def set_obj_header(obj, oid, xid, otype, subtype):
    struct.pack_into('<QQII', obj, 8, oid, xid, otype, subtype)


# ---------------------------------------------------------------- spec

def load_spec(path):
    spec = json.loads(Path(path).read_text())
    if spec.get('role', 'none') != 'none':
        raise SystemExit('only role "none" is supported: iOS APFS refuses an unencrypted volume with the Data role')
    if spec.get('case_sensitive') is not True:
        raise SystemExit('this builder makes case-sensitive volumes only (spec case_sensitive must be true)')
    name = spec['volume_name']
    if not name or len(name.encode()) >= 256:
        raise SystemExit('bad volume_name')
    entries, seen = [], set()
    for path_, uid, gid, mode in spec['directories']:
        parts = path_.split('/')
        if not path_ or path_.startswith('/') or any(p in ('', '.', '..') for p in parts):
            raise SystemExit(f'bad directory path {path_!r}')
        if any(not (0x20 <= ord(c) < 0x7f) for c in path_):
            # Non-ASCII names would need Apple's Unicode normalization for the record hash.
            raise SystemExit(f'only printable ASCII directory names are supported: {path_!r}')
        if any(len(p.encode()) > 255 for p in parts):
            raise SystemExit(f'name too long in {path_!r}')
        parent = '/'.join(parts[:-1])
        if parent and parent not in seen:
            raise SystemExit(f'{path_!r} is listed before its parent {parent!r}')
        if path_ in seen:
            raise SystemExit(f'{path_!r} is listed twice')
        mode_value = int(mode, 8) if isinstance(mode, str) else int(mode)
        if mode_value & ~0o7777:
            raise SystemExit(f'bad mode {mode!r} for {path_!r}')
        if not (0 <= int(uid) < 1 << 32 and 0 <= int(gid) < 1 << 32):
            raise SystemExit(f'bad owner for {path_!r}')
        seen.add(path_)
        entries.append((path_, int(uid), int(gid), mode_value))
    return spec, entries


# ---------------------------------------------------------------- tools

def find_tool(name, apfsprogs):
    if apfsprogs:
        for candidate in (apfsprogs / name / name, apfsprogs / name):
            if candidate.is_file() and os.access(candidate, os.X_OK):
                return candidate
        raise SystemExit(f'{name} not found in {apfsprogs}')
    found = shutil.which(name)
    if not found:
        raise SystemExit(f'{name} not found on PATH; pass --apfsprogs DIR (an apfsprogs build tree) or --fetch-apfsprogs')
    return Path(found)


def fetch_apfsprogs(where):
    """Clone apfsprogs at the pinned commit into where and build mkapfs and apfsck there (no install)."""
    if not (where / '.git').is_dir():
        where.parent.mkdir(parents=True, exist_ok=True)
        run('git', 'clone', '--quiet', APFSPROGS_URL, where)
    run('git', '-C', where, 'fetch', '--quiet', 'origin', APFSPROGS_COMMIT, check=False)
    run('git', '-C', where, 'checkout', '--quiet', '--detach', APFSPROGS_COMMIT)
    run('make', '-C', where / 'mkapfs', '--quiet')
    run('make', '-C', where / 'apfsck', '--quiet')
    return where


def run(*command, check=True, capture=False):
    print('+', ' '.join(map(str, command)), flush=True)
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True)
    if not capture and result.stdout:
        print(result.stdout, end='', flush=True)
    if result.returncode and check:
        print(result.stderr, end='', file=sys.stderr, flush=True)
        raise SystemExit(f'failed ({result.returncode}): {" ".join(map(str, command))}')
    return result


# ---------------------------------------------------------------- b-tree nodes

def fixed_leaf_records(node, area_end):
    """Records of a fixed-size (16-byte key, 16-byte value) leaf, e.g. an omap leaf."""
    nkeys = u32(node, 36)
    toc = NODE_HEADER + u16(node, 40)
    key_base = toc + u16(node, 42)
    records = []
    for index in range(nkeys):
        k, v = struct.unpack_from('<HH', node, toc + 4 * index)
        records.append((bytes(node[key_base + k:key_base + k + 16]), bytes(node[area_end - v:area_end - v + 16])))
    return records


def variable_records(node, area_end):
    nkeys = u32(node, 36)
    toc = NODE_HEADER + u16(node, 40)
    key_base = toc + u16(node, 42)
    records = []
    for index in range(nkeys):
        koff, klen, voff, vlen = struct.unpack_from('<HHHH', node, toc + 8 * index)
        records.append((bytes(node[key_base + koff:key_base + koff + klen]),
                        bytes(node[area_end - voff:area_end - voff + vlen])))
    return records


def toc_bytes(count):
    return -(-count // 8) * 8 * 8          # kvloc entries, grown in steps of eight like newfs_apfs


def variable_node(records, *, root, leaf, level, info=None):
    """Lay out a node with variable-size keys and values (catalog). Returns the 4096-byte body without header ids."""
    node = bytearray(BLOCK)
    area_end = BLOCK - (INFO_SIZE if root else 0)
    toc_len = toc_bytes(len(records))
    key_base = NODE_HEADER + toc_len
    key_at, val_used = 0, 0
    for index, (key, val) in enumerate(records):
        node[key_base + key_at:key_base + key_at + len(key)] = key
        val_used += len(val)
        node[area_end - val_used:area_end - val_used + len(val)] = val
        struct.pack_into('<HHHH', node, NODE_HEADER + 8 * index, key_at, len(key), val_used, len(val))
        key_at += len(key)
    free_len = area_end - key_base - key_at - val_used
    if free_len < 0:
        raise AssertionError('node overflow')
    flags = (BTNODE_ROOT if root else 0) | (BTNODE_LEAF if leaf else 0)
    struct.pack_into('<HHI', node, 32, flags, level, len(records))
    struct.pack_into('<HHHHHHHH', node, 40, 0, toc_len, key_at, free_len, BTOFF_INVALID, 0, BTOFF_INVALID, 0)
    if root:
        node[BLOCK - INFO_SIZE:] = info
    return node


def fits(records, root):
    area_end = BLOCK - (INFO_SIZE if root else 0)
    return NODE_HEADER + toc_bytes(len(records)) + sum(len(k) + len(v) for k, v in records) <= area_end


# ---------------------------------------------------------------- catalog records

def inode_key(ino):
    return struct.pack('<Q', (TYPE_INODE << 60) | ino)


def inode_val(ino, parent, name, nchildren, uid, gid, mode, stamp):
    raw_name = name.encode() + b'\0'
    padded = -(-len(raw_name) // 8) * 8
    val = struct.pack('<QQQQQQQiIIIIIHHQ', parent, ino, stamp, stamp, stamp, stamp, INODE_NO_RSRC_FORK, nchildren,
                      0, 0, 0, uid, gid, S_IFDIR | mode, 0, 0)
    val += struct.pack('<HH', 1, padded) + struct.pack('<BBH', INO_EXT_TYPE_NAME, XF_DO_NOT_COPY, len(raw_name))
    return val + raw_name.ljust(padded, b'\0')


def drec_key(parent, name):
    raw_name = name.encode() + b'\0'
    return struct.pack('<QI', (TYPE_DIR_REC << 60) | parent, drec_hash(name) | len(raw_name)) + raw_name


def drec_val(ino, stamp):
    return struct.pack('<QQH', ino, stamp, DT_DIR)


def catalog_sort_key(key):
    hdr = u64(key, 0)
    obj_id, rtype = hdr & OBJ_ID_MASK, hdr >> 60
    if rtype == TYPE_DIR_REC:
        return (obj_id, rtype, u32(key, 8) & ~0x3ff, key[12:-1])
    return (obj_id, rtype, 0, b'')


def catalog_records(entries, stamp):
    """All catalog records of the volume, sorted in B-tree order, and the inode numbers by path."""
    inos = {'': ROOT_DIR_INO}
    children = {'': 0}
    for index, (path, *_rest) in enumerate(entries):
        inos[path] = MIN_USER_INO + index
        children[path] = 0
        children[path.rpartition('/')[0]] += 1
    records = [
        (drec_key(ROOT_DIR_PARENT, 'root'), drec_val(ROOT_DIR_INO, stamp)),
        (drec_key(ROOT_DIR_PARENT, 'private-dir'), drec_val(PRIV_DIR_INO, stamp)),
        (inode_key(ROOT_DIR_INO), inode_val(ROOT_DIR_INO, ROOT_DIR_PARENT, 'root', children[''], 0, 0, 0o755, stamp)),
        (inode_key(PRIV_DIR_INO), inode_val(PRIV_DIR_INO, ROOT_DIR_PARENT, 'private-dir', 0, 0, 0, 0o755, stamp)),
    ]
    for path, uid, gid, mode in entries:
        parent, _, name = path.rpartition('/')
        ino = inos[path]
        records.append((drec_key(inos[parent], name), drec_val(ino, stamp)))
        records.append((inode_key(ino), inode_val(ino, inos[parent], name, children[path], uid, gid, mode, stamp)))
    records.sort(key=lambda record: catalog_sort_key(record[0]))
    return records, inos


def split_leaves(records):
    leaves, current = [], []
    for record in records:
        if current and not fits(current + [record], root=False):
            leaves.append(current)
            current = []
        current.append(record)
    leaves.append(current)
    return leaves


# ---------------------------------------------------------------- container editing

def locate(image):
    """Find the objects of the fresh mkapfs container this script edits; checks the layout is the one expected."""
    nx = image.read(0)
    if nx[32:36] != NX_MAGIC or not checksum_ok(nx):
        raise SystemExit('block 0 is not a valid APFS container superblock')
    if u32(nx, 36) != BLOCK:
        raise SystemExit('only 4096-byte APFS blocks are supported')
    xid = obj_header(nx)[1]
    desc_base, desc_blocks = u64(nx, 112), u32(nx, 104)
    if desc_base >> 63:
        raise SystemExit('non-contiguous checkpoint descriptor area')
    sb_copies, maps = [], []
    for bno in range(desc_base, desc_base + desc_blocks):
        block = image.read(bno)
        if not any(block[:8]):
            continue
        _, bxid, btype, _ = obj_header(block)
        if checksum_ok(block) and bxid == xid:
            if btype & 0xffff == OBJ_NX_SUPERBLOCK:
                sb_copies.append(bno)
            elif btype & 0xffff == OBJ_CHECKPOINT_MAP:
                maps.append(bno)
    if len(sb_copies) != 1 or len(maps) != 1:
        raise SystemExit('expected a single checkpoint (fresh mkapfs container)')
    cpm = image.read(maps[0])
    spaceman = None
    for index in range(u32(cpm, 36)):
        ctype, _, size, _, _, oid, paddr = struct.unpack_from('<IIIIQQQ', cpm, 40 + 40 * index)
        if ctype & 0xffff == OBJ_SPACEMAN:
            spaceman = (paddr, size, oid)
    if not spaceman:
        raise SystemExit('no space manager in the checkpoint map')
    fs_oids = [u64(nx, 184 + 8 * i) for i in range(u32(nx, 180))]
    if sum(1 for oid in fs_oids if oid) != 1 or not fs_oids[0]:
        raise SystemExit('expected exactly one volume')
    container_omap = image.read(u64(nx, 160))
    omap_root = image.read(u64(container_omap, 48))
    vsb_bno = omap_lookup(omap_root, fs_oids[0])
    vsb = image.read(vsb_bno)
    if vsb[32:36] != APFS_MAGIC or not checksum_ok(vsb):
        raise SystemExit('volume superblock not found')
    vol_omap_bno = u64(vsb, 128)
    vol_omap_root_bno = u64(image.read(vol_omap_bno), 48)
    vol_omap_root = image.read(vol_omap_root_bno)
    if u16(vol_omap_root, 32) != BTNODE_ROOT | BTNODE_LEAF | BTNODE_FIXED_KV:
        raise SystemExit('volume object map is not a single leaf')
    cat_oid = u64(vsb, 136)
    cat_bno = omap_lookup(vol_omap_root, cat_oid)
    cat = image.read(cat_bno)
    if u16(cat, 32) != BTNODE_ROOT | BTNODE_LEAF or u32(cat, 36) != 4:
        raise SystemExit('catalog is not the fresh four-record root leaf mkapfs makes')
    return dict(nx_sb_copies=[0, sb_copies[0]], spaceman=spaceman, vsb_bno=vsb_bno, vol_omap_root_bno=vol_omap_root_bno,
                cat_oid=cat_oid, cat_bno=cat_bno, xid=xid)


def omap_lookup(root, oid):
    if u16(root, 32) & BTNODE_LEAF == 0:
        raise SystemExit('multi-level object maps are not expected in a fresh container')
    for key, val in fixed_leaf_records(root, BLOCK - INFO_SIZE):
        if u64(key, 0) == oid:
            return u64(val, 8)
    raise SystemExit(f'oid {oid:#x} not in object map')


def allocate(image, spaceman_loc, count):
    """Take count free blocks from chunk 0 of the main device; updates bitmap, chunk info and device free counts."""
    sm_bno, sm_size, _ = spaceman_loc
    sm = image.read(sm_bno, sm_size // BLOCK)
    if not checksum_ok(sm):
        raise SystemExit('space manager checksum mismatch')
    dev = 48                                           # sm_dev[SD_MAIN]
    cib_count, cab_count = u32(sm, dev + 16), u32(sm, dev + 20)
    if cab_count:
        raise SystemExit('cib address blocks are not supported (container too large)')
    cib_bno = u64(sm, u32(sm, dev + 32))
    cib = image.read(cib_bno)
    ci = 40                                            # cib_chunk_info[0]
    ci_blocks, ci_free, bitmap_bno = u32(cib, ci + 16), u32(cib, ci + 20), u64(cib, ci + 24)
    if not bitmap_bno or ci_free < count:
        raise SystemExit('no room in the first chunk')
    bitmap = image.read(bitmap_bno)
    taken = []
    for bit in range(ci_blocks):
        if not bitmap[bit // 8] >> (bit % 8) & 1:
            bitmap[bit // 8] |= 1 << (bit % 8)
            taken.append(u64(cib, ci + 8) + bit)
            if len(taken) == count:
                break
    if len(taken) != count:
        raise SystemExit('chunk bitmap full')
    struct.pack_into('<I', cib, ci + 20, ci_free - count)
    struct.pack_into('<Q', sm, dev + 24, u64(sm, dev + 24) - count)
    image.write(bitmap_bno, bitmap)
    image.write(cib_bno, seal(cib))
    image.write(sm_bno, seal(sm))
    return taken


def build_volume(raw, entries, stamp):
    """Rebuild the catalog with the spec's directories and set the counters; returns a summary."""
    image = Image(raw, writable=True)
    loc = locate(image)
    xid = loc['xid']
    old_cat = image.read(loc['cat_bno'])
    old_keys = [key for key, _ in variable_records(old_cat, BLOCK - INFO_SIZE)]
    # The hash function must agree with mkapfs's own keys for 'root' and 'private-dir'.
    if sorted(old_keys) != sorted([drec_key(ROOT_DIR_PARENT, 'root'), drec_key(ROOT_DIR_PARENT, 'private-dir'),
                                   inode_key(ROOT_DIR_INO), inode_key(PRIV_DIR_INO)]):
        raise SystemExit('mkapfs catalog keys differ from the ones this script computes (hash mismatch?)')

    records, inos = catalog_records(entries, stamp)
    info = bytearray(INFO_SIZE)
    longest_key = max(len(k) for k, _ in records)
    longest_val = max(len(v) for _, v in records)

    nx = image.read(0)
    next_oid = u64(nx, 88)
    if fits(records, root=True):
        leaves = []
        root_records = records
        root_leaf = True
    else:
        leaves = split_leaves(records)
        root_leaf = False
    new_bnos = allocate(image, loc['spaceman'], len(leaves)) if leaves else []
    new_oids = [next_oid + i for i in range(len(leaves))]

    struct.pack_into('<IIIIIIQQ', info, 0, BTREE_KV_NONALIGNED, BLOCK, 0, 0, longest_key, longest_val,
                     len(records), 1 + len(leaves))
    for leaf, oid, bno in zip(leaves, new_oids, new_bnos):
        node = variable_node(leaf, root=False, leaf=True, level=0)
        set_obj_header(node, oid, xid, OBJ_VIRTUAL | OBJ_BTREE_NODE, OBJ_FSTREE)
        image.write(bno, seal(node))
    if not root_leaf:
        root_records = [(leaf[0][0], struct.pack('<Q', oid)) for leaf, oid in zip(leaves, new_oids)]
        if not fits(root_records, root=True):
            raise SystemExit('too many directories for a two-level catalog')
    root = variable_node(root_records, root=True, leaf=root_leaf, level=0 if root_leaf else 1, info=info)
    set_obj_header(root, loc['cat_oid'], xid, OBJ_VIRTUAL | OBJ_BTREE, OBJ_FSTREE)
    image.write(loc['cat_bno'], seal(root))

    # Volume object map: one more (oid, xid) -> paddr record per leaf.
    omap = image.read(loc['vol_omap_root_bno'])
    area_end = BLOCK - INFO_SIZE
    mappings = [(u64(k, 0), u64(k, 8), v) for k, v in fixed_leaf_records(omap, area_end)]
    mappings += [(oid, xid, struct.pack('<IIQ', 0, BLOCK, bno)) for oid, bno in zip(new_oids, new_bnos)]
    mappings.sort()
    toc_len = u16(omap, 42)
    if len(mappings) * 4 > toc_len:
        raise SystemExit('volume object map table full')
    body = bytearray(BLOCK)
    body[:32] = omap[:32]
    key_base = NODE_HEADER + toc_len
    for index, (oid, oxid, val) in enumerate(mappings):
        struct.pack_into('<HH', body, NODE_HEADER + 4 * index, 16 * index, 16 * (index + 1))
        struct.pack_into('<QQ', body, key_base + 16 * index, oid, oxid)
        body[area_end - 16 * (index + 1):area_end - 16 * index] = val
    n = len(mappings)
    struct.pack_into('<HHI', body, 32, BTNODE_ROOT | BTNODE_LEAF | BTNODE_FIXED_KV, 0, n)
    struct.pack_into('<HHHHHHHH', body, 40, 0, toc_len, 16 * n, area_end - key_base - 32 * n,
                     BTOFF_INVALID, 0, BTOFF_INVALID, 0)
    body[area_end:] = omap[area_end:]
    struct.pack_into('<Q', body, area_end + 24, n)
    image.write(loc['vol_omap_root_bno'], seal(body))

    # Volume superblock.
    vsb = image.read(loc['vsb_bno'])
    if u16(vsb, 964) != VOL_ROLE_NONE:
        raise SystemExit('mkapfs set a volume role; expected none')
    struct.pack_into('<Q', vsb, 88, u64(vsb, 88) + len(leaves))               # apfs_fs_alloc_count
    struct.pack_into('<Q', vsb, 176, MIN_USER_INO + len(entries))             # apfs_next_obj_id
    struct.pack_into('<Q', vsb, 192, len(entries))                            # apfs_num_directories
    struct.pack_into('<Q', vsb, 272 + 32, stamp)                              # apfs_formatted_by.timestamp
    image.write(loc['vsb_bno'], seal(vsb))

    # Container superblock and its checkpoint copy.
    for bno in loc['nx_sb_copies']:
        sb = image.read(bno)
        struct.pack_into('<Q', sb, 88, next_oid + len(leaves))
        image.write(bno, seal(sb))
    image.close()
    return dict(catalog_leaves=len(leaves), catalog_records=len(records), leaf_blocks=new_bnos, leaf_oids=new_oids)


# ---------------------------------------------------------------- independent read-back

def read_back(raw):
    """Walk the image from block 0 without the writer's bookkeeping and return what a mounter would see."""
    image = Image(raw)
    seen_checksums = 0

    def obj(bno, expect_type=None):
        nonlocal seen_checksums
        block = image.read(bno)
        if not checksum_ok(block):
            raise SystemExit(f'read-back: bad checksum at block {bno}')
        seen_checksums += 1
        if expect_type is not None and obj_header(block)[2] & 0xffff != expect_type:
            raise SystemExit(f'read-back: block {bno} has type {obj_header(block)[2]:#x}, wanted {expect_type:#x}')
        return block

    def walk(root_bno, resolve, fixed):
        """All leaf records of a B-tree; resolve maps a child pointer to a block number."""
        out = []

        def visit(bno, is_root):
            node = obj(bno)
            flags, level = u16(node, 32), u16(node, 34)
            if bool(flags & BTNODE_ROOT) != is_root:
                raise SystemExit('read-back: root flag wrong')
            area_end = BLOCK - (INFO_SIZE if is_root else 0)
            if flags & BTNODE_FIXED_KV:
                nkeys, toc = u32(node, 36), NODE_HEADER + u16(node, 40)
                key_base = toc + u16(node, 42)
                vsize = 16 if flags & BTNODE_LEAF else 8
                recs = []
                for i in range(nkeys):
                    k, v = struct.unpack_from('<HH', node, toc + 4 * i)
                    recs.append((bytes(node[key_base + k:key_base + k + 16]),
                                 bytes(node[area_end - v:area_end - v + vsize])))
            else:
                recs = variable_records(node, area_end)
            if flags & BTNODE_LEAF:
                if level:
                    raise SystemExit('read-back: leaf with nonzero level')
                out.extend(recs)
            else:
                for _, val in recs:
                    visit(resolve(u64(val, 0)), False)
            return node

        root = visit(root_bno, True)
        return root, out

    nx = obj(0, OBJ_NX_SUPERBLOCK)
    if nx[32:36] != NX_MAGIC:
        raise SystemExit('read-back: no NXSB')
    container_uuid = uuid.UUID(bytes=bytes(nx[72:88]))
    block_count = u64(nx, 40)
    c_omap = obj(u64(nx, 160), OBJ_OMAP)
    _, c_map = walk(u64(c_omap, 48), lambda paddr: paddr, True)
    c_map = {u64(k, 0): u64(v, 8) for k, v in c_map}
    volumes = [u64(nx, 184 + 8 * i) for i in range(u32(nx, 180)) if u64(nx, 184 + 8 * i)]
    if len(volumes) != 1:
        raise SystemExit(f'read-back: {len(volumes)} volumes')
    vsb = obj(c_map[volumes[0]], OBJ_FS)
    if vsb[32:36] != APFS_MAGIC:
        raise SystemExit('read-back: no APSB')
    v_omap = obj(u64(vsb, 128), OBJ_OMAP)
    _, v_map = walk(u64(v_omap, 48), lambda paddr: paddr, True)
    v_map = {u64(k, 0): u64(v, 8) for k, v in v_map}
    cat_root, cat = walk(v_map[u64(vsb, 136)], lambda oid: v_map[oid], False)

    incompat = u64(vsb, 56)
    inodes, drecs, other = {}, [], []
    for key, val in cat:
        hdr = u64(key, 0)
        obj_id, rtype = hdr & OBJ_ID_MASK, hdr >> 60
        if rtype == TYPE_INODE:
            parent, private, *_t, flags, nchildren, _pc, _wg, bsd, uid, gid, mode, _pad, _size = \
                struct.unpack_from('<QQQQQQQiIIIIIHHQ', val)
            nxf, used = struct.unpack_from('<HH', val, 92)
            name = None
            data_at = 96 + 4 * nxf
            for i in range(nxf):
                xtype, _xflags, xsize = struct.unpack_from('<BBH', val, 96 + 4 * i)
                if xtype == INO_EXT_TYPE_NAME:
                    name = val[data_at:data_at + xsize - 1].decode()
                data_at += -(-xsize // 8) * 8
            inodes[obj_id] = dict(parent=parent, nchildren=nchildren, uid=uid, gid=gid, mode=mode, name=name,
                                  flags=flags, bsd=bsd, xfields=nxf)
        elif rtype == TYPE_DIR_REC:
            length = u32(key, 8) & 0x3ff
            name = key[12:12 + length - 1].decode()
            if drec_hash(name) != u32(key, 8) & ~0x3ff:
                raise SystemExit(f'read-back: bad hash for {name!r}')
            ino, _added, dflags = struct.unpack_from('<QQH', val)
            drecs.append((obj_id, name, ino, dflags & 0xf))
        else:
            other.append(rtype)
    paths = {ROOT_DIR_INO: ''}
    pending = [d for d in drecs if d[0] != ROOT_DIR_PARENT]
    while pending:
        progress = [d for d in pending if d[0] in paths]
        if not progress:
            raise SystemExit('read-back: directory records with unreachable parents')
        for parent, name, ino, _ in progress:
            paths[ino] = f'{paths[parent]}/{name}'.lstrip('/')
        pending = [d for d in pending if d not in progress]
    tree = []
    for parent, name, ino, dtype in drecs:
        if parent == ROOT_DIR_PARENT:
            continue
        node = inodes[ino]
        tree.append(dict(path=paths[ino], ino=ino, uid=node['uid'], gid=node['gid'], mode=node['mode'],
                         dtype=dtype, inode_name=node['name'], parent=node['parent']))
    tree.sort(key=lambda item: item['path'])
    image.close()
    return dict(
        container_uuid=str(container_uuid).upper(), block_count=block_count,
        volume_uuid=str(uuid.UUID(bytes=bytes(vsb[240:256]))).upper(),
        volume_name=vsb[704:960].split(b'\0')[0].decode(), role=u16(vsb, 964),
        case_sensitive=not incompat & INCOMPAT_CASE_INSENSITIVE,
        normalization_insensitive=bool(incompat & INCOMPAT_NORMALIZATION_INSENSITIVE),
        unencrypted=bool(u64(vsb, 264) & FS_UNENCRYPTED), num_directories=u64(vsb, 192),
        num_files=u64(vsb, 184), other_record_types=sorted(set(other)), inodes=inodes, tree=tree,
        root_inode=inodes.get(ROOT_DIR_INO), private_inode=inodes.get(PRIV_DIR_INO),
        top_level=[d[1] for d in drecs if d[0] == ROOT_DIR_PARENT], checksummed_objects=seen_checksums)


def check_read_back(seen, spec, entries, volume_uuid, container_uuid):
    problems = []
    def want(what, got, expected):
        if got != expected:
            problems.append(f'{what}: got {got!r}, expected {expected!r}')
    want('volume name', seen['volume_name'], spec['volume_name'])
    want('role', seen['role'], VOL_ROLE_NONE)
    want('volume uuid', seen['volume_uuid'], volume_uuid)
    want('container uuid', seen['container_uuid'], container_uuid)
    want('case sensitive', seen['case_sensitive'], True)
    want('unencrypted', seen['unencrypted'], True)
    want('directory count', seen['num_directories'], len(entries))
    want('file count', seen['num_files'], 0)
    want('other catalog record types', seen['other_record_types'], [])
    want('special directories', sorted(seen['top_level']), ['private-dir', 'root'])
    want('logical blocks', seen['block_count'], spec['size_mib'] * 1024 * 1024 // BLOCK)
    root = seen['root_inode']
    want('root directory owner/mode', (root['uid'], root['gid'], root['mode']), (0, 0, S_IFDIR | 0o755))
    got = [(t['path'], t['uid'], t['gid'], t['mode']) for t in seen['tree']]
    expected = sorted((p, u, g, S_IFDIR | m) for p, u, g, m in entries)
    if got != expected:
        problems.append(f'directory tree: only on disk {sorted(set(got) - set(expected))}, '
                        f'only in spec {sorted(set(expected) - set(got))}')
    for t in seen['tree']:
        if t['dtype'] != DT_DIR or t['inode_name'] != t['path'].rpartition('/')[2]:
            problems.append(f'record mismatch for {t["path"]}')
    if len(seen['inodes']) != len(entries) + 2:
        problems.append(f'{len(seen["inodes"])} inodes, expected {len(entries) + 2}')
    return problems


def check_apfs_dump(tools, raw, spec, entries, volume_uuid, scratch):
    """Cross-check with apfs-fuse's apfsutil and apfs-dump-quick (a separate APFS implementation)."""
    problems = []
    util = run(tools / 'apfsutil', raw, capture=True).stdout
    print(util, end='')
    if volume_uuid not in util.upper():
        problems.append('apfsutil: volume UUID not shown')
    if f'Name:               {spec["volume_name"]} (Case-sensitive)' not in util:
        problems.append('apfsutil: name/case sensitivity')
    if 'Role:               No specific role' not in util:
        problems.append('apfsutil: volume has a role (expected none)')
    log = scratch / 'apfs-dump-quick.txt'
    log.unlink(missing_ok=True)
    run(tools / 'apfs-dump-quick', raw, log, capture=True)
    inodes, dirs = {}, {}
    # Numbers are hex; index-node entries ('Inode 1F => 465', child oid only) carry no '[' and are skipped.
    for line in log.read_text(errors='replace').splitlines():
        parts = line.split()
        if '=>' not in parts or '[' not in line.split('=>', 1)[1]:
            continue
        if line.startswith('Inode '):
            at = parts.index('=>')
            ino = int(parts[1], 16)
            # Inode ino => parent private [TS] flags [names] nchildren pc wg bsd [bsdnames] uid gid mode ...
            close = [i for i, p in enumerate(parts) if p.endswith(']')]
            after = close[2] + 1 if len(close) > 2 else None
            uid, gid, mode = int(parts[after]), int(parts[after + 1]), int(parts[after + 2], 8)
            inodes[ino] = (int(parts[at + 1], 16), uid, gid, mode)
        elif line.startswith('DirRec '):
            name = line.split("'")[1]
            ino = int(line.split('=>')[1].split()[0], 16)
            dirs[ino] = (int(parts[1], 16), name)
    paths = {ROOT_DIR_INO: ''}
    for _ in range(64):
        for ino, (parent, name) in dirs.items():
            if parent in paths and ino not in paths and parent != ROOT_DIR_PARENT:
                paths[ino] = f'{paths[parent]}/{name}'.lstrip('/')
    missing = [paths[ino] for ino in paths if ino not in inodes]
    if missing:
        problems.append(f'apfs-dump-quick: no inode for {missing}')
    got = sorted((paths[ino], *inodes[ino][1:]) for ino in paths if ino != ROOT_DIR_INO and ino in inodes)
    expected = sorted((p, u, g, S_IFDIR | m) for p, u, g, m in entries)
    if got != expected:
        problems.append('apfs-dump-quick: directory tree differs: '
                        f'{sorted(set(got) ^ set(expected))[:6]}')
    return problems, len(got)


# ---------------------------------------------------------------- packing

def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def compare_packed(packer, raw, packed):
    """Unpack with packed_ramdisk.regions and compare with the raw image byte for byte."""
    logical, extents = packer.regions(packed.read_bytes())
    if logical != raw.stat().st_size:
        raise SystemExit('packed logical size differs from raw size')
    zero = bytes(1 << 20)
    end = 0
    with open(raw, 'rb') as stream:
        for offset, data in [*extents, (logical, b'')]:
            while end < offset:
                count = min(len(zero), offset - end)
                if stream.read(count) != zero[:count]:
                    raise SystemExit(f'raw image has data in a packed hole near {end:#x}')
                end += count
            if stream.read(len(data)) != bytes(data):
                raise SystemExit(f'packed extent at {offset:#x} differs from raw image')
            end += len(data)
    return len(extents)


def mode_string(mode):
    out = 'd'
    for shift, special, letter in ((6, 0o4000, 's'), (3, 0o2000, 's'), (0, 0o1000, 't')):
        bits = (mode >> shift) & 7
        x = 'x' if bits & 1 else '-'
        if mode & special:
            x = letter if bits & 1 else letter.upper()
        out += ('r' if bits & 4 else '-') + ('w' if bits & 2 else '-') + x
    return out


# ---------------------------------------------------------------- main

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--packer', type=Path, required=True, help='drivers/DarwinBoot/Test/packed_ramdisk.py of UniXNU')
    parser.add_argument('--spec', type=Path, default=ROOT / 'data-seed.json')
    parser.add_argument('--out', type=Path, default=ROOT / '.cache/data-seed')
    parser.add_argument('--apfsprogs', type=Path, help='apfsprogs build tree (or directory) holding mkapfs and apfsck')
    parser.add_argument('--fetch-apfsprogs', nargs='?', type=Path, const=ROOT / '.cache/apfsprogs',
                        metavar='DIR', help=f'clone apfsprogs {APFSPROGS_COMMIT[:12]} and build it in DIR '
                        '(default .cache/apfsprogs)')
    parser.add_argument('--apfs-dump', type=Path, metavar='DIR',
                        help='apfs-fuse build directory with apfsutil and apfs-dump-quick, for a second read-back')
    parser.add_argument('--volume-uuid', help='fixed APFS volume UUID (default random)')
    parser.add_argument('--container-uuid', help='fixed APFS container UUID (default random)')
    parser.add_argument('--timestamp', type=int, help='Unix seconds for every timestamp (default SOURCE_DATE_EPOCH or now)')
    parser.add_argument('--keep-raw', action='store_true', help='keep data-linux.raw (also KEEP_RAW=1)')
    args = parser.parse_args()

    spec, entries = load_spec(args.spec)
    volume_uuid = str(uuid.UUID(args.volume_uuid) if args.volume_uuid else uuid.uuid4()).upper()
    container_uuid = str(uuid.UUID(args.container_uuid) if args.container_uuid else uuid.uuid4()).upper()
    if volume_uuid == container_uuid:
        raise SystemExit('the volume and container UUIDs must differ')
    if args.timestamp is not None:
        seconds = args.timestamp
    elif os.environ.get('SOURCE_DATE_EPOCH'):
        seconds = int(os.environ['SOURCE_DATE_EPOCH'])
    else:
        seconds = int(time.time())
    stamp = seconds * 1_000_000_000

    loader = importlib.util.spec_from_file_location('packed_ramdisk', args.packer)
    if not loader:
        raise SystemExit(f'cannot load {args.packer}')
    packer = importlib.util.module_from_spec(loader)
    loader.loader.exec_module(packer)

    apfsprogs = fetch_apfsprogs(args.fetch_apfsprogs.resolve()) if args.fetch_apfsprogs else args.apfsprogs
    mkapfs, apfsck = find_tool('mkapfs', apfsprogs), find_tool('apfsck', apfsprogs)
    mkapfs_version = run(mkapfs, '-v', capture=True, check=False).stdout.strip().splitlines()
    mkapfs_version = mkapfs_version[0] if mkapfs_version else 'unknown'

    args.out.mkdir(parents=True, exist_ok=True)
    raw, packed, report = args.out / 'data-linux.raw', args.out / 'data-linux.dbr', args.out / 'data-linux.json'
    for path in (raw, packed, report):
        path.unlink(missing_ok=True)
    with open(raw, 'wb') as stream:
        stream.truncate(spec['size_mib'] << 20)

    # Case-sensitive (-s), normalization-insensitive (the default; hashed directory keys like iOS), unencrypted.
    run(mkapfs, '-s', '-L', spec['volume_name'], '-U', container_uuid, '-u', volume_uuid, raw)
    summary = build_volume(raw, entries, stamp)
    print(f'catalog: {summary["catalog_records"]} records in {summary["catalog_leaves"]} leaves '
          f'(blocks {summary["leaf_blocks"]}) under the root index')

    check = run(apfsck, '-cuw', raw, check=False)
    if check.returncode or check.stdout.strip() or check.stderr.strip():
        print(check.stderr, end='', file=sys.stderr)
        raise SystemExit('apfsck reported problems')
    print('apfsck -cuw: clean')

    seen = read_back(raw)
    problems = check_read_back(seen, spec, entries, volume_uuid, container_uuid)
    dump_dirs = None
    if args.apfs_dump:
        more, dump_dirs = check_apfs_dump(args.apfs_dump, raw, spec, entries, volume_uuid, args.out)
        problems += more
        (args.out / 'apfs-dump-quick.txt').unlink(missing_ok=True)
    if problems:
        raise SystemExit('read-back mismatch:\n  ' + '\n  '.join(problems))
    print(f'read-back: {len(seen["tree"])} directories match the spec ({seen["checksummed_objects"]} objects checksummed)')

    raw_sha = sha256(raw)
    metadata = packer.pack(raw, packed)
    extents = compare_packed(packer, raw, packed)
    # Up to packed_ramdisk.PLAIN_HASH_LIMIT the packer's identity is the plain hash of the disk; above it, a hash of the size and the
    # nonzero pages (seed_identity), so only the small case compares against the raw file. compare_packed checks the bytes either way.
    if raw.stat().st_size <= packer.PLAIN_HASH_LIMIT and metadata['sha256'] != raw_sha:
        raise SystemExit('packed logical hash differs from raw hash')
    print(f'packed: {extents} extents, unpacked image identical to raw')

    tree = '\n'.join(f'{mode_string(t["mode"])} {t["uid"]:>4} {t["gid"]:>4} {t["path"]}' for t in seen['tree'])
    result = {
        'logical_bytes': raw.stat().st_size,
        'volume_name': seen['volume_name'],
        'volume_uuid': volume_uuid,
        'container_uuid': container_uuid,
        'role': [],
        'role_value': f'{seen["role"]:#x}',
        'filesystem': 'Case-sensitive APFS',
        'encrypted': not seen['unencrypted'],
        'layout': 'APFS container at offset 0, no partition map',
        'raw_sha256': raw_sha,
        'packed_sha256': sha256(packed),
        'packed_bytes': packed.stat().st_size,
        'packed_extents': extents,
        'timestamp': seconds,
        'root_directory': [0, 0, '0755'],
        'directories': spec['directories'],
        'tree': tree,
        'verified': ['apfsck -cuw', 'python read-back'] + (['apfs-fuse apfsutil + apfs-dump-quick'] if dump_dirs else [])
                    + ['packed_ramdisk.regions == raw'],
        'mkapfs': mkapfs_version,
        'builder': 'linux-mkapfs',
    }
    report.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: v for k, v in result.items() if k not in ('directories', 'tree')}, indent=2))
    if not args.keep_raw and os.environ.get('KEEP_RAW') != '1':
        raw.unlink()
    print(f'volume UUID: {volume_uuid}')


if __name__ == '__main__':
    main()
