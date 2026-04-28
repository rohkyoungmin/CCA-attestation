# Zephyr Realm Porting TODO

This document tracks the remaining work needed to make Zephyr a bootable Arm CCA Realm guest on the current FVP + TF-RMM + `kvmtool-cca` stack.

## Current Status

- Realm Linux is a known-good baseline on this stack.
- Zephyr Realm now boots through the standalone shim and reaches an interactive
  mini-shell prompt.
- The guest UART path works in both directions through kvmtool/FVP/telnet.
- `help`, `ping`, `status`, and `attest` now execute from the Realm mini-shell.
- The first RSI attestation command succeeded and published a token to the
  shared alias buffer.
- kvmtool now has a Normal World token watcher that can dump the shared token
  and metadata after `attest` completes. This watcher is enabled by default and
  no longer requires `--debug`.
- The Realm mini-shell now has a `normal [msg]` command that sends a mailbox
  request to the Normal World kvmtool watcher and waits for an ack. This is the
  first interactive Realm-to-Normal-World command path.
- `normal attest [N]` now performs the paper-facing measurement loop:
  RSI token generation, shared-buffer publish, AGL userspace verifier ack,
  per-iteration CSV output, and summary statistics.
- AGL now runs `agl_attest_verifier`, which receives the token, validates
  definite-length CBOR structure, computes SHA-256, measures AGL-side verifier
  time, and returns an acknowledgement to the Realm path.
- The active remaining work is no longer "make Zephyr reach `main()`"; it is
  now to add full CCA COSE/certificate-chain verification once platform
  attestation key material is available.
- The preferred target architecture is now a standalone Realm shim with no Zephyr kernel modification requirement.
- The current active implementation path is:
  - standalone shim at `0x80000000`
  - Zephyr payload at `0x80200000`
  - single bundled Realm image
  - selectable mini-shell UART via `--shell-port`
  - default mini-shell UART on `/dev/ttyAMA2`, normally `telnet localhost 5002`
  - fallback clean mini-shell UART on `/dev/ttyAMA3`, normally `telnet localhost 5003`

## Scope

The bootable Realm-aware Zephyr substrate is now implemented. The current scope
is to preserve this path as a reproducible paper experiment and harden the
cryptographic verifier.

The experimental in-Zephyr adapter path remains useful for reference, but it is
no longer the preferred target.

Success for the initial port now means:

- Zephyr Realm reaches `main()`.
- Zephyr Realm can emit a reliable boot status signal.
- At least one debug path is stable:
  - guest console output on `5002` or `5003`, or
  - a shared status page visible to the host.
- `ping`, `status`, and `attest` commands execute from the Realm mini-shell.
- `normal [msg]` can trigger a visible Normal World-side handler and ack path.
- `normal attest [N]` can produce reproducible token-generation,
  Realm-to-AGL verifier ack, AGL verifier, and total timing data.

## Reference Baseline

Use the Linux CCA guest as the reference implementation.

For the target architecture, also use:

- `docs/realm-shim-design.md`

Relevant local files:

- `src/linux/arch/arm64/kernel/rsi.c`
- `src/linux/arch/arm64/kernel/setup.c`
- `src/linux/arch/arm64/mm/init.c`
- `src/linux/arch/arm64/mm/mmu.c`
- `src/linux/arch/arm64/mm/pageattr.c`

The Linux baseline proves that:

- Realm entry works on the current stack.
- RSI calls work on the current stack.
- RIPAS setup can succeed on the current stack.
- The remaining gap is Zephyr-specific.

## Phase 0: Standalone Shim First

Goal: move Realm boot translation outside Zephyr.

Required:

- standalone shim source tree
- standalone shim build script
- bundled shim plus payload image build
- documented payload contract
- fixed shared status page
- shim phases visible before payload entry
- Zephyr payload linked above the shim region without Zephyr kernel changes

Done when:

- the Realm-specific boot contract is no longer tied to Zephyr kernel internals
- the test image path is `lkvm -> shim bundle -> Zephyr payload`

## Phase 1: Bootable Realm Zephyr Payload

Goal: reach `main()` and expose a reliable boot signal.

The first implementation target is:

- external shim
- minimal payload contract
- Zephyr payload above that contract
- no Zephyr kernel modification requirement

### 1. Early RSI initialization

Implement the minimum Linux-like early Realm initialization in the shim.

Required:

- Call `RSI_VERSION` very early.
- Call `RSI_REALM_CONFIG`.
- Record `ipa_bits`.
- Compute the shared alias bit from `ipa_bits`.
- Store this state in Zephyr arch globals so later MMU and driver code can use it.

Candidate shim files:

- `src/realm_shim/start.S`
- `src/realm_shim/contract.h`

Done when:

- the shim can detect Realm configuration before handing off to the payload
- the payload can remain a normal Zephyr app and board build

### 2. MMU and device mapping alignment

This is the most important part of the payload bring-up.

In a Realm, accesses to unprotected or shared memory need the shared alias bit in the physical address used by the PTE.

Target principle:

- this translation should live in the shim if possible
- payload-specific kernel modifications should be avoided
- Zephyr kernel source should remain unchanged

Required:

- Make device mappings Realm-aware.
- Ensure UART or other console MMIO mappings use the shared alias bit.
- Ensure any fixed mappings used before `main()` do not use a plain private physical address for shared devices.

Candidate payload-side files:

