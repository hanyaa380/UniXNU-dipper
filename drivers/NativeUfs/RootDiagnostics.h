// SPDX-License-Identifier: BSD-2-Clause-Patent
// Passive snapshot formatter; caller supplies integer types and snprintf.
// No hardware access or decision about trust, transport or publication here.
struct UfsRootDiagnostics {
  const char *stage = "not started";
  uint64_t dma = 0, list = 0, lba = 0, first = 0, last = 0;
  uint64_t windowBytes = 0, policyCount = 0, reads = 0, failures = 0;
  uint32_t cap = 0, version = 0, hcs = 0, hce = 0, doorbell = 0, interrupt = 0;
  uint32_t smr = 0, s2cr = 0, cbar = 0, sctlr = 0;
  uint32_t type = 0, opcode = 0, lun = 0, command = 0, ocs = 0xff, scsi = 0xffff;
  uint32_t response = 0, result = 0, power = 0, manifest = 0xffffffff;
  uint32_t magic0 = 0, magic1 = 0, windows = 0, prepared = 0, published = 0;
  uint32_t verify = 0xffffffff;
  uint32_t protocol = 0, residual = 0, sense = 0;
  uint64_t far = 0, contiguous = 0;
  uint32_t runStop = 0, ahit = 0, polls = 0, dbSeen = 0, isSeen = 0, isBefore = 0;
  uint32_t gfsrBefore = 0, gfsr = 0, gsyn = 0, fsrBefore = 0, fsr = 0, fsyn = 0;
  uint32_t smmuId2 = 0, cba2r = 0, uic[5] = {};
  uint32_t smmuId1 = 0, smmuSlot = 0xffffffff;
  uint32_t publicationPhase = 0, publicationWindow = 0xffffffff;
};

static inline int formatUfsRootDiagnostics(char *buffer, size_t capacity,
                                          const UfsRootDiagnostics &d, const char *reason,
                                          const char *title = "UFS ROOT FAILURE")
{
  return snprintf(buffer, capacity,
    "%s: %s\nstage=%s result=%08x\n"
    "HC cap=%08x ver=%08x hcs=%08x hce=%08x db=%08x is=%08x\n"
    "DMA=%llx UTRL=%llx SMMU smr=%08x s2cr=%08x cbar=%08x sctlr=%08x\n"
    "CMD n=%u type=%02x op=%02x lun=%02x ocs=%02x rsp=%08x scsi=%04x power=%02x\n"
    "PROTOCOL reject=%u residual=%u sense=%06x SMMU slot=%u id1=%x\n"
    "QUEUE rsr=%x ahit=%x polls=%u dbSeen=%x isSeen=%x isBefore=%x contiguous=%llx\n"
    "FAULT global=%x->%x syn=%x cb=%x->%x syn=%x far=%llx id2=%x cba2r=%x\n"
    "UIC pa=%08x dl=%08x net=%08x tr=%08x dme=%08x\n"
    "GPT/read lba=%llx userdata=%llx..%llx magic=%08x %08x manifest=%u\n"
    "WINDOWS found=%u prepared=%u published=%u bytes=%llx policies=%llu verify=%08x\n"
    "IO reads=%llu failures=%llu pubPhase=%u window=%u\n",
    title, reason, d.stage, d.result, d.cap, d.version, d.hcs, d.hce, d.doorbell, d.interrupt,
    (unsigned long long)d.dma, (unsigned long long)d.list, d.smr, d.s2cr, d.cbar, d.sctlr,
    d.command, d.type, d.opcode, d.lun, d.ocs, d.response, d.scsi, d.power,
    d.protocol, d.residual, d.sense, d.smmuSlot, d.smmuId1,
    d.runStop, d.ahit, d.polls, d.dbSeen, d.isSeen, d.isBefore, (unsigned long long)d.contiguous,
    d.gfsrBefore, d.gfsr, d.gsyn, d.fsrBefore, d.fsr, d.fsyn, (unsigned long long)d.far,
    d.smmuId2, d.cba2r, d.uic[0], d.uic[1], d.uic[2], d.uic[3], d.uic[4],
    (unsigned long long)d.lba, (unsigned long long)d.first, (unsigned long long)d.last,
    d.magic0, d.magic1, d.manifest, d.windows, d.prepared, d.published,
    (unsigned long long)d.windowBytes, (unsigned long long)d.policyCount, d.verify,
    (unsigned long long)d.reads, (unsigned long long)d.failures, d.publicationPhase, d.publicationWindow);
}
