# QcomXnuUsbDebug

Read `README.md`, `docs/boundaries.md`, `docs/porting-other-socs.md`, then `docs/integration.md` before adapting a platform. Use `docs/sources-and-evidence.md` to trace source files and `docs/usage-and-validation.md` to select checks.

## Platform changes

- Record target resources and their source locations before adding `soc/<Name>.h`: MMIO windows, register layout, clocks/power/PHY owners, SID, DMA limits, boot handoff and reset conduit. Leave unknown values unresolved.
- Keep shared DWC3/CDC in `usb/`, without QUD, Qualcomm or XNU dependencies. Supply USB identity and MMIO/DMA callbacks through its public API. Keep Qualcomm qualification/DMA and SoC resource profiles in `soc/`. Reuse the current platform algorithms only when register semantics and inherited-state requirements match. Different PHY or SMMU architectures need platform code and tests.
- The backend qualifies inherited power/PHY. Claim cold-start support only after implementing and verifying those dependencies. A relocated model does not establish support for another SoC.
- Use only the private translated DMA path in the native adapter. Do not add an identity/BYPASS fallback or global SMMU writes. Preserve a lease and static memory until reset when ownership or DMA stop cannot be proved.
- Public hooks use `qcom_usb_debug_*` and `XNU_QCOM_USB_DEBUG`. Keep OS patches versioned and pinned under `patches/darwin22/`; add a separate profile when adapting another XNU revision.
- Screenshots use a display-registered CPU-coherent provider. Keep capture outside USB guards and panic, and leave mapping, synchronization and lifetime with the display driver.
- Reboot uses normal XNU shutdown on a separate thread. Preserve existing PE reset ownership; set `QUD_PSCI_SMC_RESET=0` in a new profile until the board's SMC reset is qualified.

## Tests and evidence

- Put test sources, runners and checked-in fixtures in `tests/`. Keep build, patch-management and runtime tools in `scripts/`.
- Use `tests/check_all.py` for complete logging builds; individual entry points are `tests/check.py`, `tests/check_logging.py`, `scripts/build.py` and `tests/check_xnu.py` as described in `docs/integration.md`. New SoC profiles need an explicit test-matrix entry and matching fixtures.
- Generated objects, executables and reports belong in workspace `tmp/` or `.cache/`, outside this repository. Preserve original USB captures and their command/recovery metadata under the workspace rules.
- Report model checks, kernel compilation/linking and hardware acceptance separately. Keep resource sources, revisions, hashes, reports and unverified capabilities with the consumer's adaptation record.
- Retain file license notices and origin hashes when moving source. Keep original identities in `docs/sources-and-evidence.md`; Git tracks current revisions.

This is an independent driver repository. Consumers reference it as `drivers/QcomXnuUsbDebug`; remote publication and device catalog/archive integration are separate changes.

## Logging extensions

Read `docs/logging.md` when adapting logging. Preserve the CPU-only msgbuf/early path and atomic framed records. Userland uses a root-only BSD sysctl, never a GPU endpoint. Keep Darwin22 memory and 20H392 system-store ABI checks explicit; other SoCs do not imply other userspace versions. Compile the final consumer start.s and inspect its resolved timestamp store before hardware acceptance.
