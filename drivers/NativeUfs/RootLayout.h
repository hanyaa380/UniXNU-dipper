// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// Shared production parser. Requires hostRead4k, SHA256, diagnostics and C memory primitives.
// ---- LUN 0 GPT and the root-image manifest -------------------------------------------------
struct RootWindow {
  char name[32];
  uint64_t offset;   // bytes from the start of the userdata partition
  uint64_t size;     // bytes
  uint8_t sha256[32];
};

struct RootLayout {
  uint64_t userdataFirst, userdataLast;   // 4096-byte LBAs on LUN 0
  uint64_t imageBytes;                    // size of the flashed root image from the manifest (0 when none)
  uint64_t gptEntriesLba;                 // GPT facts recorded for the persistent store's before/after comparison
  uint32_t gptHeaderCrc, gptEntriesCrc, gptEntryCount;
  uint32_t windows;
  RootWindow window[4];
  uint32_t manifestStatus;                // 0 ok, 1 no manifest magic, 2 bad version/count, 3 hash, 4 bounds, 5 names, 6 overlap, 7 attributes
  uint32_t magic0, magic1;                // first 8 bytes of the partition's first block
  uint32_t ext4Magic;                     // 16 bits at byte 1080 of the partition (0xef53 on Linux ext4)
  uint32_t ext4Blocks;                    // s_blocks_count_lo at byte 1028
};

static uint8_t gScratch[kDataMax];

static uint32_t le32 (const uint8_t *at)
{
  return uint32_t (at[0]) | (uint32_t (at[1]) << 8) | (uint32_t (at[2]) << 16) | (uint32_t (at[3]) << 24);
}

static uint64_t le64 (const uint8_t *at)
{
  return uint64_t (le32 (at)) | (uint64_t (le32 (at + 4)) << 32);
}

