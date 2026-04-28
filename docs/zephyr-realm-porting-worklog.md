# Zephyr Realm Porting Worklog

This log records the concrete bring-up steps, observations, and code changes for the Zephyr-on-Realm port.

## 2026-04-07

### Payload entry stub added to separate shim handoff from Zephyr boot

Reason:

- the standalone shim now reliably reaches `phase=18(shim_payload_jump)`
- but no payload-side `phase=32(payload_main)` is visible yet
- we need a post-shim, pre-Zephyr checkpoint without modifying the Zephyr kernel

Action taken:

- added `src/vecu_zephyr/src/payload_entry.S`
- added a payload-owned entry symbol: `realm_payload_entry`
- changed the payload link entry in `src/vecu_zephyr/CMakeLists.txt` to:
  - `-Wl,-e,realm_payload_entry`
- updated `kvmtool` phase naming in `dev_workspace/kvmtool-cca/builtin-run.c`

New contract:

- shim still publishes:
  - `0x10`: `shim_entry`
  - `0x11`: `shim_rsi_ready`
  - `0x12`: `shim_payload_jump`
- payload-owned stub now publishes:
  - `0x19`: `payload_stub_entry`
  - `0x1a`: `payload_zephyr_jump`
- then the payload stub branches to Zephyr `__start`

Interpretation goal:

- if `0x19/0x1a` appear, control crossed from the standalone shim into the
  payload image successfully
- if `0x19/0x1a` appear but `0x20/0x21` do not, the remaining failure is
  inside Zephyr boot, not at the shim handoff boundary

## 2026-04-05

### Baseline findings

- Confirmed that Normal World AGL guest boots correctly.
- Confirmed that Realm Linux boots correctly on the same FVP + TF-RMM + `kvmtool-cca` stack.
- Confirmed that Zephyr Realm still does not produce a reliable visible boot side effect on the guest console.

### Important conclusions

- The current stack can run Realm guests.
- The remaining problem is Zephyr-specific and should be treated as a Zephyr-on-Realm porting task.
- Linux CCA guest code under `src/linux/arch/arm64/` is the active reference baseline.

### Earlier Zephyr Realm alignment work already in tree

- Added Linux-like early RSI detection and Realm config handling in Zephyr arch code.
- Added Realm-aware shared alias bit handling for device mappings in Zephyr ARM64 MMU code.
- Deferred aggressive early full-RAM RIPAS transitions after they proved too disruptive for bring-up.
- Added Linux Realm baseline scripts and README usage notes.
- Added safe per-VM shutdown script usage to avoid killing unrelated guests.

### New work in this step

Goal:

- stop relying on UART as the only bring-up signal
- add a fixed shared status page path for Phase 1 bring-up

Code changes:

- `src/vecu_zephyr/src/rsi.h`
  - added fixed shared status page definitions
  - added a simple `realm_status_page_t` structure and phase IDs
- `src/vecu_zephyr/src/rsi.c`
  - added `rsi_status_page_map()`
  - added `rsi_status_page_update()`
  - added `rsi_status_page_heartbeat()`
- `src/vecu_zephyr/src/main.c`
  - maps the fixed shared status page after `RSI_REALM_CONFIG`
  - writes status markers for:
    - main entered
    - RSI ready
    - attestation success
    - attestation failure
    - init failure
    - heartbeat
- `dev_workspace/kvmtool-cca/arm/aarch64/realm.c`
  - changed the extra page after the DTB window so it is only initialized as an IPA range
  - the page is no longer populated as private Realm RAM
  - intent: keep it usable as a shared debug page
- `dev_workspace/kvmtool-cca/builtin-run.c`
  - added an optional Realm status page watcher thread
  - watcher is enabled only when `LKVM_REALM_STATUS_WATCH=1`
  - watcher reads the fixed shared page GPA and logs phase transitions
- `scripts/run-vecu-zephyr.sh`
  - in `--debug`, now exports:
    - `LKVM_REALM_STATUS_WATCH=1`
    - `LKVM_REALM_STATUS_GPA=0x83e10000`

### Expected next check

After rebuilding and re-uploading:

- `realm-vecu1.log` should show `realm status ...` updates if Zephyr reaches the status page setup code
- if the status page remains unchanged, Zephyr still does not reach the first app-visible Realm-aware step

### Runtime note: stale VM blocker

Observed during the first status-page validation attempt:

- re-running `run-vecu-zephyr.sh` can fail with:
  - `A Realm VM named realm-vecu1 is already running.`
- removing only `/root/.lkvm/realm-vecu1.sock` is not sufficient
- the old `lkvm` process for `realm-vecu1` must be terminated first

Required FVP-side cleanup before each rerun:

```bash
/root/stop-lkvm-vm.sh realm-vecu1
ps -ef | grep realm-vecu1 | grep lkvm | grep -v grep
rm -f /root/.lkvm/realm-vecu1.sock /root/realm-vecu1.log /root/realm-vecu1.dtb
```

Interpretation:

- this blocker does not yet say anything about Zephyr Realm progress
- it only means runtime validation must start from a clean `realm-vecu1` process state

### Runtime result: status watcher alive, no phase transition yet

Observed after a clean rerun:

- guest serial/debug stream shows:
  - `realm status watch gpa=0x83e10000 host=...`
  - `realm guest realm-vecu1 vcpu0 live pc=0x80000000 pstate=0x3c5`
- no `realm status phase=...` transition was emitted

Interpretation:

- host-side watcher is active and reading the intended shared status GPA
- Zephyr still does not reach the app-level status publishing path in `main.c`
- current evidence suggests the guest is not yet reaching `main()` or any later app-visible Realm initialization

Follow-up action taken:

- moved the next diagnostic checkpoint earlier than `main()`
- added new shared status phases for:
  - `mmu_ready`
  - `pre_cstart`
- connected those checkpoints to `z_prep_c()` so that Zephyr can publish progress immediately after MMU init and immediately before `z_cstart()`

### New code added for earlier status publication

Files updated:

- `dev_workspace/zephyr/zephyr/arch/arm64/core/prep_c.c`
  - added a weak `z_arm64_realm_boot_status_hook()`
  - calls the hook:
    - immediately after `z_arm64_mm_init(true)` with phase `mmu_ready`
    - immediately before `z_cstart()` with phase `pre_cstart`
- `src/vecu_zephyr/src/rsi.h`
  - added new status phase IDs:
    - `REALM_STATUS_PHASE_MMU_READY`
    - `REALM_STATUS_PHASE_PRE_CSTART`
  - exported `z_arm64_realm_boot_status_hook(...)`
- `src/vecu_zephyr/src/rsi.c`
  - implemented `z_arm64_realm_boot_status_hook(...)`
  - hook lazily maps the shared status page and publishes the early phase text
- `dev_workspace/kvmtool-cca/builtin-run.c`
  - watcher now prints readable names for:
    - `mmu_ready`
    - `pre_cstart`

### Verification update

Rebuilt successfully after the early-hook patch:

```bash
cd /home/csos/NDSS/CCA-attestation
CCACHE_DISABLE=1 ./scripts/build-vecu-zephyr.sh

source /home/csos/NDSS/CCA-attestation/scripts/config.sh
make -C /home/csos/NDSS/CCA-attestation/dev_workspace/kvmtool-cca \
    ARCH=arm64 \
    CROSS_COMPILE=$THIRD_PARTY_DIR/$CROSS_COMPILE_DIR_LINUX/bin/aarch64-none-linux-gnu- \
    LIBFDT_DIR=/home/csos/NDSS/CCA-attestation/dev_workspace/libfdt-src/libfdt \
    lkvm-static -j4
```

Result:

- Zephyr rebuild passed
- `lkvm-static` rebuild passed

### Next runtime expectation

After uploading the rebuilt `zephyr.bin` and `lkvm-static`, the first useful signal is now one of:

- `realm status phase=7(mmu_ready) ...`
- `realm status phase=8(pre_cstart) ...`

Interpretation:

- if either appears, Zephyr reaches `z_prep_c()` and the problem is later than pure entry
- if neither appears and the watcher still only reports the watch GPA, the failure is earlier than `z_prep_c()` or the shared-page publication path is still unreachable

### Follow-up refinement: ultra-early EL1 status probe

Reason:

- the first early-hook attempt still produced only:
  - `realm status watch gpa=0x83e10000 host=...`
  - no phase transition
- that means either:
  - Zephyr is not reaching `z_prep_c()`, or
  - the `z_prep_c()`-time status publishing path is still too late or too heavyweight

New action taken:

- added an ultra-early status probe directly in `z_arm64_el1_init()`
- after `RSI_REALM_CONFIG` succeeds, Zephyr now:
  - marks the fixed status page as shared with `RSI_IPA_STATE_SET(..., RIPAS_EMPTY)`
  - writes directly to the shared-alias address without going through `z_phys_map()` or `main()`

Files updated:

- `dev_workspace/zephyr/zephyr/arch/arm64/core/reset.c`
  - added a minimal early status page structure
  - added `EARLY_REALM_STATUS_PHASE_EL1_RSI_READY`
  - added `realm_status_share_page_early()`
  - added `realm_status_write_early()`
  - `realm_rsi_init_early()` now publishes `el1 rsi ready` immediately after Realm config is read
- `dev_workspace/kvmtool-cca/builtin-run.c`
  - watcher now names phase `9` as `el1_rsi_ready`

Expected meaning of the next runtime:

- if `realm status phase=9(el1_rsi_ready)` appears:
  - Zephyr reaches `z_arm64_el1_init()` and executes RSI/config logic
  - the remaining problem is later than the first Realm-aware EL1 init step
- if phase `9` does not appear either:
  - the failure is even earlier than the first useful EL1 RSI step, or the direct shared-alias write is still not observable

### Adapter-layer restructure started

Reason:

- raw RSI helpers and app logic were getting mixed together
- the port now clearly needs a translation layer between:
  - Arm CCA Realm rules
  - Zephyr boot and app expectations

Design decision:

- keep raw RSI ABI in `rsi.[ch]`
- add a new `Realm Adapter Layer`
- move app-facing Realm logic behind the adapter
- keep early boot visibility hooks connected to the adapter path

Files added:

- `docs/zephyr-realm-adapter-design.md`
- `src/vecu_zephyr/src/realm_adapter.h`
- `src/vecu_zephyr/src/realm_adapter.c`

Files updated:

- `src/vecu_zephyr/src/main.c`
  - now uses adapter APIs instead of directly orchestrating most raw RSI work
- `src/vecu_zephyr/src/rsi.h`
  - remains the raw ABI header
- `src/vecu_zephyr/src/rsi.c`
  - retains low-level status page and RSI helpers
  - removed the boot hook implementation that moved into the adapter
- `src/vecu_zephyr/CMakeLists.txt`
  - now builds `realm_adapter.c`
- `docs/zephyr-realm-porting-todo.md`
  - now references the adapter-layer design
- `README.md`
  - now links the adapter-layer design document

Adapter responsibilities introduced in code:

- `realm_adapter_init()`
- `realm_adapter_state()`
- `realm_adapter_publish_status()`
- `realm_adapter_heartbeat()`
- `realm_adapter_prepare_comm_ctrl()`
- `realm_adapter_attest_once()`
- `realm_adapter_publish_shared_buffers()`
- `z_arm64_realm_boot_status_hook()`

Interpretation:

- this does not solve the boot issue by itself

## 2026-04-07

### Architecture pivot: standalone shim becomes the active implementation path

Reason:

- direct Zephyr Realm entry still stalled before any reliable payload-visible side effect
- Realm Linux is a known-good baseline on the same stack
- the preferred design principle is now:
  - keep Zephyr kernel code unmodified
  - move Realm-specific boot translation into a standalone shim

### Zephyr kernel restore

To align the implementation with that principle, the experimental Zephyr core
patches were restored back to upstream state inside the local Zephyr workspace:

- `dev_workspace/zephyr/zephyr/arch/arm64/core/reset.c`
- `dev_workspace/zephyr/zephyr/arch/arm64/core/prep_c.c`
- `dev_workspace/zephyr/zephyr/arch/arm64/core/mmu.c`
- `dev_workspace/zephyr/zephyr/arch/arm64/core/boot.h`

Meaning:

- the active shim-first path no longer depends on modified Zephyr kernel boot code
- any remaining payload cooperation should happen at the app or board level

### Zephyr payload reset to a basic payload app

The Zephyr payload app was simplified so it is no longer the Realm translation
layer.

Files changed:

- `src/vecu_zephyr/src/main.c`
  - replaced the RSI-heavy experimental app with a simple payload:
    - prints `main entered`
    - emits a heartbeat in a loop
- `src/vecu_zephyr/CMakeLists.txt`
  - removed the experimental adapter and raw RSI sources from the active build
- `src/vecu_zephyr/prj.conf`
  - restored to the tracked baseline configuration
- `src/vecu_zephyr/src/rsi.c`
- `src/vecu_zephyr/src/rsi.h`
- `src/vecu_zephyr/src/rsi_asm.S`
  - restored to the tracked baseline state
- removed experimental app-layer files:
  - `src/vecu_zephyr/src/realm_adapter.c`
  - `src/vecu_zephyr/src/realm_adapter.h`

Note:

- the old `realm_adapter.*` and `rsi.*` files remain in the tree as reference or
  experimental work, but they are no longer part of the active shim-first
  payload build

### Fixed payload contract implementation

The standalone shim contract was tightened into a real fixed bundle layout.

Files changed:

- `src/realm_shim/contract.h`
  - added:
    - `REALM_SHIM_LOAD_BASE`
    - `REALM_SHIM_PAYLOAD_OFFSET`
    - `REALM_SHIM_PAYLOAD_LOAD_BASE`
  - moved shim boot phases into a separate range:
    - `0x10`: shim entry
    - `0x11`: shim RSI ready
    - `0x12`: payload jump
- `dev_workspace/kvmtool-cca/builtin-run.c`
  - watcher now recognizes the shim phase names

Current fixed layout:

- `0x80000000`: shim entry
- `0x80200000`: Zephyr payload

### New Zephyr payload board

To keep the payload above the shim region without changing Zephyr kernel code,
the payload now uses a dedicated board definition.

Files added:

- `src/vecu_zephyr/boards/arm64/lkvm_realm_payload/Kconfig.board`
- `src/vecu_zephyr/boards/arm64/lkvm_realm_payload/Kconfig.defconfig`
- `src/vecu_zephyr/boards/arm64/lkvm_realm_payload/lkvm_realm_payload.dts`
- `src/vecu_zephyr/boards/arm64/lkvm_realm_payload/lkvm_realm_payload_defconfig`