- board DTS files under `src/vecu_zephyr/boards/arm64/`
- optional app-level helper code only if unavoidable

Done when:

- Early console MMIO does not immediately fault because of a private mapping.

### 3. Out-of-band status page

Do not depend on UART alone for bring-up.

Required:

- Reserve one fixed page as a shared status page.
- Make the shim write boot phase markers into that page.
- Optionally let the payload app write later phase markers without requiring kernel changes.
- Read the page from the host side for debugging.

Suggested boot markers:

- `0x10`: shim entry reached
- `0x11`: shim RSI init complete
- `0x12`: payload jump
- `0x20`: payload `main()` reached
- `0x21`: attestation path entered

Suggested locations:

- shim side:
  - `src/realm_shim/`
- payload app side:
  - `src/vecu_zephyr/src/`
- host side:
  - either a minimal `kvmtool` watcher or a shared buffer inspection helper

Done when:

- We can tell whether the shim handed off to the payload.
- We can tell whether Zephyr reached `main()` even if UART output is absent.

### 4. Defer aggressive RIPAS changes

Do not protect all RAM too early in the port.

Required:

- Keep the Linux-like ordering in mind.
- Avoid full RAM `RIPAS_RAM` conversion in the earliest reset path until console and status page are stable.

Done when:

- Zephyr can boot to `main()` without early memory-state churn masking the real failure.

## Phase 2: Shared and Private Memory API

Goal: add the minimum Zephyr subsystem needed to share buffers safely with the host.

### 5. Implement `realm_share_page()`

This is the Zephyr equivalent of the Linux CCA guest sharing path.

Required behavior:

1. call `RSI_IPA_STATE_SET` to move the page to an unprotected or shared state
2. update the Zephyr PTE to use the shared alias bit
3. flush TLB and cache so the new mapping is visible consistently

Candidate files:

- `src/vecu_zephyr/src/realm_adapter.c`
- `src/vecu_zephyr/src/rsi.c`
- `dev_workspace/zephyr/zephyr/arch/arm64/core/mmu.c`
- possibly a new helper file under `src/vecu_zephyr/src/`

Done when:

- A fixed page can be shared after boot without corrupting the guest.

### 6. Implement `realm_unshare_page()`

Required behavior:

1. stop host-side sharing
2. restore the private mapping
3. restore the private Realm page state
4. flush TLB and cache again

This can come after `realm_share_page()` if needed, but it should be part of the design from the beginning.

### 7. Start with static shared buffers

Do not start with heap-backed dynamic transitions.

Required:

- Allocate fixed shared buffers in a dedicated section.
- Convert those buffers once after boot.

Suggested buffers:

- status page
- attestation token buffer
- host command or response page

Done when:

- Token publishing and debugging can use fixed pages without allocator complexity.

## Phase 3: Attestation Integration

Goal: produce measurable attestation output from Zephyr Realm.

### 8. Token generation

Required:

- Request a token from the Realm using the RSI attestation interface.
- Keep the working token memory private while the token is being generated.

Candidate files:

- `src/vecu_zephyr/src/rsi.c`
- `src/vecu_zephyr/src/main.c`

### 9. Token publish

Required:

- Copy the completed token into a shared buffer.
- Signal completion through either:
  - the shared status page, or
  - a host-visible completion flag.

Done when:

- The host can read a real token generated by Zephyr Realm.

Current status:

- Minimal host readout is implemented through the kvmtool token watcher.
- AGL-side userspace token consumption is implemented through
  `agl_attest_verifier`.
- Next step is full cryptographic CCA COSE/certificate-chain verification.

### 10. Latency measurement

Required:

- Read `CNTPCT_EL0` or the Zephyr equivalent before and after the RSI token request path.
- Record:
  - token generation latency
  - publish latency
  - total end-to-end time

Done when:

- The paper can report pure token-generation cost and full publication cost separately.

## Optional Follow-up Work

These are useful, but should not block the first bootable Realm Zephyr milestone.

### 11. Generalize shared memory transitions

- page range sharing instead of single-page helpers
- better abstractions for shared sections
- integration with a future Zephyr Realm subsystem

### 12. Better signaling than polling

- host interrupt or doorbell
- virtual GIC notification
- explicit completion event instead of polling a status byte

### 13. Broader driver support

- virtio paths
- console variants
- shared I/O abstractions beyond a single UART path

## Recommended Implementation Order

Use this order to avoid getting stuck on the wrong layer.

1. keep Linux Realm as the bring-up baseline
2. stabilize Zephyr early RSI initialization
3. stabilize Realm-aware MMU and device mappings
4. add the shared status page
5. confirm `main()` entry using the status page
6. add fixed shared token buffer support
7. generate and publish a token
8. move any remaining app-visible Realm logic behind the adapter layer
8. add latency measurement
9. only then generalize to reusable page-sharing APIs

## Not The First Priority

These are intentionally out of scope for the first successful port:

- dynamic heap page-state transitions
- full generic Realm memory manager
- interrupt-driven completion signaling
- polishing the final attestation application protocol

## Exit Criteria

The Zephyr Realm port is usable for the paper baseline when all of the following are true:

- Zephyr Realm reaches `main()`
- the host can observe progress through console or shared status page
- a fixed shared buffer can be published safely
- Zephyr Realm can request an attestation token
- the host can read the token
- the token path latency can be measured reproducibly