// GPT uses the reflected IEEE CRC32 polynomial. The header CRC field is zero
// during its own checksum; array CRC covers exactly count * entrySize bytes.
static uint32_t rootCrc32 (const uint8_t *data, size_t bytes, bool header = false)
{
  uint32_t crc = 0xffffffffU;
  for (size_t i = 0; i < bytes; ++i) {
    crc ^= header && i >= 16 && i < 20 ? 0 : data[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
  }
  return ~crc;
}

static bool rootEntryUsed (const uint8_t *entry)
{
  uint8_t used = 0;
  for (unsigned i = 0; i < 16; ++i) used |= entry[i];
  return used != 0;
}

// Find the dedicated "unixnu" partition, or fall back to "userdata", in LUN 0's primary GPT
// (4096-byte sectors), then read the manifest at its first block. Read-only. Returns false only
// if the GPT itself is unusable.
static bool findRootLayout (RootLayout *layout, const char **why)
{
  ROOT_STAGE ("GPT header");
  bzero (layout, sizeof (*layout));
  if (hostRead4k (0, 1, 1, gScratch) != kIOReturnSuccess) {
    *why = "GPT header read";
    return false;
  }
  if (le32 (gScratch) != 0x20494645U || le32 (gScratch + 4) != 0x54524150U || le32 (gScratch + 84) != 128) {
    *why = "GPT header";
    return false;
  }
  uint32_t headerBytes = le32 (gScratch + 12);
  if (le32 (gScratch + 8) != 0x00010000 || headerBytes < 92 || headerBytes > 4096 ||
      le32 (gScratch + 20) || le64 (gScratch + 24) != 1 ||
      rootCrc32 (gScratch, headerBytes, true) != le32 (gScratch + 16)) {
    *why = "GPT header CRC/revision";
    return false;
  }
  uint64_t backup = le64 (gScratch + 32), first = le64 (gScratch + 40), last = le64 (gScratch + 48);
  uint32_t entriesCrc = le32 (gScratch + 88);
  uint64_t entriesLba = le64 (gScratch + 72);
  uint32_t count = le32 (gScratch + 80);
  layout->gptHeaderCrc = le32 (gScratch + 16);
  layout->gptEntriesCrc = entriesCrc;
  layout->gptEntriesLba = entriesLba;
  layout->gptEntryCount = count;
  if (entriesLba < 2 || entriesLba > 64 || !count || count > 128) {
    *why = "GPT geometry";
    return false;
  }
  uint32_t blocks = (count * 128 + 4095) / 4096;
  // This path uses READ(10), so reject geometry beyond its actual LBA range.
  if (first < entriesLba + blocks || last < first || backup <= last || backup > UINT32_MAX ||
      backup - last <= blocks) {
    *why = "GPT usable range";
    return false;
  }
  ROOT_STAGE ("GPT entries");
  if (hostRead4k (0, entriesLba, blocks, gScratch) != kIOReturnSuccess) {
    *why = "GPT entries read";
    return false;
  }
  if (rootCrc32 (gScratch, count * 128) != entriesCrc) {
    *why = "GPT entries CRC";
    return false;
  }
  auto named = [] (const uint8_t *entry, const char *wanted) {
    for (unsigned c = 0;; c++) {
      if (entry[56 + 2 * c] != static_cast<uint8_t> (wanted[c]) || entry[57 + 2 * c] != 0) return false;
      if (!wanted[c]) return true;
    }
  };
  bool unixnuFound = false, userdataFound = false;
  uint32_t unixnuIndex = 0, userdataIndex = 0;
  uint64_t unixnuFirst = 0, unixnuLast = 0, userdataFirst = 0, userdataLast = 0;
  for (uint32_t index = 0; index < count; index++) {
    const uint8_t *entry = gScratch + index * 128;
    if (!rootEntryUsed (entry)) continue;
    uint64_t entryFirst = le64 (entry + 32), entryLast = le64 (entry + 40);
    bool unixnu = named (entry, "unixnu");
    bool userdata = named (entry, "userdata");
    if (entryFirst < first || entryLast < entryFirst || entryLast > last) {
      // Only the selected root partition is ever read; another partition outside the usable range is reported, not fatal.
      ROOT_LOG ("GPT entry %u %llx..%llx outside usable %llx..%llx%s\n", index, entryFirst, entryLast, first, last,
                unixnu ? " (unixnu)" : userdata ? " (userdata)" : "");
      if (unixnu || userdata) {
        *why = "GPT partition bounds";
        return false;
      }
      continue;
    }
    if (unixnu) {
      if (unixnuFound) {
        *why = "duplicate unixnu partition";
        return false;
      }
      unixnuFirst = entryFirst;
      unixnuLast = entryLast;
      unixnuIndex = index;
      unixnuFound = true;
    } else if (userdata) {
      if (userdataFound) {
        *why = "duplicate userdata partition";
        return false;
      }
      userdataFirst = entryFirst;
      userdataLast = entryLast;
      userdataIndex = index;
      userdataFound = true;
    }
  }
  bool found = unixnuFound || userdataFound;
  const char *selected = unixnuFound ? "unixnu" : "userdata";
  if (unixnuFound) {
    layout->userdataFirst = unixnuFirst;
    layout->userdataLast = unixnuLast;
    userdataIndex = unixnuIndex;
  } else if (userdataFound) {
    layout->userdataFirst = userdataFirst;
    layout->userdataLast = userdataLast;
  }
  if (found) {
    // The window must not alias any other partition.
    for (uint32_t index = 0; index < count; index++) {
      const uint8_t *other = gScratch + index * 128;
      if (index == userdataIndex || !rootEntryUsed (other)) continue;
      if (layout->userdataFirst <= le64 (other + 40) && le64 (other + 32) <= layout->userdataLast) {
        ROOT_LOG ("GPT entry %u %llx..%llx overlaps %s\n", index, le64 (other + 32), le64 (other + 40), selected);
        *why = "GPT partition overlap";
        return false;
      }
    }
  }
  if (!found || layout->userdataLast < layout->userdataFirst) {
    *why = "no unixnu or userdata partition";
    return false;
  }
  gRootDiagnostic.first = layout->userdataFirst;
  gRootDiagnostic.last = layout->userdataLast;
  ROOT_LOG ("%s root LBA %llx..%llx\n", selected, layout->userdataFirst, layout->userdataLast);
  ROOT_STAGE ("root manifest");
  // First block of the partition: manifest if the root image was flashed, ext4 otherwise.
  if (hostRead4k (0, layout->userdataFirst, 1, gScratch) != kIOReturnSuccess) {
    *why = "userdata first block read";
    return false;
  }
  layout->magic0 = le32 (gScratch);
  layout->magic1 = le32 (gScratch + 4);
  ROOT_LOG ("root manifest magic %08x %08x\n", layout->magic0, layout->magic1);
  layout->ext4Magic = uint32_t (gScratch[1080]) | (uint32_t (gScratch[1081]) << 8);
  layout->ext4Blocks = le32 (gScratch + 1028);
  layout->manifestStatus = 1;
  if (layout->magic0 == 0x474e4853U && layout->magic1 == 0x544f4f52U) {        // "SHNGROOT"
    uint32_t version = le32 (gScratch + 8), n = le32 (gScratch + 12);
    uint64_t imageBytes = le64 (gScratch + 16);
    layout->manifestStatus = 2;
    if (version == 1 && n && n <= 4) {
      SHA256_CTX context;
      uint8_t digest[32];
      SHA256_Init (&context);
      SHA256_Update (&context, gScratch, 4064);
      SHA256_Final (digest, &context);
      layout->manifestStatus = 3;
      uint32_t chunk[8];
      for (unsigned c = 0; c < 8; ++c) chunk[c] = rootCrc32 (gScratch + 512 * c, 512);
      // Shown on the failure page (log lines scroll away under it): per-512-byte CRCs and the computed/stored digest prefixes.
      snprintf (gRootManifestText, sizeof (gRootManifestText),
                "MANIFEST crc %08x %08x %08x %08x %08x %08x %08x %08x\nsha calc %02x%02x%02x%02x%02x%02x%02x%02x stored "
                "%02x%02x%02x%02x%02x%02x%02x%02x\n", chunk[0], chunk[1], chunk[2], chunk[3], chunk[4], chunk[5], chunk[6],
                chunk[7], digest[0], digest[1], digest[2], digest[3], digest[4], digest[5], digest[6], digest[7], gScratch[4064],
                gScratch[4065], gScratch[4066], gScratch[4067], gScratch[4068], gScratch[4069], gScratch[4070], gScratch[4071]);
      // Bytes 0..279 matched the image file on hardware; list the nonzero bytes after the entries (expected none), then read the
      // block again: unchanged bytes are on the disk, changed ones are stale cache lines.
      for (unsigned pass = 0; pass < 2; ++pass) {
        size_t used = strlen (gRootManifestText);
        used += snprintf (gRootManifestText + used, sizeof (gRootManifestText) - used, "%s crc0 %08x nz:", pass ? "reread" : "read",
                          rootCrc32 (gScratch, 512));
        for (unsigned at = 280, shown = 0; at < 4064 && shown < 24 && used + 12 < sizeof (gRootManifestText); ++at)
          if (gScratch[at]) {
            used += snprintf (gRootManifestText + used, sizeof (gRootManifestText) - used, " %x=%02x", at, gScratch[at]);
            ++shown;
          }
        strlcat (gRootManifestText, "\n", sizeof (gRootManifestText));
        if (pass || hostRead4k (0, layout->userdataFirst, 1, gScratch) != kIOReturnSuccess) break;
      }
      if (memcmp (digest, gScratch + 4064, 32) != 0) {
        // Hardware 2026-10-22 (fajita): something stamps a protective MBR (0xEE entry at 0x1BE, 55AA at 0x1FE) on the
        // partition's first sector. The manifest never uses 0x1BE..0x1FF, so accept it only if the hash holds with that
        // region zeroed; any other change is still rejected.
        static uint8_t copy[4064];  // off the kernel stack
        memcpy (copy, gScratch, sizeof (copy));
        bzero (copy + 0x1be, 0x200 - 0x1be);
        SHA256_Init (&context);
        SHA256_Update (&context, copy, sizeof (copy));
        SHA256_Final (digest, &context);
        if (memcmp (digest, gScratch + 4064, 32) == 0) ROOT_LOG ("root manifest: ignoring a protective MBR stamped on it\n");
      }
      if (memcmp (digest, gScratch + 4064, 32) == 0) {
        uint64_t partitionBytes = (layout->userdataLast - layout->userdataFirst + 1) * 4096ULL;
        layout->manifestStatus = 4;
        bool bounds = imageBytes >= 4096 && !(imageBytes & 4095) && imageBytes <= partitionBytes;
        for (uint32_t index = 0; index < n && bounds; index++) {
          const uint8_t *entry = gScratch + 24 + 128 * index;
          uint64_t offset = le64 (entry + 32), size = le64 (entry + 40);
          if ((offset & 4095) || offset < 4096 || (size & 511) || !size || offset >= imageBytes ||
              size > imageBytes - offset || ((size + 4095) & ~4095ULL) > imageBytes - offset) {
            bounds = false;
            break;
          }
          if (le32 (entry + 48) != 512 || le32 (entry + 52) != 1) {
            layout->manifestStatus = 7; bounds = false; break;
          }
          unsigned length = 0;
          while (length < 32 && entry[length] >= 0x20 && entry[length] <= 0x7e) ++length;
          if (!length || length == 32 || entry[length] != 0) {
            layout->manifestStatus = 5; bounds = false; break;
          }
          for (uint32_t prior = 0; prior < index; ++prior) {
            const RootWindow &other = layout->window[prior];
            if (!strcmp (reinterpret_cast<const char *> (entry), other.name)) {
              layout->manifestStatus = 5; bounds = false; break;
            }
            // Include the final sector padding that a physical 4K read fetches.
            if (offset < other.offset + ((other.size + 4095) & ~4095ULL) &&
                other.offset < offset + ((size + 4095) & ~4095ULL)) {
              layout->manifestStatus = 6; bounds = false; break;
            }
          }
          if (!bounds) break;
          memcpy (layout->window[index].name, entry, 32);
          layout->window[index].name[31] = 0;
          layout->window[index].offset = offset;
          layout->window[index].size = size;
          memcpy (layout->window[index].sha256, entry + 56, 32);
        }
        if (bounds) {
          layout->windows = n;
          layout->imageBytes = imageBytes;
          layout->manifestStatus = 0;
        }
      }
    }
  }
  gRootDiagnostic.magic0 = layout->magic0;
  gRootDiagnostic.magic1 = layout->magic1;
  gRootDiagnostic.manifest = layout->manifestStatus;
  gRootDiagnostic.windows = layout->windows;
  return true;
}