Key payload memory contract:

- Zephyr SRAM base: `0x80200000`
- Zephyr SRAM size: `0x03e00000`
- first `2 MiB` reserved for the shim

Cleanup note:

- the older direct-entry board file `src/vecu_zephyr/boards/arm64/lkvm_realm/lkvm_realm.dts`
  was restored to its tracked baseline so the active path is clearly the new
  shim payload board

### Build pipeline updates

Files changed:

- `scripts/build-vecu-zephyr.sh`
  - now builds the payload board `lkvm_realm_payload` by default
  - now disables `ccache` during the build because the local environment hit a
    reproducible `/run/user/.../ccache-tmp` write failure
- `scripts/build-realm-zephyr-shim-bundle.sh`
  - new script
  - builds:
    - standalone shim
    - Zephyr payload
  - bundles them into:
    - `src/realm_shim/build/realm_zephyr_shim.bin`
- `scripts/run-realm-shim-zephyr.sh`
  - new runtime wrapper for the bundled Realm image

### Immediate next validation

The next runtime check should use the bundled image:

- upload `src/realm_shim/build/realm_zephyr_shim.bin`
- run it as the Realm guest
- confirm the first visible shim phases:
  - `shim_entry`
  - `shim_rsi_ready`
  - `shim_payload_jump`

If those appear, the shim handoff path is proven. The next question then becomes
whether the basic Zephyr payload emits any visible sign after the handoff.

### Runtime friction: stale `realm-vecu1` process after shim-first reruns

Observed during the first shim-bundle validation:

- rerunning the Realm guest sometimes still reported:
  - `A Realm VM named realm-vecu1 is already running.`
- that happened even after removing the socket manually
- `tail -f /root/realm-vecu1.log` also failed when the run aborted before the
  debug pipeline had created the log file

Action taken:

- `scripts/stop-lkvm-vm.sh`
  - now finds PIDs more robustly with `awk`
  - retries the kill a few times
  - reports remaining PIDs if the VM still survives
- `scripts/run-vecu-zephyr.sh`
  - now attempts one automatic stop via `/root/stop-lkvm-vm.sh realm-vecu1`
    before giving up
  - pre-creates the debug log file in `--debug` mode so `tail -f` has a stable
    target

Expected result:

- shim-first reruns should now be less brittle
- when a rerun still fails, the remaining PID list should be visible immediately

### Next experiment: app-level status write without Zephyr kernel changes

Goal:

- verify whether the payload app can publish a visible post-handoff signal
  without any Zephyr kernel MMU or RSI changes

Implementation:

- `src/vecu_zephyr/src/main.c`
  - added a minimal fixed-address status page write
  - the app now writes:
    - phase `0x20`: `payload main entered`
    - phase `0x21`: `payload heartbeat`
  - the write intentionally uses the fixed page base `0x83e10000` directly as
    an experiment, without introducing new Zephyr kernel mappings
- `dev_workspace/kvmtool-cca/builtin-run.c`
  - watcher now names:
    - `0x20` as `payload_main`
    - `0x21` as `payload_heartbeat`

Interpretation target:

- if phase `0x20` appears:
  - the shim not only jumped, but the Zephyr app reached `main()`
- if only `0x12` appears:
  - the shim handoff is real, but the payload still does not produce an
    observable app-level side effect
- it creates the right porting boundary so future Realm-specific work is not scattered across the app

### Build cleanup after adapter split

First rebuild after the adapter split exposed two cleanup issues:

- `realm_adapter.c`
  - missing `BIT()` macro include
- `rsi.c`
  - still needs the early arch globals for `rsi_status_page_map()`
  - `z_arm64_realm_rsi_present`
  - `z_arm64_realm_prot_ns_shared`

Fix applied:

- added `#include <zephyr/sys/util.h>` to `realm_adapter.c`
- restored the needed `extern` declarations in `rsi.c`

Interpretation:

- this was a refactor integration issue, not a new functional regression
- the adapter split still remains the active direction

Final verification after the cleanup:

```bash
cd /home/csos/NDSS/CCA-attestation
CCACHE_DISABLE=1 ./scripts/build-vecu-zephyr.sh
```

Result:

- adapter-layer Zephyr build passed
- output:
  - `src/vecu_zephyr/build/zephyr/zephyr.bin`
  - `src/vecu_zephyr/build/zephyr/zephyr.elf`

Current state after this step:

- the repository now has an explicit Realm Adapter Layer design
- the Zephyr app now depends on adapter APIs rather than directly orchestrating most Realm logic
- the next runtime comparison should be done using the rebuilt `zephyr.bin` with the existing debug watcher

### Architecture pivot: standalone shim as target principle

New design principle agreed:

- Zephyr kernel code should remain unmodified in the target architecture
- Realm-specific boot translation should move into a standalone shim

Implication:

- the current in-Zephyr adapter layer remains useful as an experimental path
- but it is no longer the preferred final architecture

Actions taken:

- added `docs/realm-shim-design.md`
- added standalone shim scaffold under `src/realm_shim/`
- added fixed shim contract header:
  - `src/realm_shim/contract.h`
- added standalone build script:
  - `scripts/build-realm-shim.sh`

Shim scaffold purpose:

- provide a Linux `Image`-compatible Realm entry binary
- read Realm config
- publish shared status phases
- jump to a fixed future payload contract address

Interpretation:

- the shim scaffold is the first step toward a reproducible, reusable boot translation layer
- this path is better suited for future payload reuse than deeper Zephyr kernel modifications

### Verification state

- Code changes prepared
- Build verification complete
- Runtime verification pending

### Build verification

Zephyr rebuild:

```bash
cd /home/csos/NDSS/CCA-attestation
CCACHE_DISABLE=1 ./scripts/build-vecu-zephyr.sh
```

Result:

- build passed
- output:
  - `src/vecu_zephyr/build/zephyr/zephyr.bin`
  - `src/vecu_zephyr/build/zephyr/zephyr.elf`

Sanity check:

- confirmed the rebuilt image contains:
  - `[realm] main entered`
  - `[realm] RSI ready`
  - `[realm] attestation ok`
  - `[realm] attestation failed`

`kvmtool-cca` rebuild:

```bash
make -C /home/csos/NDSS/CCA-attestation/dev_workspace/libfdt-src libfdt -j4
source /home/csos/NDSS/CCA-attestation/scripts/config.sh
make -C /home/csos/NDSS/CCA-attestation/dev_workspace/kvmtool-cca \
    ARCH=arm64 \
    CROSS_COMPILE=$THIRD_PARTY_DIR/$CROSS_COMPILE_DIR_LINUX/bin/aarch64-none-linux-gnu- \
    LIBFDT_DIR=/home/csos/NDSS/CCA-attestation/dev_workspace/libfdt-src/libfdt \
    lkvm-static -j4
```

Result:

- build passed
- output:
  - `dev_workspace/kvmtool-cca/lkvm-static`

### Next runtime test

Upload the rebuilt artifacts to FVP Linux:

```bash
scp src/vecu_zephyr/build/zephyr/zephyr.bin root@192.168.122.33:/root/realm-zephyr.bin
scp dev_workspace/kvmtool-cca/lkvm-static root@192.168.122.33:/root/lkvm-static.new
scp scripts/run-vecu-zephyr.sh root@192.168.122.33:/root/run-vecu-zephyr.sh
scp scripts/stop-lkvm-vm.sh root@192.168.122.33:/root/stop-lkvm-vm.sh
```

Inside FVP Linux:

```bash
chmod +x /root/run-vecu-zephyr.sh /root/stop-lkvm-vm.sh
/root/stop-lkvm-vm.sh realm-vecu1
mv /root/lkvm-static.new /root/lkvm-static
rm -f /root/.lkvm/realm-vecu1.sock /root/realm-vecu1.log /root/realm-vecu1.dtb
/root/run-vecu-zephyr.sh --debug /root/realm-zephyr.bin
tail -f /root/realm-vecu1.log
```

Expected new signal:

- `realm status phase=...` lines from the `kvmtool` watcher
- if those appear, Zephyr reached the shared status page path even if UART is still silent

### Shim runtime result: payload jump reached, payload progress still missing

Observed runtime output from the standalone shim bundle:

- `realm status phase=18 ... value0=33 value1=4294967296`

Interpretation:

- decimal `18` corresponds to shim phase `0x12`, i.e. `shim_payload_jump`
- `value0=33` confirms `ipa_bits=33`
- `value1=4294967296` confirms the expected shared alias bit (`1 << 32`)

This means:

- the standalone shim successfully entered
- `RSI_REALM_CONFIG` succeeded
- the shim successfully prepared the shared status page
- the shim reached the payload jump point

At this stage there was still no observable payload-side phase (`payload_main` or
`payload_heartbeat`), so the next debugging focus moved from the shim itself to
the payload handoff contract.

### Follow-up fix: correct bundle `image_size`

The first bundle runtime still logged:

- `Loaded kernel to 0x80000000 - 0x80002000 (2318340 bytes actual)`

Even though the file size was more than 2 MiB, the Linux Image header inside the
bundle still advertised the old shim-only `image_size` (`8192` bytes).

Actions taken:

- updated `scripts/build-realm-zephyr-shim-bundle.sh`
- after appending the Zephyr payload, the script now rewrites the Linux Image
  header `image_size` field at offset `0x10`

Verification:

```bash
python3 - <<'PY'
from pathlib import Path
import struct
p = Path('/home/csos/NDSS/CCA-attestation/src/realm_shim/build/realm_zephyr_shim.bin')
data = p.read_bytes()[:64]
print('bundle size:', p.stat().st_size)
print('image_size:', struct.unpack_from('<Q', data, 0x10)[0])
PY
```

Observed:

- `bundle size: 2318340`
- `image_size: 2318340`

So the bundle header now matches the full shim+payload image.

### Follow-up fix: explicit shim handoff page for the payload app

The payload app had originally attempted to write status directly to the status
GPA base (`0x83e10000`). That is not sufficient in Realm, because the shim had
already transitioned that page for shared access and payload-side publication
needs the shared alias address, not only the GPA.

To keep the Zephyr kernel unmodified while still giving the payload the Realm
translation details it needs, the shim now writes a private handoff structure
into a fixed page:

- handoff page: `0x801ff000`

Contract fields written by the shim:

- `status_gpa`
- `shared_alias_bit`
- `status_shared_addr`
- `dtb`
- `payload_load_base`
- `payload_entry`

Files updated:

- `src/realm_shim/contract.h`
- `src/realm_shim/start.S`
- `src/vecu_zephyr/src/main.c`

The payload app now reads the handoff page and publishes status using
`status_shared_addr`, rather than directly assuming the private GPA is usable.

### Rebuild verification after handoff-page fix

Bundle rebuild:

```bash
source /home/csos/NDSS/CCA-attestation/scripts/config.sh
/home/csos/NDSS/CCA-attestation/scripts/build-realm-zephyr-shim-bundle.sh
```

Result:

- build passed
- output:
  - `src/realm_shim/build/realm_zephyr_shim.bin`

`kvmtool-cca` rebuild:

```bash
make -C /home/csos/NDSS/CCA-attestation/dev_workspace/libfdt-src libfdt -j4
make -C /home/csos/NDSS/CCA-attestation/dev_workspace/kvmtool-cca clean
source /home/csos/NDSS/CCA-attestation/scripts/config.sh
make -C /home/csos/NDSS/CCA-attestation/dev_workspace/kvmtool-cca \
    ARCH=arm64 \
    CROSS_COMPILE=$THIRD_PARTY_DIR/$CROSS_COMPILE_DIR_LINUX/bin/aarch64-none-linux-gnu- \
    LIBFDT_DIR=/home/csos/NDSS/CCA-attestation/dev_workspace/libfdt-src/libfdt \
    lkvm-static -j4
```

Result:

- rebuild passed after cleaning stale mixed-architecture objects
- watcher strings now include:
  - `shim_payload_jump`
  - `payload_main`
  - `payload_heartbeat`

### Next runtime expectation

After uploading the rebuilt bundle and rebuilt `lkvm-static`, the expected
runtime sequence is:

- `phase=16(shim_entry)`
- `phase=17(shim_rsi_ready)`
- `phase=18(shim_payload_jump)`
- then ideally:
  - `phase=32(payload_main)`
  - `phase=33(payload_heartbeat)`

If `phase=18` still appears without `32/33`, the remaining issue is no longer
the shim translation itself, but the payload app execution path after handoff.

### Follow-up fix: payload jump now uses the actual Zephyr ELF entry

After the bundle and handoff-page fixes, the next likely issue was that the shim
still branched to the payload load base (`0x80200000`) rather than the payload's
real ELF entry point.

Observed from the rebuilt payload:

```bash
readelf -h /home/csos/NDSS/CCA-attestation/src/vecu_zephyr/build/zephyr/zephyr.elf \
    | rg 'Entry point'
```

Result:

- `Entry point address: 0x80201074`

Actions taken:

- updated `src/realm_shim/contract.h`
  - `REALM_SHIM_PAYLOAD_ENTRY` is now overridable
- updated `scripts/build-realm-shim.sh`
  - accepts `REALM_SHIM_PAYLOAD_ENTRY` as a compile-time override
- updated `scripts/build-realm-zephyr-shim-bundle.sh`
  - builds the payload first
  - extracts the payload ELF entry with `readelf`
  - rebuilds the shim with that exact payload entry

Verification:

Bundle build output now shows:

- `payload entry : 0x80201074`
- `payload entry override : 0x80201074`

Shim disassembly confirms the branch target was updated:

- the final `br x26` path now loads `0x80201074`, not `0x80200000`

Implication:

- the shim no longer depends on the payload Linux Image header branch stub
- the handoff now targets the actual Zephyr entry point directly

### Follow-up fix: payload-owned entry stub is now the actual handoff target

The first payload-entry-stub attempt compiled but was removed from the final
ELF by section garbage collection. To keep the Zephyr kernel untouched while
still observing post-shim execution:

- added `src/vecu_zephyr/realm_payload_entry.ld`
  - `KEEP(*(.text.realm_payload_entry))`
- updated `src/vecu_zephyr/CMakeLists.txt`
  - linker snippet registered with `zephyr_linker_sources(SECTIONS ...)`
- updated `scripts/build-realm-zephyr-shim-bundle.sh`
  - now prefers the `realm_payload_entry` symbol address from `nm`
  - falls back to the ELF entry only if the symbol is absent

Verification:

```bash
nm -an /home/csos/NDSS/CCA-attestation/src/vecu_zephyr/build/zephyr/zephyr.elf \
    | rg 'realm_payload_entry$|__start$'
```

Result:

- `realm_payload_entry = 0x80200ff0`
- `__start            = 0x802010e4`

Stub disassembly now shows:

- writes `0x19` (`payload_stub_entry`) to the shared status page
- writes `0x1a` (`payload_zephyr_jump`) immediately before branching
- then `b __start`

New expectation for the next runtime:

- `phase=18(shim_payload_jump)`
- then:
  - `phase=25(payload_stub_entry)`
  - `phase=26(payload_before_reset_prep)`
  - `phase=27(payload_after_reset_prep)`
  - `phase=28(payload_after_highest_init)`
  - `phase=29(payload_after_el1_init)`
  - `phase=30(payload_before_z_prep_c)`
- then ideally:
  - `phase=32(payload_main)`
  - `phase=33(payload_heartbeat)`

### Follow-up refinement: payload stub now wraps Zephyr early boot stages

The first payload stub only proved that control crossed from the shim into the
payload image. To narrow the remaining failure without touching the Zephyr
kernel, the payload-owned entry stub now executes the early Zephyr boot flow in
smaller observable steps:

- publish `0x19` on entry
- call `__reset_prep_c`
- publish `0x1b`
- call `z_arm64_el_highest_init`
- publish `0x1c`
- verify `CurrentEL == EL1`
- call `z_arm64_el1_init`
- publish `0x1d`
- publish `0x1e` immediately before branching to `z_prep_c`

If `CurrentEL` is not EL1, the stub publishes:

- `0x1f`: `payload_unexpected_el`

This keeps the Zephyr kernel unmodified while making the remaining boot failure
location much more precise.

### Runtime result: failure narrowed to inside `z_prep_c`

Observed on FVP after the staged payload-stub update:

- `phase=30(payload_before_z_prep_c)`
- no later payload or app phase was observed

Interpretation:

- shim handoff is good
- payload stub is good
- `__reset_prep_c`, `z_arm64_el_highest_init`, and `z_arm64_el1_init` all ran
- the remaining failure is inside `z_prep_c` or immediately after entering it

Follow-up action:

To keep the Zephyr kernel untouched while narrowing this further, the payload
stub now inlines the same sequence that `z_prep_c` would perform:

- set `tpidrro_el0 = &_kernel`
- call `z_bss_zero`
- call `z_arm64_mm_init(1)`
- call `z_arm64_interrupt_init`
- branch to `z_cstart`

New payload-only debug phases:

- `0x22`: `payload_after_bss_zero`
- `0x23`: `payload_after_mmu_init`
- `0x24`: `payload_after_interrupt_init`
- `0x25`: `payload_before_cstart`

This keeps the kernel code unchanged and should show exactly which internal
`z_prep_c` step is failing.
- Phase narrowing update: `phase=34(payload_after_bss_zero)` confirmed that the
  standalone shim plus payload stub now reaches and returns from `z_bss_zero()`.
  The next bottleneck is therefore inside `z_arm64_mm_init(1)`.
- Suspected root cause: the upstream `qemu_cortex_a53` SoC MMU region table maps
  UART/GIC with the private IPA values (`0x01000000`, `0x3ffd0000`,
  `0x3fff0000`). Under Realm, those device accesses should use the shared alias
  bit.
- Response: switched to an out-of-tree `SOC_ROOT` override inside
  `src/vecu_zephyr/soc/arm64/qemu_cortex_a53/` so Zephyr kernel sources remain
  untouched while MMU device regions come from the payload DTS.
- Payload DTS was updated so UART and GIC now use the shared alias addresses:
  `0x00000001_01000000`, `0x00000001_3ffd0000`, and `0x00000001_3fff0000`.
- Verification: the rebuilt payload now compiles
  `src/vecu_zephyr/soc/arm64/qemu_cortex_a53/mmu_regions.c.obj` instead of the
  upstream Zephyr kernel `soc/arm64/qemu_cortex_a53/mmu_regions.c.obj`, which
  confirms the out-of-tree SoC override is active.
- Next diagnostic step: insert a standalone TLBI probe immediately after
  `z_bss_zero()` and before `z_arm64_mm_init()`. This separates
  "Realm rejects/halts on TLBI system instruction" from
  "MMU table construction itself is faulty".

### Runtime result: TLBI probe succeeded, failure moved deeper into MMU setup

Observed on FVP after the TLBI probe update:

- `phase=39`

Interpretation:

- `39` is decimal `0x27`, which corresponds to `payload_after_tlbi_probe`
- the explicit `dsb; tlbi vmalle1; dsb; isb` sequence completed successfully
- the remaining failure is therefore *after* the standalone TLBI probe and
  still before `payload_after_mmu_init`

This rules out "Realm EL1 halts on `tlbi vmalle1` itself" as the primary
failure and shifts the focus to the internal page-table construction and sync
logic of `z_arm64_mm_init(1)`.

### Adapter-side response: move MMU table construction into payload-owned code

To keep the Zephyr kernel unmodified while gaining finer control over the
remaining failure, the payload now owns the page-table setup sequence and only
delegates the final MMU enable to Zephyr:

- new payload helper: `src/vecu_zephyr/src/payload_mmu.c`
- payload stub now calls `realm_payload_mmu_probe(status_shared_addr)`
- `realm_payload_mmu_probe()` performs:
  - `new_table()`
  - Zephyr execution-region mappings
  - device-region mappings from `mmu_config`
  - TLB/cache sync
- after that, the payload stub calls `z_arm64_mm_init(false)` so Zephyr only
  performs the EL1 MMU enable path

New payload-only debug phases for this split:

- `0x28`: `payload_after_new_table`
- `0x29`: `payload_after_zephyr_ranges`
- `0x2a`: `payload_after_device_ranges`
- `0x2b`: `payload_after_mmu_sync`

This keeps the kernel sources untouched and turns the shim/payload layer into
the active "Realm translation" layer, which is also more aligned with the
standalone adapter architecture.

### Runtime result: `new_table()` succeeds

Observed on FVP after the payload-owned MMU setup split:

- `phase=40`
- `text="new_table"`

Interpretation:

- `40` is decimal `0x28`, which corresponds to `payload_after_new_table`
- the payload-owned MMU adapter successfully allocates the base translation
  table and stores it into `kernel_ptables.base_xlat_table`
- the remaining failure is therefore in the mapping loops that follow:
  - Zephyr execution ranges
  - device ranges
  - or the final sync phase

### Follow-up refinement: publish every mapping attempt

To determine exactly which mapping step stalls, the payload adapter now emits a
status update immediately before each mapping call:

- `0x2c`: `payload_map_zephyr_range`
  - `value0 = range index`
  - `value1 = range start`
- `0x2d`: `payload_map_device_range`
  - `value0 = region index`
  - `value1 = region base_va`

This should reveal whether the first failure is on the image RAM range, the
text/rodata ranges, or one of the shared-alias device regions.

### Runtime result: first stall is on the original image RAM range

Observed on FVP after per-mapping phase publication:

- `phase=44`
- `value0=0`
- `value1=0x80200000`
- `text="map_zephyr_range"`

Interpretation:

- `44` is decimal `0x2c`, which corresponds to `payload_map_zephyr_range`
- `value0=0` means the first Zephyr execution range
- `value1=0x80200000` is `_image_ram_start`

So the first stall occurs before the adapter finishes mapping the broad
`image_ram` range that originally covered the entire payload image.

### Follow-up refinement: remove overlapping Zephyr execution ranges

The original Zephyr range order intentionally overlays mappings:

- whole image RAM as RW
- text as RX
- rodata as RO

That is correct for the upstream MMU setup, but it also forces the very first
mapping to cover the same region that will later be refined by overlapping
entries. To simplify the adapter path, the payload-owned MMU probe now uses
non-overlapping ranges:

- text: `__text_region_start .. __text_region_end`
- rodata: `__rodata_region_start .. __rodata_region_end`
- data/bss/noinit: `__rodata_region_end .. _image_ram_end`

Additionally, the final data range is still chunked page-by-page so we can
continue to localize failures at page granularity if the stall moves there.

### Runtime result: first 4 KiB Zephyr chunk still stalls

Observed on FVP after chunking the Zephyr mapping path:

- `phase=46`
- `value0=0`
- `value1=0x80200000`
- `text="map_zephyr_chunk"`

Interpretation:

- `46` is decimal `0x2e`, which corresponds to `payload_map_zephyr_chunk`
- the payload adapter is no longer stalling on the broad range-level call
- instead, it now stalls while attempting the very first 4 KiB mapping chunk
- the first chunk address is `0x80200000`

This confirms that the failure is tightly coupled to the first payload image
mapping itself rather than to the later device-region loop or to the final MMU
sync stage.

### Follow-up refinement: avoid mapping the currently executing text path first

To separate "general Zephyr text mapping fails" from "remapping the currently
executing page fails", the payload-owned Zephyr ranges were reordered and
split more deliberately:

- rodata is mapped first as a flat RO range
- data/bss/noinit is mapped second and chunked page-by-page
- text is mapped last and chunked page-by-page
- the current executing text page containing `realm_payload_mmu_probe()` is
  skipped during the first text chunk loop and mapped last

The active adapter logic is now designed to answer these questions:

- can rodata be mapped before any text remap happens?
- can data/bss be mapped before the executing text page is touched?
- if text fails, does it fail on the first text page in general or only when
  the currently executing page is remapped?

Expected interpretation on the next run:

- `phase=44`, `value0=0`, `value1=0x80205000`
  - stall on rodata flat mapping
- `phase=46`, `value0=1`, `value1=0x80207000`
  - data chunk mapping has started
- `phase=46`, `value0=2`, `value1=0x80200000`
  - text chunk mapping has started on a non-current page
- `phase=46`, `value0=2`, `value1=0x80201000`
  - the currently executing text page is being remapped last

### Runtime result: first reordered Zephyr range is now rodata

Observed on FVP after reordering the payload-owned Zephyr ranges:

- `phase=44`
- `value0=0`
- `value1=0x80206000`
- `text="map_zephyr_range"`

Interpretation:

- the reordered bundle is active
- the first payload-owned Zephyr range is now the rodata region
- the previous stall at the broad image base (`0x80200000`) is gone
- the current failure has moved to the first rodata mapping attempt

This is still progress: the adapter is now reaching a later, more specific
mapping step.

### Follow-up refinement: chunk rodata page-by-page too

To localize the rodata failure with the same granularity as the later data/text
ranges, the payload-owned MMU adapter now chunks rodata page-by-page as well.

Expected interpretation on the next run:

- `phase=46`, `value0=0`, `value1=0x80206000`
  - first rodata page stalls
- `phase=46`, `value0=0`, `value1=0x80207000`
  - at least one rodata page mapped successfully
- `phase=46`, `value0=1`, ... or `phase=44`, `value0=1`, ...
  - rodata finished and the adapter moved on to the data range

### Follow-up refinement: defer the first rodata page

The first rodata page at `0x80206000` is special in this image:

- it contains `initlevel`
- it contains `device_area`
- it contains `_sw_isr_table`
- and only later in the same 4 KiB page does the ordinary rodata content begin

To test whether this mixed first page is the real blocker, the adapter now:

- skips the first rodata page during the initial rodata chunk loop
- maps any later rodata pages first
- maps `0x80206000` last

Expected interpretation on the next run:

- `phase=46`, `value0=0`, `value1=0x80207000`
  - at least one later rodata page maps before the mixed first page
- `phase=46`, `value0=0`, `value1=0x80206000` with
  `text="map_zephyr_chunk_first_rodata"`
  - the adapter made it back to the special first rodata page

If the stall remains only on `0x80206000`, then the remaining issue is likely
specific to that mixed first page rather than to rodata pages in general.

### Runtime result: later rodata page also stalls

Observed on FVP after deferring the mixed first rodata page:

- `phase=46`
- `value0=0`
- `value1=0x80207000`
- `text="map_zephyr_chunk"`

Interpretation:

- the adapter successfully skipped the first mixed rodata page at `0x80206000`
- it then reached the next rodata page at `0x80207000`
- the stall therefore is not limited to the special first page contents

This weakens the "mixed first rodata page" hypothesis and points more strongly
to a rodata-wide permission or attribute problem.

### Follow-up refinement: temporarily map rodata as RW

To distinguish "RO mapping permissions are the issue" from "these pages cannot
be mapped at all", the payload adapter now maps the rodata window with writable
permissions for diagnosis:

- previous attr: `MT_NORMAL | MT_P_RO_U_RO | MT_DEFAULT_SECURE_STATE`
- temporary attr: `MT_NORMAL | MT_P_RW_U_NA | MT_DEFAULT_SECURE_STATE`

Expected interpretation on the next run:

- if the adapter moves beyond `value0=0` into later ranges, the blocker is
  likely tied to RO permissions
- if it still stalls on the same rodata page, the blocker is more likely tied
  to the page addresses/range state themselves

### Runtime result: writable rodata still stalls on the same page

Observed on FVP after temporarily mapping rodata as RW:

- `phase=46`
- `value0=0`
- `value1=0x80207000`
- `text="map_zephyr_chunk"`

Interpretation:

- changing rodata permissions from RO to RW did not move the failure
- this makes a pure RO-permission explanation less likely
- the remaining blocker is more likely in the address/range-specific
  `add_map()` / `set_mapping()` path rather than the final access flags

### Follow-up refinement: add a scratch RW mapping probe

Before mapping any Zephyr-owned ranges, the payload adapter now attempts one
plain RW mapping on a late image page near `_image_ram_end`.

New diagnostic phases:

- `0x2f` (`47`): `scratch_map_before`
- `0x30` (`48`): `scratch_map_after`

Expected interpretation on the next run:

- stop at `47`
  - generic early `add_map()` path stalls even on a plain RW scratch page
- reach `48` and then later stall on `46`
  - generic mapping works, but the failure is specific to the Zephyr-owned
    range addresses

### Runtime result: scratch map stalls before returning

Observed on FVP after adding the scratch probe:

- `phase=47`
- `value0=0x80236000`
- `value1=0x80208000`
- `text="scratch_map_before"`

Interpretation:

- the adapter does not return even from a plain RW scratch-page mapping
- so the failure is not limited to rodata pages
- this points back to the adapter's MMU internal state rather than any one
  Zephyr range

### Root-cause candidate found: wrong `kernel_ptables` offset

While validating the payload-side MMU helper against the rebuilt ELF:

- `new_table()` offset remained correct
- `__add_map.constprop.0` offset remained correct
- `invalidate_tlb_all()` offset remained correct
- but `kernel_ptables` had moved to:
  - `0x802105f0`
- while the payload helper was still using:
  - `z_arm64_mm_init + 0x0bdbc`
  - which resolved to `0x8020f984`

The payload-side status log already reflected this mismatch:

- the earlier `new_table` status published `value1=0x8020121c`
- which is not the real `kernel_ptables` address

This means the adapter was writing `base_xlat_table` into the wrong global
location before calling `__add_map.constprop.0`, which is consistent with the
scratch-map stall.

Action taken:

- updated `REALM_MMU_KERNEL_PTABLES_OFFSET` to `0x0ca28`

Expected next interpretation:

- if the next run reaches `48`, the wrong `kernel_ptables` pointer was the
  dominant blocker
- if it still stops at `47`, then another MMU-internal state dependency remains

### Follow-up finding: scratch page overlapped Zephyr stack/noinit

While re-checking the rebuilt payload layout, the scratch probe address from the
latest runtime result turned out not to be a neutral test page:

- observed scratch probe page:
  - `0x80236000`
- current payload symbols show:
  - `z_main_stack` at `0x80233590`
  - `z_interrupt_stacks` at `0x80235590`
  - `_image_ram_end` at `0x80237000`

So the previous scratch calculation:

- `_image_ram_end - PAGE_SIZE`

landed inside Zephyr's `.noinit` stack window, not in an otherwise-unused RAM
page. That means the `phase=47(scratch_map_before)` result was not yet a clean
proof of a generic `__add_map()` failure.

Action taken:

- changed the payload MMU probe to use a fixed unused RAM page outside the
  payload image:
  - `0x80400000`

Why this matters:

- if the next run still stops at `47`, the result now much more strongly points
  to the generic early `add_map()` path
- if it reaches `48`, then the earlier `47` was likely caused by choosing a
  stack-adjacent scratch page rather than by `add_map()` itself

### Follow-up finding: `kernel_ptables` offset drifted again after rebuild

After the scratch-page cleanup still yielded:

- `phase=47`
- `value0=0x80400000`
- `text="scratch_map_before"`

the payload-side MMU helper was revalidated against the current rebuilt ELF.

Current symbol addresses:

- `new_table            = 0x8020388c`
- `invalidate_tlb_all   = 0x8020390c`
- `__add_map.constprop.0= 0x80203b58`
- `z_arm64_mm_init      = 0x80203c14`
- `kernel_ptables       = 0x802105f0`

This means the current correct relative offsets are:

- `new_table`             : `z_arm64_mm_init - 0x388`
- `__add_map.constprop.0` : `z_arm64_mm_init - 0x0bc`
- `invalidate_tlb_all`    : `z_arm64_mm_init - 0x308`
- `kernel_ptables`        : `z_arm64_mm_init + 0x0c9dc`

The first three still matched the payload helper, but `kernel_ptables` no
longer did:

- payload helper still used `+0x0ca28`
- current ELF requires `+0x0c9dc`

Reason:

- the current `z_arm64_mm_init()` moved by `0x4c`
- but the payload-side `kernel_ptables` offset was left at the previous build's
  value

Action taken:

- updated `REALM_MMU_KERNEL_PTABLES_OFFSET` to `0x0c9dc`

Expected interpretation on the next run:

- if the next run reaches `48`, the stale `kernel_ptables` offset was the
  dominant blocker behind the safe scratch-map stall
- if it still stops at `47`, then another MMU-internal dependency remains

### Runtime result: payload MMU probe completes through `mmu_sync`

Observed on FVP after correcting the `kernel_ptables` offset:

- `phase=43`
- `text="mmu_sync"`
- `value0=0x80208000`
- `value1=3`

Interpretation:

- the payload-owned MMU adapter now completes:
  - scratch probe
  - Zephyr ranges
  - device ranges
  - TLB/cache synchronization
- this is a major step forward from the earlier `47` / `46` stalls
- the current blocker has moved past `realm_payload_mmu_probe()`

New hypothesis:

- after `mmu_sync`, the payload stub calls `z_arm64_mm_init(false)` to do the
  final MAIR/TCR/TTBR0/SCTLR enable sequence
- once the MMU is enabled, the status pointer inherited from the shim must
  remain mapped, otherwise the next payload phase write can fault immediately

Action taken:

- explicitly added the shared status page itself to the payload-owned page
  tables before final enable
- added two new diagnostic phases:
  - `0x31`: `status_map_before`
  - `0x32`: `status_map_after`

Expected interpretation on the next run:

- if `0x31/0x32` appear and then `0x23` appears:
  - final MMU enable succeeded and post-enable phase writes are now valid
- if `0x31/0x32` appear but `0x23` still does not:
  - the remaining blocker is inside the final `z_arm64_mm_init(false)` enable
    sequence itself

### Follow-up finding: `kernel_ptables` drifted again after status-map patch

After adding the status-page mapping step, the next FVP run unexpectedly fell
back to:

- `phase=47`
- `text="scratch_map_before"`

Re-checking the rebuilt ELF showed why:

- `new_table            = 0x802038d0`
- `invalidate_tlb_all   = 0x80203950`
- `__add_map.constprop.0= 0x80203b9c`
- `z_arm64_mm_init      = 0x80203c58`
- `kernel_ptables       = 0x802105f0`

The relative offsets are now:

- `new_table`             : `z_arm64_mm_init - 0x388`
- `__add_map.constprop.0` : `z_arm64_mm_init - 0x0bc`
- `invalidate_tlb_all`    : `z_arm64_mm_init - 0x308`
- `kernel_ptables`        : `z_arm64_mm_init + 0x0c998`

The helper still had `REALM_MMU_KERNEL_PTABLES_OFFSET = 0x0c9dc`, which was
correct for the previous build but stale for the current one. In other words,
adding the new status-map step shifted `z_arm64_mm_init()` again, and only the
`kernel_ptables` offset needed to move with it.

Action taken:

- updated `REALM_MMU_KERNEL_PTABLES_OFFSET` to `0x0c998`

Expected interpretation on the next run:

- if the payload again reaches `43(mmu_sync)`, then the fallback to `47` was
  explained by this rebuilt-ELF offset drift
- if it moves beyond `43` into `0x31/0x32` or `0x23`, then the status-page map
  fix is also working as intended

### Follow-up note: payload-entry instrumentation shifted MMU-relative symbols again

After adding the explicit pre-final-enable phase (`0x33`) and rebuilding, the
MMU helper offsets were checked one more time against the current ELF.

Current symbols:

- `new_table            = 0x802038f0`
- `invalidate_tlb_all   = 0x80203970`
- `__add_map.constprop.0= 0x80203bbc`
- `z_arm64_mm_init      = 0x80203c78`
- `kernel_ptables       = 0x802105f0`

The relative offsets remain stable for the helper functions:

- `new_table`             : `-0x388`
- `__add_map.constprop.0` : `-0x0bc`
- `invalidate_tlb_all`    : `-0x308`

But `kernel_ptables` moved again to:

- `z_arm64_mm_init + 0x0c978`

Reason:

- adding diagnostics in `payload_entry.S` shifted the text layout again
- this helper still relies on a relative offset from `z_arm64_mm_init` to the
  `kernel_ptables` global, so it must be refreshed whenever the final link
  layout changes

Action taken:

- updated `REALM_MMU_KERNEL_PTABLES_OFFSET` to `0x0c978`

### Runtime result: back to `43(mmu_sync)` after offset correction

Observed on FVP after updating `REALM_MMU_KERNEL_PTABLES_OFFSET` to `0x0c998`:

- `phase=43`
- `text="mmu_sync"`

Interpretation:

- the fallback to `47` was indeed caused by the stale rebuilt-ELF offset
- the payload-owned MMU adapter is back to completing fully
- the remaining uncertainty is whether:
  - `realm_payload_mmu_probe()` returns and the next phase is simply too fast
    for the watcher, or
  - execution stalls immediately inside `z_arm64_mm_init(false)`

Action taken:

- added an explicit pre-enable phase:
  - `0x33`: `payload_before_final_mmu_enable`
- inserted a short payload-side spin delay after publishing `0x33`

Why this helps:

- if the next run shows `0x33`, then the probe definitely returned and control
  reached the final MMU-enable call site
- if `0x33` appears but `0x23` does not, then the blocker is strongly localized
  to `z_arm64_mm_init(false)` itself

### Runtime result: `0x33(payload_before_final_mmu_enable)` reached

Observed on FVP:

- `phase=51(payload_before_final_mmu_enable)`
- `value0=2149613568`
- `value1=3`
- `text="mmu_sync"`

Interpretation:

- `realm_payload_mmu_probe()` returned successfully
- control reached the final MMU-enable boundary
- the remaining blocker is no longer the payload-owned page-table setup path
- it is now inside the final EL1 MMU enable sequence itself

### Adapter refinement: stabilize `kernel_ptables` lookup and inline final enable

The payload adapter was still using a `kernel_ptables` address derived from a
relative offset to `z_arm64_mm_init()`. That had already drifted multiple times
whenever the payload text layout changed.

The current ELF layout shows a more stable anchor:

- `__bss_start = 0x80208000`
- `kernel_ptables = 0x802105f0`
- therefore `kernel_ptables - __bss_start = 0x85f0`

Action taken:

- changed the payload helper to derive `kernel_ptables` from `__bss_start`
  instead of `z_arm64_mm_init()`
- replaced the direct `bl z_arm64_mm_init(false)` call with a
  payload-owned final-enable helper that mirrors Zephyr's EL1 sequence:
  - write `MAIR_EL1`
  - write `TCR_EL1`
  - write `TTBR0_EL1`
  - barrier
  - update `SCTLR_EL1`
  - barrier
- added fine-grained phases for this boundary:
  - `0x34`: `payload_final_mair_before`
  - `0x35`: `payload_final_mair_after`
  - `0x36`: `payload_final_tcr_after`
  - `0x37`: `payload_final_ttbr0_after`
  - `0x38`: `payload_final_sctlr_before`
  - `0x39`: `payload_final_sctlr_after`
  - `0x3a`: `payload_final_isb_after`

Expected interpretation on the next run:

- if the payload now reaches any of `0x34..0x3a`, the previous `0x33` result
  is confirmed and the exact sysreg boundary becomes visible
- if the payload still never moves beyond `0x33`, then the failure lies before
  the payload-owned helper itself, which would be unexpected given the current
  control flow

### Runtime result: stall at `0x38(payload_final_sctlr_before)`

Observed on FVP:

- `phase=56(payload_final_sctlr_before)`
- `value0=819271804`
- `value1=5`
- `text="final_sctlr_before"`

Interpretation:

- the payload-owned final-enable helper executed successfully through:
  - `MAIR_EL1`
  - `TCR_EL1`
  - `TTBR0_EL1`
- execution stalls at the boundary where `SCTLR_EL1.M | SCTLR_EL1.C` would
  actually enable translation and dcache

Most likely cause:

- the Realm payload is still built with:
  - `CONFIG_ARM64_VA_BITS = 32`
  - `CONFIG_ARM64_PA_BITS = 32`
- but the Realm shared alias bit is `1 << 32`, so shared VA/PA addresses sit
  above the 32-bit limit
- once translation is enabled, these addresses become invalid under the current
  `TCR_EL1` programming, even though they were usable with the MMU off

Action taken:

- updated the payload board defconfig to:
  - `CONFIG_ARM64_VA_BITS_36=y`
  - `CONFIG_ARM64_PA_BITS_36=y`

Expected interpretation on the next run:

- if the payload now advances beyond `0x38`, the 32-bit VA/PA limitation was
  the real blocker
- if it still stalls at `0x38`, the next investigation should focus on which
  exact post-enable VA (status page, current code page, or stack page) is still
  missing from the new tables

### Runtime result: reached `0x25(payload_before_cstart)`

Observed on FVP after rebuilding with `VA_BITS=36` / `PA_BITS=36`:

- `phase=37(payload_before_cstart)`
- `text="final_isb_after"`

Interpretation:

- the final EL1 MMU enable sequence completed far enough to return from the
  payload-owned helper
- execution also passed:
  - `payload_after_mmu_init`
  - `payload_after_interrupt_init`
  quickly enough that the watcher only caught the last visible pre-`z_cstart`
  phase
- this means the previous stall at `payload_final_sctlr_before` was consistent
  with the 32-bit VA/PA limitation hypothesis

Note:

- the accompanying `text="final_isb_after"` string is stale data from the
  previous status update because the assembly-side `publish_phase` helper only
  changed the phase field

Action taken:

- updated the assembly status helper to clear `value0`, `value1`, and the first
  text word on each phase-only publish so later logs do not retain stale text

Next expected runtime evidence:

- `0x20`: `payload_main`
- `0x21`: `payload_heartbeat`
- or UART output from the payload app

### App-runtime refinement: remove late `device_map()` dependency from payload app

After reaching `payload_before_cstart`, the most likely next blocker was no
longer Realm MMU bring-up itself but the app-side status publication path.

The previous payload app logic did this:

- `main()`
  - `device_map()` the shim handoff page
  - read `status_shared_addr` from that page
  - `device_map()` the status page again
  - publish `payload_main` / `payload_heartbeat`

That late remap step is unnecessary and risky in the Realm path because the
payload already knows the shared status VA before entering `z_cstart()`.

Action taken:

- added payload-owned runtime globals:
  - `realm_payload_status_shared_addr`
  - `realm_payload_shared_alias_bit`
  - `realm_payload_dtb_addr`
- in `payload_entry.S`, stored those globals immediately before branching to
  `z_cstart()`
- added phase:
  - `0x3b`: `payload_runtime_ctx_ready`
- simplified `main()` to publish directly to the already-mapped shared status
  page instead of using `device_map()`
- replaced `k_sleep()` heartbeat with a simple busy loop because this payload
  board intentionally disables the system clock during early Realm bring-up

Expected interpretation on the next run:

- if the payload reaches `0x3b`, the runtime context is ready before `z_cstart`
- if `0x20(payload_main)` or `0x21(payload_heartbeat)` appears, then the
  standalone-shim path has progressed all the way into the Zephyr app itself

### Follow-up note: `kernel_ptables` BSS offset drifted after app-runtime refinement

After adding the runtime-context globals for the payload app and rebuilding, the
payload BSS layout shifted slightly again.

Current ELF symbols:

- `__bss_start       = 0x80208000`
- `kernel_ptables    = 0x802105f8`

Therefore:

- `kernel_ptables - __bss_start = 0x85f8`

The payload helper still had:

- `REALM_MMU_KERNEL_PTABLES_BSS_OFFSET = 0x85f0`

That stale offset explains why a freshly rebuilt bundle could fall back to:

- `0x2f`: `scratch_map_before`

again, even though previous runtime results had already progressed as far as
`payload_before_cstart`.

Action taken:

- updated `REALM_MMU_KERNEL_PTABLES_BSS_OFFSET` to `0x85f8`

Expected interpretation on the next run:

- if the payload returns to `0x33/0x37/0x3b` territory, this was simply another
  rebuilt-ELF offset drift
- if not, the next step is to remove this remaining manual offset dependency
  entirely from the payload MMU adapter path

### Follow-up note: split `z_cstart -> bg_thread_main -> main` with app-owned init phases

After recovering back to `0x25(payload_before_cstart)`, the remaining blind spot
is the path after `z_cstart()` starts executing.

To keep the Zephyr kernel unmodified while still making progress visible, the
payload app now registers four tiny `SYS_INIT()` hooks and publishes explicit
phases from each init level:

- `0x60`: `payload_init_pre_kernel_1`
- `0x61`: `payload_init_pre_kernel_2`
- `0x62`: `payload_init_post_kernel`
- `0x63`: `payload_init_application`

This gives a clean split:

- `0x25` only: branch to `z_cstart()` / very-early `z_cstart()` issue
- `0x60/0x61` but no `0x62`: `z_cstart()` runs, but main-thread switch stalls
- `0x62/0x63` but no `0x20`: `bg_thread_main()` runs, but app handoff stalls
- `0x20/0x21`: the Zephyr payload app itself is alive in Realm

Verification after rebuild:

- `payload_init_pre_kernel_1  = 0x80201a38`
- `payload_init_pre_kernel_2  = 0x80201a6c`
- `payload_init_post_kernel   = 0x80201aa0`
- `payload_init_application   = 0x80201ad4`

and the init linker ranges are now non-empty:

- `__init_PRE_KERNEL_1_start = 0x80206000`
- `__init_PRE_KERNEL_2_start = 0x80206040`
- `__init_POST_KERNEL_start  = 0x80206050`
- `__init_APPLICATION_start  = 0x80206060`
- `__init_end                = 0x80206070`

So the next run can distinguish:

- very-early `z_cstart()` failure
- failure while switching into the main thread
- failure after `bg_thread_main()` starts but before `main()`
- successful `main()` / heartbeat execution

Additional PRE_KERNEL_1 split:

- `0x64` / `100`: `payload_init_after_intc`
- `0x65` / `101`: `payload_init_after_serial`

For this board, PRE_KERNEL_1 now effectively brackets:

- app marker at priority `0`
- GIC device init at priority `40`
- NS16550 serial device init at priority `50`
- app marker at priority `45`
- app marker at priority `55`

Expected interpretation:

- only `0x60`: stall before / inside interrupt-controller init
- `0x64` but no `0x65`: stall in UART device init
- `0x65` but no `0x61`: stall after serial but before PRE_KERNEL_2
- `0x61` and beyond: PRE_KERNEL_1 completed

Follow-up note:

`phase=100(init_after_intc)` with no later PRE_KERNEL_1 marker strongly points
at the NS16550 device init path. For the Realm payload board, the UART address
already uses the shared-alias MMIO address (`0x101000000`) and is already
covered by the payload-owned MMU setup, so the board DTS now marks the UART as
`io-mapped;`.

That causes `uart_ns16550_init()` to skip the early `DEVICE_MMIO_MAP()` path and
use the provided shared-alias address directly, which fits the standalone-shim
bring-up model better and avoids another early mapping operation inside the
driver.

Follow-up correction:

The `io-mapped` DTS attempt is not viable on this arm64 payload. In Zephyr's
`uart_ns16550` driver that property routes accesses through `sys_in8/sys_out8`
style helpers, which are not available here and cause link failure.

Next step:

- keep the normal UART board path unchanged
- add a temporary **headless bring-up** overlay (`prj.headless.conf`) that
  disables `SERIAL`, `CONSOLE`, and `UART_CONSOLE`
- use the shared status page only to prove that Zephyr can progress from
  `z_cstart()` into `main()` without depending on the early NS16550 init path

This preserves the long-term goal (Realm-aware UART and shell support) while
unblocking the immediate proof that a basic Zephyr app can execute inside the
Realm.

Environment note:

- rebuilding `kvmtool-cca` locally to refresh phase-name strings is currently
  blocked on the host build environment (`asm/types.h` missing when attempting
  an arm64 cross build)
- this does not block bring-up debugging because the payload still publishes
  numeric phase values in the shared status page
- interpret the new app-owned init phases numerically if needed:
  - `0x60` / `96`: `payload_init_pre_kernel_1`
  - `0x61` / `97`: `payload_init_pre_kernel_2`
  - `0x62` / `98`: `payload_init_post_kernel`
  - `0x63` / `99`: `payload_init_application`

Breakthrough result:

A subsequent run of the headless Realm bundle produced:

- `phase=51(payload_before_final_mmu_enable)`
- `phase=32(payload_main)` with `text="payload main entered"`
- `phase=33(payload_heartbeat)` with `text="payload heartbeat"`

This is the first end-to-end proof that the standalone `Realm shim` can boot an
unmodified Zephyr payload far enough for:

- Realm entry and shim-side RSI/config handling
- payload handoff
- payload-owned MMU setup
- `z_cstart()`
- Zephyr init levels
- `main()`
- repeated application heartbeat execution

Interpretation:

- the temporary headless configuration successfully bypassed the PRE_KERNEL_1
  UART initialization stall
- the bring-up problem is no longer "can Zephyr execute in a Realm?" but "how
  do we restore a Realm-safe console/shell path on top of a now-working app
  runtime?"

This changes the project state from bring-up/debugging into application and
runtime integration. The next concrete steps are:

- preserve this headless path as the known-good baseline
- swap the heartbeat demo for the attestation app logic
- expose attestation status/results through the shared status page first
- then reintroduce a Realm-safe UART / shell path for interactive execution

Realm-safe UART / shell follow-up:

To move from the headless proof into interactive app control, an out-of-tree
Realm-safe polling UART path was added instead of modifying Zephyr kernel code.
The payload board now uses a custom compatible (`lkvm,realm-uart`) and an app
local driver (`src/realm_uart.c`) that:

- accepts the Realm shared-alias MMIO address directly from DTS
- performs only minimal NS16550 setup
- exposes polling-only `uart_poll_in()` / `uart_poll_out()`
- avoids the `uart_ns16550_init()` PRE_KERNEL_1 path that previously stalled

In parallel, a new shell profile (`prj.shell.conf`) was added:

- keeps the standalone shim / unmodified Zephyr payload model
- enables `CONFIG_SHELL`
- uses the serial shell backend in polling mode
- turns off the regular UART console path so the shell can use the new UART
  transport without depending on `UART_CONSOLE`
- re-enables the system clock and ARM architectural timer because the stock
  serial shell backend polling transport uses `k_timer`

The shell profile now builds successfully with:

```bash
SHELL_MODE=1 ./scripts/build-realm-zephyr-shim-bundle.sh
```

This produces a new bundle with:

- the standalone Realm shim at `0x80000000`
- the Zephyr payload entry at `0x80201050`
- a polling UART driver bound to `/soc/uart@101000000`
- shell commands registered in the payload app:
  - `realm status`
  - `realm ping`

Interpretation:

- headless mode remains the known-good application baseline
- shell mode is now the next validation target for interactive command-driven
  app execution
- once the Realm shell prompt appears, the project can move from fixed app
  loops into command-triggered attestation flows

UART smoke follow-up:

- added a POST_KERNEL UART smoke hook to `src/vecu_zephyr/src/main.c`
- this uses `DT_CHOSEN(zephyr_shell_uart)` plus `uart_poll_out()` directly
  instead of waiting for the Zephyr shell prompt
- new status phases:
  - `0x67`: `uart_smoke_before`
  - `0x68`: `uart_smoke_after`

Purpose:

- if `init_post_kernel` is reached and the host sees:
  - status `uart_smoke_after`, or
  - the literal line `[realm-uart] post-kernel smoke`
  on `telnet localhost 5002`
- then the custom Realm-safe polling UART path is alive and the remaining issue
  is shell autostart / shell backend bring-up rather than raw UART access

Verification:

- `SHELL_MODE=1 ./scripts/build-realm-zephyr-shim-bundle.sh` rebuilds cleanly
- the current bundle contains the expected smoke markers:
  - `uart_smoke_before`
  - `uart_smoke_after`
  - `[realm-uart] post-kernel smoke`

PRE_KERNEL_1 UART init simplification:

- repeated Realm shell runs still stopped at `phase=100(init_after_intc)`
  without reaching `init_after_serial`
- that narrows the stall to the custom polling UART driver's PRE_KERNEL_1 init
- for the current bring-up phase, `src/realm_uart.c` now:
  - publishes `uart_drv_init_before` / `uart_drv_init_after`
  - avoids programming UART registers in `realm_uart_init()`
  - simply marks the device ready so later `uart_poll_out()` smoke and shell
    backend code can use raw THR/LSR access directly

Rationale:

- the immediate goal is to prove the Realm-safe UART datapath and shell prompt
  path, not to finalize full NS16550-style configuration
- if this unblocks `init_after_serial` and the POST_KERNEL smoke line, the next
  remaining issue is shell backend behavior, not UART bring-up itself

Latest runtime result:

- after simplifying `realm_uart_init()`, the shell bundle no longer stalls at
  `phase=100(init_after_intc)`
- the guest now reaches:
  - `phase=97(payload_init_pre_kernel_2)`
  - `text="init_pre_kernel_2"`

Interpretation:

- the custom Realm-safe UART driver is no longer the first blocking step
- built-in shell bring-up has advanced past PRE_KERNEL_1 into PRE_KERNEL_2
- the remaining stall moved later, most likely around the scheduler / main
  thread handoff that must occur before POST_KERNEL, APPLICATION, and the shell
  backend thread become visible

Next focus:

- instrument or narrow the PRE_KERNEL_2 -> POST_KERNEL transition
- determine whether the next blocker is:
  - timer bring-up
  - thread start / scheduler handoff
  - shell backend thread startup

Mini-shell fallback on top of the proven baseline:

The built-in Zephyr shell profile kept reintroducing earlier MMU bring-up
regressions (`scratch_map_before`) before the payload could reach the shell
backend. To keep forward progress and preserve the "unmodified Zephyr kernel"
goal, the next step was to add an application-level UART mini shell on top of
the already proven `payload_main` path instead of blocking on `CONFIG_SHELL`.

Implemented changes:

- added `prj.minishell.conf` to keep the headless bring-up profile
  (`UART_CONSOLE=n`, `CONSOLE=n`, `SERIAL=n`) while still compiling the app
- added `MINI_SHELL_MODE=1` support to `scripts/build-vecu-zephyr.sh`
- added `REALM_MINI_SHELL` compile flag wiring in `src/vecu_zephyr/CMakeLists.txt`
- linked the existing attestation helpers (`src/rsi.c`, `src/rsi_asm.S`) into
  the payload app
- excluded the experimental `realm_uart.c` driver from mini-shell builds so
  there is no PRE_KERNEL serial-device init dependency
- extended `src/vecu_zephyr/src/main.c` with:
  - direct polling UART MMIO helpers using the shim-provided shared alias bit
  - a `realm:~$ ` prompt
  - commands:
    - `help`
    - `status`
    - `ping`
    - `attest`

The mini-shell bundle now builds successfully with:

```bash
MINI_SHELL_MODE=1 ./scripts/build-realm-zephyr-shim-bundle.sh
```

Verification performed locally:

- `strings src/realm_shim/build/realm_zephyr_shim.bin` contains:
  - `Realm mini shell ready`
  - `realm:~$ `
  - `commands: help status ping attest`
  - `attest: token ready size=`

Interpretation:

- the project now has a practical interactive path that does not depend on the
  built-in Zephyr shell surviving early Realm bring-up
- the next FVP validation target is:
  - prompt appears on `telnet localhost 5002`
  - `status` works
  - `ping` works
  - `attest` generates a token and reports success

arm64 kvmtool helper:

An `Exec format error` occurred in FVP Linux when `/root/lkvm-static` was
replaced with a host-built x86-64 binary. To make the correct build path
repeatable, a dedicated helper script was added:

- `scripts/build-kvmtool-arm64.sh`

This helper now:

- cross-builds `libfdt` with the arm64 Linux toolchain
- cross-builds `dev_workspace/kvmtool-cca/lkvm-static` with:
  - `ARCH=arm64`
  - `CROSS_COMPILE=aarch64-none-linux-gnu-`
  - `LIBFDT_DIR=.../dev_workspace/libfdt-src/libfdt`

Result:

- `dev_workspace/kvmtool-cca/lkvm-static` is now verified as:
  - `ELF 64-bit, ARM aarch64, statically linked`

Operational note:

- the mini-shell validation requires uploading both:
  - the fresh `realm_zephyr_shim.bin`
  - the fresh arm64 `lkvm-static`

Mini-shell MMU regression fix:

The mini-shell bundle still stalled at `phase=47(scratch_map_before)` even
though the bundle contained the new prompt strings. Re-checking the current
`z_arm64_mm_init()` disassembly showed why: the payload probe still assumed a
fixed BL offset for `__add_map.constprop.0()`, but the mini-shell build had
shifted that call from the previously used offset.

Fix:

- `src/vecu_zephyr/src/payload_mmu.c`
  - stop hardcoding BL offsets for:
    - `new_table()`
    - `__add_map.constprop.0()`
    - `invalidate_tlb_all()`
  - instead, scan the current `z_arm64_mm_init()` body and decode:
    - 1st BL -> `new_table`
    - 2nd BL -> `__add_map`
    - 3rd BL -> `invalidate_tlb_all`

Result:

- `MINI_SHELL_MODE=1 ./scripts/build-realm-zephyr-shim-bundle.sh` now rebuilds
  a fresh bundle using a less brittle MMU probe decoder
- the next FVP validation should confirm whether the mini-shell path advances
  past `scratch_map_before`

Mini-shell shell-core integration:

The built-in serial shell path progressed to `phase=97(init_pre_kernel_2)`,
which means the earlier PRE_KERNEL_1 UART init blocker was gone but
`sys_clock_driver_init()` still blocked the serial backend path before
`POST_KERNEL`.

To stop fighting that timer-coupled backend, the mini-shell profile was
reworked so the app-owned polling UART loop becomes a transport for the
Zephyr shell core instead:

- `src/vecu_zephyr/prj.minishell.conf`
  - enable `CONFIG_SHELL=y`
  - enable `CONFIG_SHELL_BACKEND_DUMMY=y`
  - disable `CONFIG_SHELL_BACKEND_SERIAL`
  - enable `CONFIG_SYS_CLOCK_EXISTS=y`
- `src/vecu_zephyr/src/main.c`
  - include `shell_dummy.h`
  - replace the manual command parser with `shell_execute_cmd()`
  - forward dummy backend output back over the raw Realm-safe UART loop
  - register `realm attest` as a real Zephyr shell command
- `src/vecu_zephyr/CMakeLists.txt`
  - compile `realm_uart.c` only when `CONFIG_SERIAL=y`
  - add `realm_timer.c` only for `REALM_MINI_SHELL`
- `src/vecu_zephyr/src/realm_timer.c`
  - add a lightweight polling-only system clock provider for the mini-shell
    profile so shell core + dummy backend can link without the built-in
    arch timer driver

Local validation:

- `MINI_SHELL_MODE=1 ./scripts/build-realm-zephyr-shim-bundle.sh` now builds
  successfully again
- generated `.config` confirms:
  - `CONFIG_SYS_CLOCK_EXISTS=y`
  - `CONFIG_SHELL_BACKEND_DUMMY=y`
- the final bundle contains:
  - `Realm mini shell ready`
  - `payload main entered`
  - `token ready size=0x%lx buffer=0x%lx`

Next FVP validation target:

- prompt appears on `telnet localhost 5002`
- `help` returns shell-managed output
- `realm status` works
- `realm ping` works
- `realm attest` generates and reports a token

Mini-shell UART init stall fix:

FVP validation with the shell-core-on-mini-transport bundle still reached only:

- `phase=32(payload_main)`

and never published:

- `payload_mini_shell_ready`

That narrowed the new stall to `main()` immediately after entry, before the
mini prompt was shown. The remaining suspect was `mini_shell_uart_init()`,
which still reprogrammed the NS16550 registers directly even though the same
kind of early UART programming had already caused bring-up stalls elsewhere.

Fix:

- `src/vecu_zephyr/src/main.c`
  - make `mini_shell_uart_init()` side-effect free
  - stop rewriting divisor/LCR/FCR/MCR in the mini transport path
  - publish two new status markers:
    - `0x74 = mini_uart_init_before`
    - `0x75 = mini_uart_init_after`
- `dev_workspace/kvmtool-cca/builtin-run.c`
  - add phase names for `0x74/0x75`

Local validation:

- `MINI_SHELL_MODE=1 ./scripts/build-realm-zephyr-shim-bundle.sh` rebuilds
  successfully with the no-op mini UART init
- `./scripts/build-kvmtool-arm64.sh` rebuilds a fresh arm64
  `dev_workspace/kvmtool-cca/lkvm-static`

Next FVP validation target:

- `phase=74(payload_mini_uart_init_before)`
- `phase=75(payload_mini_uart_init_after)`
- `phase=70(payload_mini_shell_ready)`
- visible `realm:~$` prompt on `telnet localhost 5002`

Date: 2026-04-27

Observation:

- FVP shell-path run reported `phase=118(payload_mini_uart_cfg_before)`, which
  means the payload already reached `main()` and entered the app-owned
  mini-shell loop.
- That narrowed the remaining problem to prompt visibility and/or very-late
  raw UART writes, not to Realm bring-up or Zephyr runtime entry.

Refinement:

- `src/vecu_zephyr/src/main.c`
  - add `payload_publish_status_sticky()` and a short busy-wait hold for the
    mini-shell visibility checkpoints
  - make the following phases sticky so the host watcher is less likely to miss
    them:
    - `0x70 = mini_shell_ready`
    - `0x78 = mini_banner_before`
    - `0x79 = mini_banner_after`
    - `0x73 = mini_shell_prompt`

Local validation:

- `MINI_SHELL_MODE=1 ./scripts/build-realm-zephyr-shim-bundle.sh` rebuilds
  successfully with the sticky mini-shell phases

Next FVP validation target:

- `phase=112(payload_mini_shell_ready)`
- `phase=118(payload_mini_uart_cfg_before)`
- `phase=119(payload_mini_uart_cfg_after)`
- `phase=120(payload_mini_banner_before)`
- `phase=121(payload_mini_banner_after)`
- `phase=115(payload_mini_shell_prompt)`
- ideally visible `Realm mini shell ready` and `realm:~$` on `telnet localhost 5002`

Date: 2026-04-27

Observation:

- FVP reported:
  - `phase=112(payload_mini_shell_ready)`
  - `phase=120(payload_mini_banner_before)`
  - `phase=122(payload_mini_char_before)`
- That proves:
  - Realm shim handoff still works
  - Zephyr definitely reaches `main()`
  - the app-owned mini-shell loop definitely runs
  - the remaining stall is at the first raw UART character write

Diagnosis:

- The payload writes to the Realm shared-alias UART address
  `0x101000000`.
- kvmtool's 8250 serial emulation in `hw/serial.c` was only registering an
  MMIO trap for the non-shared address `0x01000000`.
- That mismatch fits the observed behavior: status-page writes continue to
  work, but the first UART THR write never returns.

Fix:

- `dev_workspace/kvmtool-cca/hw/serial.c`
  - compute the Realm shared alias bit from the guest IPA size
  - register the same 8250 MMIO trap at both:
    - `0x01000000`
    - `0x101000000` (for current 33-bit Realm IPA)
  - teach the MMIO handler to normalize alias addresses back to the base
    device offsets
- `dev_workspace/kvmtool-cca/kvm-cpu.c`
  - extend MMIO debug logging to also print accesses in the shared-alias UART
    window `0x101000000..0x101001000`

Expected next signal:

- `phase=123(payload_mini_char_after)` and/or UART MMIO writes to
  `0x101000000`
- ideally followed by `phase=121(payload_mini_banner_after)` and visible
  prompt output on `telnet localhost 5002`

Date: 2026-04-27

Observation:

- Even after adding the shared-alias UART trap to kvmtool, FVP still showed:
  - `phase=112(payload_mini_shell_ready)`
  - `phase=120(payload_mini_banner_before)`
  - `phase=122(payload_mini_char_before)`
- But there were still no alias-MMIO hit logs, which suggested the payload was
  not actually computing the UART alias base as `0x101000000`.

Diagnosis:

- In `src/vecu_zephyr/src/payload_entry.S`, the payload handoff path validated
  the shim magic but never loaded `shared_alias_bit` from the handoff
  structure.
- Later, `realm_payload_shared_alias_bit` was populated from `x23`, which
  still held a stale register value rather than the shim-provided alias bit.
- That explains the obviously wrong `value0` seen in `payload_mini_shell_ready`
  and why raw UART writes never hit the host-side alias trap.

Fix:

- `src/vecu_zephyr/src/payload_entry.S`
  - load `x23` from handoff offset `#16` (`shared_alias_bit`) before the
    runtime context is exported to C globals

Expected next signal:

- `payload_mini_shell_ready` should now report `value0 = 0x100000000`
- host debug should show `serial8250 alias mmio ... addr=0x101000000`
- then ideally `payload_mini_char_after`, `payload_mini_banner_after`, and a
  visible prompt on `telnet localhost 5002`

Date: 2026-04-27

Observation:

- FVP still showed `payload_mini_shell_ready value0=0x80201078`-like values
  instead of `0x100000000`, even after the initial handoff load fix.
- That means the payload reaches `main()`, but the C global
  `realm_payload_shared_alias_bit` is still being populated with a stale
  register value.

Fix:

- `src/vecu_zephyr/src/payload_entry.S`
  - re-read `shared_alias_bit` and `status_shared_addr` from
    `REALM_SHIM_HANDOFF_BASE` immediately before exporting the runtime context
    to C globals
  - this avoids relying on `x23` surviving all early Zephyr init calls

Expected next signal:

- `payload_mini_shell_ready value0=4294967296`
- then alias UART MMIO at `0x101000000`

Date: 2026-04-27

Observation:

- After re-reading the shim handoff page immediately before exporting the
  runtime context, FVP regressed to:
  - `phase=51(payload_before_final_mmu_enable)`
  - `phase=36(payload_after_interrupt_init)`
- It did not reach `payload_runtime_ctx_ready`, `payload_before_cstart`, or
  `payload_main`.

Diagnosis:

- The handoff page is located in the shim window at `0x801ff000`.
- That address is valid while running under the shim's early identity mapping,
  but it is not part of the Zephyr payload's final page-table contract.
- Re-reading the handoff page after `realm_payload_enable_mmu_final()` can
  fault or stall because the final payload MMU mapping intentionally focuses on
  the Zephyr image, RAM scratch areas, status page, DTB, and shared-alias MMIO.

Fix:

- `src/vecu_zephyr/src/payload_entry.S`
  - re-read `status_shared_addr`, `shared_alias_bit`, and `dtb_addr` from the
    shim handoff page immediately after `z_bss_zero`, then export them into C
    globals
  - this is late enough that `.bss` no longer wipes the globals, but early
    enough that the shim handoff values are still available without requiring a
    final-MMU mapping for the shim handoff page
  - remove the post-MMU handoff-page re-read

Expected next signal:

- no longer stuck at `phase=36(payload_after_interrupt_init)`
- `phase=112(payload_mini_shell_ready) value0=4294967296`
- host debug should show `serial8250 alias mmio ... addr=0x101000000`

Local validation:

- `MINI_SHELL_MODE=1 ./scripts/build-realm-zephyr-shim-bundle.sh` completed
  successfully after moving the runtime context export before final MMU enable.
- New bundle:
  - `src/realm_shim/build/realm_zephyr_shim.bin`
  - payload entry override: `0x80201070`
  - payload size: `245764` bytes
  - bundle image size: `2342916` bytes
  - SHA-256:
    `a4984636f28d1de7fa1297618ade0d57b2ed67727f0b5f8d65879376e6daef5a`

Date: 2026-04-27

Observation:

- FVP progressed past the previous `phase=36` regression and reached:
  - `phase=112(payload_mini_shell_ready) value0=4294967296`
  - `phase=120(payload_mini_banner_before) value0=4294967296`
  - `phase=122(payload_mini_char_before) value0=4294967296 value1=62`
- This proves the shim handoff export is now correct:
  - `4294967296 == 0x100000000`, the expected Realm shared alias bit
- The remaining stall happens at the first raw UART THR write.

Diagnosis:

- kvmtool registers the shared-alias serial trap at `0x101000000`, but no
  `serial8250 alias mmio` hit is observed before the stall.
- That suggests the access may still be blocked before reaching the host MMIO
  emulation path, most likely in the payload's stage-1 MMU mapping.

Fix / next diagnostic:

- `src/vecu_zephyr/src/payload_mmu.c`
  - explicitly map the shared-alias UART page from the payload's final page
    table builder:
    - VA: `0x101000000`
    - PA: `0x101000000`
    - size: `0x1000`
    - attrs: device, EL1 RW
  - publish:
    - `0x7c = payload_uart_alias_map_before`
    - `0x7d = payload_uart_alias_map_after`
  - put the `add_map()` return value in `value1` for `0x7d`
- `src/vecu_zephyr/boards/arm64/lkvm_realm_payload/lkvm_realm_payload_defconfig`
  - increase `CONFIG_MAX_XLAT_TABLES` from the Zephyr default to `16`, because
    the Realm payload maps low RAM plus several high shared-alias/device IPA
    windows.
- `dev_workspace/kvmtool-cca/builtin-run.c`
  - add phase names for `0x7c` and `0x7d`

Expected next signal:

- `phase=125(payload_uart_alias_map_after) ... value1=0`
- then either:
  - `serial8250 alias mmio ... addr=0x101000000` and
    `phase=123(payload_mini_char_after)`, or
  - a nonzero `value1`, meaning final page-table construction still failed for
    the UART alias mapping

Local validation:

- `MINI_SHELL_MODE=1 ./scripts/build-realm-zephyr-shim-bundle.sh` completed
  successfully after the explicit UART alias MMU mapping.
- New bundle:
  - `src/realm_shim/build/realm_zephyr_shim.bin`
  - payload entry override: `0x80201070`
  - payload size: `278532` bytes
  - bundle image size: `2375684` bytes
  - SHA-256:
    `8889f97a684c65c85755fa87aef269762643f1b4040c51e01eed2dc1cc5942f6`
- `./scripts/build-kvmtool-arm64.sh` also completed successfully after adding
  the `0x7c/0x7d` phase names.
- New `lkvm-static` SHA-256:
  `2db2178c9097dceb44aec82f2a2a6f71c5b685070b23c5db63ef8a9ce2632a2c`

Date: 2026-04-27

Observation:

- FVP produced a flood of:
  - `MMIO read addr=0x1000005 len=1 data0=0x60`
- `0x1000005` is UART LSR.
- `0x60` means `THRE | TEMT`, so transmit is empty and there is no received
  byte pending.

Interpretation:

- This is not a boot stall. It means the mini-shell has likely reached its
  input-poll loop and is repeatedly checking whether the user typed anything.
- The debug flood itself hides the guest prompt and makes telnet look broken.

Fix:

- `dev_workspace/kvmtool-cca/kvm-cpu.c`
  - stop logging UART LSR reads (`offset 0x5`)
  - keep logging UART writes and non-LSR reads so TX/output and RX data events
    are still visible without drowning the serial console

Follow-up observation:

- After suppressing the LSR data line, the console still flooded with:
  - `vcpu0 exit_reason=6`
- `6` is `KVM_EXIT_MMIO`; the mini-shell input loop legitimately generates one
  MMIO exit per UART LSR poll.

Follow-up fix:

- `dev_workspace/kvmtool-cca/kvm-cpu.c`
  - suppress the generic `exit_reason` debug line for `KVM_EXIT_MMIO`
  - rely on the filtered MMIO detail logger for UART writes and non-LSR reads

Local validation:

- `./scripts/build-kvmtool-arm64.sh` completed successfully.
- New `lkvm-static` SHA-256:
  `bb892992c59857633dbd4fbc505db14d26572afb7ef67d055cd70e8b73b9b7ff`

Expected next signal:

- telnet should be much quieter after replacing `lkvm-static`
- if the mini-shell is alive, type:
  - press Enter
  - `help`
  - `kernel version`
  - `realm attest`

Date: 2026-04-28

Observation:

- After suppressing MMIO exit noise, the console still flooded with:
  - `realm guest realm-vecu1 vcpu0 live pc=...`
- The PC kept changing through the polling path, so the live-PC trace thread
  kept printing even though the guest was not stuck.

Fix:

- `dev_workspace/kvmtool-cca/builtin-run.c`
  - keep the live-PC trace implementation, but do not start it by default
  - enable it only when `LKVM_LIVE_PC_TRACE` is set to a nonzero value

Local validation:

- `./scripts/build-kvmtool-arm64.sh` completed successfully.
- New `lkvm-static` SHA-256:
  `3b468aa035ca95d1b5e744b5f19336133917f61d24cf646f512ef4d9df90addc`

Expected next signal:

- `--debug` still shows Realm status-page phase transitions
- default telnet/log output no longer floods with live-PC samples
- if instruction-level liveness is needed later:
  - run lkvm with `LKVM_LIVE_PC_TRACE=1`

Date: 2026-04-28

Milestone: Zephyr Realm mini-shell prompt is visible

Observation:

- FVP/telnet reached:
  - `phase=112(payload_mini_shell_ready)`
  - `phase=120(payload_mini_banner_before)`
  - `phase=122(payload_mini_char_before)`
  - UART output: `>`
  - `phase=123(payload_mini_char_after)`
  - UART output: `Realm mini shell ready`
  - `phase=121(payload_mini_banner_after)`
  - UART output: `realm:~$ `
  - `phase=115(payload_mini_shell_prompt)`

Conclusion:

- The standalone Realm shim successfully transfers control into Zephyr.
- Zephyr reaches `main()`.
- The app-level mini-shell loop runs inside the Realm.
- Realm shared-alias UART output works end-to-end through kvmtool and telnet.
- This is the first concrete "Zephyr as a Realm payload is interactive"
  milestone.

Remaining polish:

- The console still interleaves UART characters with optional MMIO write debug
  lines when `--debug` is used.
- Make UART MMIO write logging opt-in so the prompt is clean by default.

Fix:

- `dev_workspace/kvmtool-cca/kvm-cpu.c`
  - gate UART MMIO detail logs behind `LKVM_UART_MMIO_TRACE`
  - default debug mode now keeps status-page logs, but no longer prints every
    UART character write

Local validation:

- `./scripts/build-kvmtool-arm64.sh` completed successfully.
- New `lkvm-static` SHA-256:
  `f37df5e3e66ed309746768ed2535a2855cd34be961a021031d9c58f5f9a1ecdb`

Expected next signal:

- With the next `lkvm-static`, telnet should show a clean:
  - `>`
  - `Realm mini shell ready`
  - `realm:~$ `
- If UART MMIO tracing is needed:
  - run lkvm with `LKVM_UART_MMIO_TRACE=1`

Date: 2026-04-28

Observation:

- Even after gating the `kvm-cpu.c` UART MMIO details, `--debug` could still
  hide the prompt because:
  - `hw/serial.c` printed `serial8250 alias mmio ...` directly
  - `scripts/run-vecu-zephyr.sh` merged stderr into stdout and tee'd both host
    debug and guest serial back to the telnet TTY

Fix:

- `dev_workspace/kvmtool-cca/hw/serial.c`
  - gate `serial8250 alias mmio ...` behind `LKVM_UART_MMIO_TRACE`
- `scripts/run-vecu-zephyr.sh`
  - in `--debug` background mode, route guest stdout to `/dev/ttyAMA2`
  - route host debug stderr only to `/root/<vm>.log`
  - no longer tee host debug logs into the telnet serial stream

Expected next signal:

- `telnet localhost 5002` should show only the guest UART stream
- `tail -f /root/realm-vecu1.log` should show host debug/status logs

Local validation:

- `./scripts/build-kvmtool-arm64.sh` completed successfully.
- New `lkvm-static` SHA-256:
  `d729b2a1d0c33515f98c3a56dec5623fc5a243f88081b4df184fcf60c16ab3dc`
- Updated `run-vecu-zephyr.sh` SHA-256:
  `e2a322934d03bca15a5e12cbbaf75a95ecd355604dd5e91317f3ac6aed91e131`

Date: 2026-04-28

Observation:

- The guest UART path was proven by `phase=123`, `phase=121`, `phase=115`,
  but the user still did not reliably see a shell prompt after connecting via
  telnet.

Diagnosis:

- The mini-shell banner/prompt is printed once immediately after boot.
- If telnet attaches after those bytes have already gone through `/dev/ttyAMA2`,
  the initial prompt can be lost.
- The existing idle reprompt interval was `5,000,000` UART LSR polls, which is
  too long for interactive bring-up.

Fix:

- `src/vecu_zephyr/src/main.c`
  - reduce `REALM_MINI_SHELL_REPROMPT_SPINS` from `5,000,000` to `50,000`
  - late telnet attach should now see `realm:~$ ` periodically without needing
    to reboot the Realm

Expected next signal:

- Start VM, then connect with `telnet localhost 5002`
- If the initial banner was missed, wait briefly or press Enter
- Expected prompt:
  - `realm:~$ `

Local validation:

- Rebuilt the Realm Zephyr shim bundle with `MINI_SHELL_MODE=1`.
- New `realm_zephyr_shim.bin` SHA-256:
  `e2e214661ff8d970e5bb35fe90ee0bf50ec7c74eac5eeac721a2d542f8dd35de`
- New Zephyr payload SHA-256:
  `86991e8eae6f7b30f629ca23116f8bf765409a9a5a809f09b95aa43a9b7161f5`

FVP deploy note:

- Copy the rebuilt bundle, updated lkvm, and launcher scripts.
- The key check is separation of streams:
  - `telnet localhost 5002` should show guest UART only
  - `tail -f /root/realm-vecu1.log` should show host debug/status only
- If telnet still appears blank, wait briefly or press Enter; the mini-shell
  now periodically reprints `realm:~$ ` while idle.

Date: 2026-04-28

Observation:

- User reported that `telnet localhost 5002` still showed a blank screen.

Diagnosis:

- At this point, blank telnet can mean either:
  - Zephyr is not emitting guest UART bytes anymore, or
  - Zephyr emits bytes, but the FVP `/dev/ttyAMA2` path is dropping/misdirecting
    them before the host telnet client sees them.

Fix:

- `scripts/run-vecu-zephyr.sh`
  - add `/root/realm-vecu1.uart.log`
  - pipe lkvm guest stdout through `tee` so guest UART goes to both:
    - `/dev/ttyAMA2` for `telnet localhost 5002`
    - `/root/realm-vecu1.uart.log` for deterministic local inspection
  - keep host debug/status stderr in `/root/realm-vecu1.log`

Local validation:

- `bash -n scripts/run-vecu-zephyr.sh` passed.
- Updated `run-vecu-zephyr.sh` SHA-256:
  `a05915e6e989877ea59f92ed008ca81f505ce61e5316867d88fced09f2d5204b`

Expected next signal:

- If `/root/realm-vecu1.uart.log` contains `Realm mini shell ready` or
  `realm:~$ ` while telnet is blank, the problem is the FVP tty/telnet bridge,
  not Zephyr's UART write path.
- If `/root/realm-vecu1.uart.log` is empty but `/root/realm-vecu1.log` reaches
  `phase=115(payload_mini_shell_prompt)`, the problem is inside kvmtool's
  terminal stdout path.

Date: 2026-04-28

Observation:

- Telnet finally displayed the mini-shell banner and prompt:
  - `>`
  - `Realm mini shell ready`
  - `realm:~$`

Next objective:

- Make the visible shell useful for the attestation demo path, not just boot
  proof.

Fix:

- `src/vecu_zephyr/src/main.c`
  - add mini-shell built-in command dispatch independent of Zephyr shell backend
  - support direct commands:
    - `help`
    - `ping`
    - `status`
    - `attest`
    - `realm ping`
    - `realm status`
    - `realm attest`
  - `attest` calls `payload_generate_attestation()` directly and prints token
    size and token buffer address on UART
  - status page still reports `REALM_PAYLOAD_PHASE_ATTEST_OK` or
    `REALM_PAYLOAD_PHASE_ATTEST_FAIL`

Local validation:

- Rebuilt the Realm Zephyr shim bundle with `MINI_SHELL_MODE=1`.
- New `realm_zephyr_shim.bin` SHA-256:
  `c5fc02eaa4fb19334ddd15397674c267e706e26644f7795c59835eca44f23d3d`
- New Zephyr payload SHA-256:
  `c0c7a6f6ed05e01b77778037801e7b50752dbceaed1c592707128a23c566e041`

Expected next signal:

- After uploading the new bundle, `help` should print the mini-shell command
  list immediately.
- `ping` should print `pong`.
- `status` should print handoff values.
- `attest` should print either:
  - `attest: token ready size=... buffer=...`
  - or `attest: failed rc=...`, which gives the next RSI/shared-memory bug to
    debug.

Date: 2026-04-28

Observation:

- User typed `help`, `ping`, or `status`, but no command output appeared.
- This indicates guest UART TX works, but guest UART RX is not being delivered
  into the Realm VM.

Diagnosis:

- The launcher now pipes lkvm stdout through `tee` so guest UART can be logged
  to `/root/realm-vecu1.uart.log` and forwarded to `/dev/ttyAMA2`.
- In kvmtool, `term_init()` only created the input poll thread when both stdin
  and stdout were ttys.
- With stdout connected to a pipe, the terminal input poll thread was skipped.
- Result: output works, but typed telnet characters never reach the 8250 RX
  path, so the mini-shell never receives Enter and never executes commands.

Fix:

- `dev_workspace/kvmtool-cca/term.c`
  - change `term_init()` to require only `isatty(STDIN_FILENO)` for input
    polling
  - allow stdout to be a pipe for UART logging/forwarding

Local validation:

- `./scripts/build-kvmtool-arm64.sh` completed successfully.
- New `lkvm-static` SHA-256:
  `17fb475a6ea24de9e26801eed1c2658d4ea2407d6b1712e626d74d565d78c7bf`

Expected next signal:

- Upload the new `lkvm-static`.
- Restart the Realm VM.
- In `telnet localhost 5002`, `help` should now execute and print the mini-shell
  command list.

Date: 2026-04-28

Observation:

- `telnet localhost 5002` showed RMM/TF-A SMC trace lines such as
  `SMC_RMM_REALM_CREATE`, `SMC_RSI_ABI_VERSION`, and `SMC_RSI_IPA_STATE_SET`.

Diagnosis:

- Those lines are not Zephyr mini-shell output.
- The FVP port 5002 / `/dev/ttyAMA2` path is being polluted by secure monitor /
  RMM tracing, so it is not a clean guest UART channel.

Fix:

- `scripts/run-vecu-zephyr.sh`
  - move the default Zephyr guest UART to `/dev/ttyAMA3`
  - report the user-facing connection as `telnet localhost 5003`
  - add optional overrides:
    - `--serial-tty <tty>`
    - `--telnet-port <port>`

Local validation:

- `bash -n scripts/run-vecu-zephyr.sh scripts/run-realm-shim-zephyr.sh` passed.
- Updated `run-vecu-zephyr.sh` SHA-256:
  `7536f93a4b0dd35d468f2b67b218b2d264ac004cfc2ed3fc235c99cf7e86f7db`

Expected next signal:

- Upload the updated script.
- Restart `realm-vecu1`.
- Connect to `telnet localhost 5003`, not 5002.
- 5003 should show the Zephyr mini-shell without RMM SMC traces, assuming
  another VM is not already using `/dev/ttyAMA3`.

Date: 2026-04-28

Observation:

- The mini-shell is now interactive:
  - `Realm mini shell ready`
  - `realm:~$`
- `help` works and shows the Zephyr shell command registry.
- The running bundle reports:
  - `realm`
  - `retval`
  - `shell`
- Direct commands such as `ping`, `status`, and `attest` returned
  `command not found` on the running image.

Diagnosis:

- This is no longer a Realm boot failure.
- It proves:
  - the standalone Realm shim entered the Zephyr payload
  - Zephyr reached application-level execution
  - guest UART TX and RX work through kvmtool/FVP/telnet
  - the Zephyr shell backend is alive
- The missing direct commands are a command-registration/image freshness issue.
  The current source has both `realm ...` subcommands and direct aliases, but
  the FVP guest must run the newly uploaded bundle.

Fix:

- `src/vecu_zephyr/src/main.c`
  - register direct Zephyr shell aliases:
    - `ping`
    - `status`
    - `attest`
  - keep compatibility commands:
    - `realm ping`
    - `realm status`
    - `realm attest`
  - extend the mini-shell command dispatcher so it can call the attestation
    path without relying on the full Zephyr shell backend

Attestation path update:

- `src/vecu_zephyr/src/rsi.h`
  - align RSI FIDs with the local Linux CCA guest reference
  - use RIPAS names for IPA state transitions
- `src/vecu_zephyr/src/rsi.c`
  - generate the token into a private aligned buffer
  - use `RSI_ATTESTATION_TOKEN_INIT`
  - loop on `RSI_ATTESTATION_TOKEN_CONTINUE`
  - publish a completed token size back to the caller
- `src/vecu_zephyr/src/rsi_asm.S`
  - pass all eight challenge words using the custom RSI init SMC wrapper
- `src/vecu_zephyr/src/payload_mmu.c`
  - map the fixed shared token/control pages through the Realm shared alias
- `src/vecu_zephyr/src/main.c`
  - measure token generation with the virtual counter
  - copy the completed token into the fixed shared publish buffer
  - update the shared control page with token readiness, size, generation
    count, challenge, and generation time

Local validation:

- `MINI_SHELL_MODE=1 ./scripts/build-realm-zephyr-shim-bundle.sh` completed.
- Cleaned the `rsi_version_check()` printk format warnings and rebuilt the
  mini-shell bundle again.
- `src/realm_shim/build/realm_zephyr_shim.bin` SHA-256:
  `edf165c4b6ae78718f8eb83e6a8944b113c037a56bf980860580e3f64bc72df5`
- `src/vecu_zephyr/build/zephyr/zephyr.bin` SHA-256:
  `826474e417e1ccec8c095080e5dc1f7bc071db25cebc8a9188155f3a934ea00a`
- `dev_workspace/kvmtool-cca/lkvm-static` SHA-256:
  `17fb475a6ea24de9e26801eed1c2658d4ea2407d6b1712e626d74d565d78c7bf`
- `scripts/run-vecu-zephyr.sh` SHA-256:
  `7536f93a4b0dd35d468f2b67b218b2d264ac004cfc2ed3fc235c99cf7e86f7db`

Expected next signal:

- On the old uploaded bundle, use:
  - `realm ping`
  - `realm status`
  - `realm attest`
- After uploading the new bundle, either form should work:
  - `ping`
  - `status`
  - `attest`
- `attest` should print either a token-ready result with size, cycles, and
  nanoseconds, or a concrete RSI/publish error code for the next debugging
  step.

AGL requirement:

- Zephyr-only is enough to prove Realm payload boot and token generation.
- AGL should be launched in parallel for the paper's end-to-end V-ECU
  communication experiment, because AGL is the Normal World consumer/verifier
  of the shared token/control buffer.

Date: 2026-04-28

Milestone: Zephyr Realm mini-shell attestation command succeeded

Runtime observation:

- The updated mini-shell bundle booted and accepted commands:
  - `help`
  - `ping`
  - `status`
  - `attest`
- `ping` returned:
  - `pong`
- `status` returned:
  - `status_shared=0x0000000183e10000`
  - `alias_bit=0x0000000100000000`
  - `dtb=0x0000000083e00000`
- `attest` completed successfully after RSI token init/continue calls.

Attestation result:

- Token buffer IPA used for RSI:
  - `0x8020c000`
- Published shared token buffer:
  - `0x0000000183ffe000`
- Token size:
  - `0x4c2` bytes
- Measured cycles:
  - `0x04507464`
- Measured time:
  - `0x2b248be8` ns
- The command printed:
  - `attest: token ready size=0x00000000000004c2 buffer=0x0000000183ffe000 cycles=0x0000000004507464 ns=0x000000002b248be8`

Conclusion:

- Phase 1 bootable Realm Zephyr is complete for the shim-first path.
- Phase 2 fixed shared publish path is minimally working for token output.
- Phase 3 first token-generation measurement is working inside Zephyr Realm.

Next objective:

- Add a Normal World reader/verifier path so AGL or the host-side agent can
  consume the token/control buffer and produce end-to-end V-ECU communication
  measurements.

Date: 2026-04-28

Implementation: Normal World token reader/dumper

Goal:

- Make the Zephyr Realm `attest` result observable from Normal World without
  manually parsing telnet output.
- This is the first host-side consumer path before wiring the token into AGL or
  a full verifier service.

Changes:

- `dev_workspace/kvmtool-cca/builtin-run.c`
  - add a Realm token watcher thread controlled by `LKVM_REALM_TOKEN_WATCH`
  - watch the shared control page at `0x83fff000`
  - read the shared token page at `0x83ffe000`
  - detect new tokens via `COMM_MAGIC`, `token_ready`, and `gen_count`
  - dump token bytes to `LKVM_REALM_TOKEN_OUT`
  - dump metadata to `LKVM_REALM_TOKEN_META`
- `scripts/run-vecu-zephyr.sh`
  - when `--debug` is used, enable the token watcher automatically
  - default token dump path:
    - `/root/realm-vecu1.token.bin`
  - default metadata path:
    - `/root/realm-vecu1.token.meta`
- `scripts/read-realm-token.sh`
  - inspect the dumped token and metadata inside FVP Linux
  - check metadata token size against the actual file size
  - print SHA-256 and the first 64 token bytes when tools are available

Local validation:

- `bash -n scripts/run-vecu-zephyr.sh scripts/run-realm-shim-zephyr.sh` passed.
- `sh -n scripts/read-realm-token.sh` passed.
- `./scripts/build-kvmtool-arm64.sh` completed and rebuilt `lkvm-static`.

Expected runtime signal:

- Start the Realm VM with `--debug`.
- Run `attest` in the Zephyr Realm shell.
- `/root/realm-vecu1.log` should contain:
  - `realm token watch ...`
  - `realm token published gen=... size=... gen_time_ns=...`
- FVP Linux should contain:
  - `/root/realm-vecu1.token.bin`
  - `/root/realm-vecu1.token.meta`
- `/root/read-realm-token.sh` should print token metadata, actual file size,
  SHA-256, and a small hexdump.

Limit:

- This is reader/metadata validation, not full cryptographic CCA token
  verification yet.
- Full verifier integration should be the next step, using AGL or a Normal
  World verifier service consuming the dumped token.

Date: 2026-04-28

Implementation: separate clean mini-shell runs from debug/status tracing

Goal:

- Keep the interactive Zephyr Realm mini-shell visible on a chosen FVP telnet
  port.
- Keep kvmtool debug/status logs out of the mini-shell path for paper-facing
  runs.
- Continue dumping the attestation token to Normal World without requiring
  `--debug`.

Changes:

- `scripts/run-vecu-zephyr.sh`
  - add `--shell-port <5000..5004>` convenience option
  - default the mini-shell stream to `5002` (`/dev/ttyAMA2`)
  - keep `5003` as the current clean fallback when TF-A/RMM traces pollute
    `5002`
  - enable the Normal World token watcher by default
  - add `--no-token-watch` for runs that should not dump token files
  - keep kvmtool status/phase tracing tied to `--debug`
- `scripts/run-realm-shim-zephyr.sh`
  - forward option values correctly for `--shell-port`, `--serial-tty`,
    `--telnet-port`, and `--entry-offset`
- `dev_workspace/kvmtool-cca/builtin-run.c`
  - allow the token watcher to run without `--debug`

Runtime policy:

- Use this for clean shell plus token dump:
  - `/root/run-realm-shim-zephyr.sh --shell-port 5002 /root/realm-zephyr-shim.bin`
- If `5002` shows TF-A/RMM `SMC_RSI` or `SMC_RMM` trace lines, move the shell:
  - `/root/run-realm-shim-zephyr.sh --shell-port 5003 /root/realm-zephyr-shim.bin`
- Use `--debug` only when investigating boot phases or kvmtool internals.

Note:

- The `SMC_RSI` / `SMC_RMM` lines are emitted by TF-RMM/TF-A logging, not by
  the mini-shell command layer.
- `--shell-port 5004` is accepted by the launcher, but the current FVP boot
  script enables only UART0..UART3. A real `5004` path requires the model to
  expose `/dev/ttyAMA4`, or a separate TCP/PTY bridge to be added later.

Date: 2026-04-28

Implementation: Realm mini-shell command to trigger Normal World

Goal:

- Make a command typed in the Zephyr Realm mini-shell cause visible Normal
  World-side behavior.
- Keep this path independent from TF-RMM debug traces and separate from token
  dump parsing.

Changes:

- `src/vecu_zephyr/src/rsi.h`
  - extended the shared control page with `host_req`, `host_ack`, `host_gen`,
    `host_status`, `host_arg0`, `host_arg1`, and `host_msg`
- `src/vecu_zephyr/src/main.c`
  - added the `normal [msg]` mini-shell command
  - added `realm normal [msg]` and top-level `normal` Zephyr shell aliases
  - send a shared mailbox request and wait for the host ack
- `dev_workspace/kvmtool-cca/builtin-run.c`
  - extended the Normal World watcher to detect `COMM_HOST_REQ_NORMAL`
  - print `realm normal-world request ... msg="..."` when a request arrives
  - write `host_status=0` and echo `host_ack=host_gen`

Expected runtime:

- Realm shell:
  - `normal hello-normal`
  - `normal: ack gen=0x0000000000000001 status=0x0000000000000000`
- Normal World log:
  - `Info: realm normal-world request gen=1 req=1 arg0=0x100000000 arg1=0x83e00000 msg="hello-normal"`

Validation:

- `MINI_SHELL_MODE=1 ./scripts/build-realm-zephyr-shim-bundle.sh` completed.
- `./scripts/build-kvmtool-arm64.sh` completed.

Limit:

- This proves an interactive Realm-to-Normal-World mailbox path through
  kvmtool. It is not yet the final AGL verifier service; that can now be built
  on top of the same control-page request/ack structure.

Date: 2026-04-28

Implementation: paper-facing `normal attest [N]` measurement loop

Goal:

- Convert the working attestation demo into a repeatable measurement command
  that matches the planned paper experiment boundary.
- Measure token generation, shared publish, Normal World notification/ack, and
  total end-to-end latency from a single Realm shell command.

Changes:

- `src/vecu_zephyr/src/main.c`
  - added decimal printing and simple summary-stat helpers for the mini-shell
  - added `normal attest [N]` and `realm normal attest [N]`
  - run optional warmup iterations before measured samples
  - print CSV rows:
    - `iter`
    - `token_size`
    - `gen_cycles`, `gen_ns`
    - `publish_cycles`, `publish_ns`
    - `normal_cycles`, `normal_ns`
    - `total_cycles`, `total_ns`
    - `status`
  - print summary rows for `gen_ns`, `publish_ns`, `normal_ack_ns`, and
    `total_ns`
- `src/vecu_zephyr/src/rsi.h`
  - added explicit Normal World status values
- `dev_workspace/kvmtool-cca/builtin-run.c`
  - recognize `attest` mailbox requests
  - validate that a token is ready before returning `COMM_HOST_STATUS_OK`
  - log token generation count, token size, and Realm-measured generation time
  - reduce watcher polling interval to improve ack-latency measurement

Runtime command:

- `normal attest 50`

Interpretation:

- `gen_ns` is the Realm-internal RSI token generation time.
- `publish_ns` is the shared token/control-page publish time.
- `normal_ack_ns` is the Realm-to-Normal watcher notification and ack path.
- `total_ns` is the implemented end-to-end path:
  Realm token generation + publish + Normal World token-ready acknowledgement.

Limit:

- The Normal World consumer is still the kvmtool watcher. This is sufficient for
  the first reproducible Realm-to-Normal measurement, but the final paper path
  should replace or supplement this watcher with an AGL verifier service.

Date: 2026-04-28

Implementation: AGL-visible attestation demo packaging

Goal:

- Run AGL on FVP telnet `5001` and the Zephyr Realm mini-shell on telnet
  `5002` in one reproducible command.
- Keep the existing `normal attest [N]` CSV measurement path.
- Make each Normal World attestation acknowledgement visible from the AGL-side
  console/log while the true AGL userspace verifier remains future work.

Changes:

- `dev_workspace/kvmtool-cca/builtin-run.c`
  - added optional `LKVM_REALM_AGL_EVENT_TTY` and
    `LKVM_REALM_AGL_EVENT_LOG` outputs
  - emit `[AGL verifier bridge] realm_attest ...` whenever the Normal World
    watcher consumes an attestation token and returns an ack
- `scripts/run-vecu-agl.sh`
  - added `--serial-tty` and `--telnet-port`
  - default remains AGL on `/dev/ttyAMA1`, host telnet `5001`
- `scripts/run-agl-realm-attest-demo.sh`
  - starts only the named VMs: `agl-normal` and `realm-vecu1`
  - wires the Realm watcher event bridge to the AGL console tty
  - prints the exact host telnet commands and the Realm measurement command
- `scripts/upload-agl-realm-attest-demo.sh`
  - uploads `lkvm-static`, the Realm shim bundle, and all demo scripts into
    FVP Linux
- `docs/agl-realm-attestation-demo.md`
  - documents the full host upload, FVP run, telnet, measurement, and cleanup
    flow

Runtime commands:

- Host:
  - `./scripts/upload-agl-realm-attest-demo.sh root@192.168.122.33`
- FVP Linux:
  - `/root/run-agl-realm-attest-demo.sh --iterations 50`
- Host telnet terminals:
  - `telnet localhost 5001`
  - `telnet localhost 5002`
- Realm mini-shell:
  - `normal attest 50`

Current limit:

- The AGL side currently observes the bridge event on its console/log. It does
  not yet run a native AGL verifier process that parses and cryptographically
  verifies the CCA token.

Date: 2026-04-28

Implementation: final AGL userspace verifier path

Goal:

- Remove the bridge-only experiment boundary and make the paper-facing path use
  a real AGL userspace process.
- Keep the Realm command interface stable: `normal attest [N]`.

Changes:

- `src/agl_attest_verifier/agl_attest_verifier.c`
  - added an AGL-side verifier daemon/client
  - connects from AGL to the kvmtool verifier port, receives token bytes,
    validates definite-length CBOR structure, computes SHA-256, measures
    AGL-side processing time, and returns an ack/result record
- `dev_workspace/kvmtool-cca/builtin-run.c`
  - added `LKVM_REALM_AGL_VERIFY=1` TCP verifier transport
  - waits for an AGL verifier connection and forwards each Realm token to AGL
  - returns AGL timing values to the Realm control page in `host_arg0` and
    `host_arg1`
  - removed the AGL-visible bridge-event emission from the paper-facing path
- `src/vecu_zephyr/src/main.c`
  - extended `normal attest [N]` CSV with `agl_verify_ns` and `agl_hash_ns`
  - prints `summary,agl_verify_ns,...`
- `scripts/build-agl-attest-verifier.sh`
  - builds the static ARM64 AGL verifier binary
- `scripts/install-agl-attest-verifier-rootfs.sh`
  - installs the verifier into the AGL rootfs and creates systemd/SysV startup
    entries
- `scripts/run-agl-realm-attest-demo.sh`
  - installs the verifier, starts AGL, starts the Realm, waits for the Realm
    shell, then waits for `realm AGL verifier connected`

Runtime commands:

- Host:
  - `./scripts/build-agl-attest-verifier.sh`
  - `./scripts/build-kvmtool-arm64.sh`
  - `MINI_SHELL_MODE=1 ./scripts/build-realm-zephyr-shim-bundle.sh`
  - `./scripts/upload-agl-realm-attest-demo.sh root@192.168.122.33`
- FVP Linux:
  - `/root/run-agl-realm-attest-demo.sh --iterations 50`
- Host telnet terminals:
  - `telnet localhost 5001`
  - `telnet localhost 5002`
- Realm mini-shell:
  - `normal attest 50`

Expected evidence:

- `/root/realm-vecu1.log`
  - `realm AGL verifier connected on port 7777`
  - `realm AGL verifier ack gen=... token_size=... parse_ns=... hash_ns=... total_ns=...`
- AGL console or `/root/agl-normal.uart.log`
  - `agl_csv,gen=...,token_size=1218,...,status=0x00000000,sha256=...`
- Realm shell CSV:
  - includes `agl_verify_ns` and `agl_hash_ns`

Current verifier scope:

- The AGL verifier performs deterministic token consumption suitable for the
  current performance experiment: token size checks, CBOR structure validation,
  SHA-256 digest, and timing.
- Full cryptographic CCA COSE signature and certificate-chain verification
  still requires platform attestation key/certificate material and should be
  documented as the next security-completeness step.
