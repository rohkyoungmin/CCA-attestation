# Realm Shim Design

This document defines the standalone Realm shim that sits between the Realm boot entry and a guest payload such as Zephyr.

## Primary Design Principle

The target architecture is:

- **do not modify Zephyr kernel code**
- keep Realm-specific boot translation outside the guest OS

That means the standalone shim is not an optional optimization. It is the preferred long-term architecture.

## Goal

Use a small, reusable binary as a translation layer:

- input side:
  - `kvmtool`
  - RMM
  - Realm entry contract
  - raw RSI ABI
- output side:
  - guest payload contract
  - status/debug publication
  - later, shared/private memory helpers

This keeps Realm-specific logic out of the guest OS as much as possible.

## Why A Standalone Shim

Compared with embedding all Realm logic directly into Zephyr:

- better reproducibility
- reusable across different guest payloads
- cleaner separation of responsibilities
- easier comparison between:
  - Zephyr payload
  - Linux payload
  - future RTOS payloads

This also preserves a stronger research claim:

- the guest OS remains substantially unchanged
- the Realm adaptation logic is explicit, inspectable, and reusable

## Intended Boot Flow

1. `kvmtool` enters the Realm guest at shim entry
2. shim reads Realm configuration
3. shim opens a small shared status page
4. shim publishes early boot phases
5. shim transfers control to the payload entry point
6. payload uses the shim contract instead of raw Realm rules where possible

Target shape:

- `lkvm -> realm_shim.bin -> payload.bin`

## Fixed Bundle Layout

The first concrete implementation uses a single bundled Linux `Image`-style
binary:

- `0x80000000`: standalone shim entry
- `0x80200000`: Zephyr payload image

That means:

- the shim owns the first `2 MiB`
- the Zephyr payload is linked to `0x80200000`
- `kvmtool` still loads a single binary at `0x80000000`

The bundle build step pads the image so the payload lands at the fixed
contract address.

## Responsibilities

### Shim must do

- provide a Linux `Image`-compatible entry header
- preserve the boot-time DTB pointer in `x0`
- detect Realm support with `RSI_VERSION`
- read `RSI_REALM_CONFIG`
- compute the shared alias bit
- mark one fixed page as shared for status/debug
- publish visible phase markers before the payload starts
- jump to the payload entry address

### Shim must not do yet

- full dynamic payload relocation
- general page allocator integration
- full attestation path
- device driver policy for the payload

### Payload should not need from the kernel

The intended payload model is:

- Zephyr kernel source stays unmodified
- any required payload cooperation should be limited to:
  - payload placement
  - entry contract
  - optional small app-side integration

If a requirement would force deep Zephyr kernel changes, that is evidence that the shim contract is still incomplete.

## Shared Status Page

Current fixed address:

- IPA base: `0x83e10000`

Current expected contents:

- magic
- phase
- optional values
- short text

This page is used only for early debug until the full shared/private buffer model is in place.

## Initial Payload Contract

The first contract is intentionally simple.

The shim guarantees:

- `x0` still points to the DTB
- Realm config was read successfully
- one shared status page is available
- a payload can assume the shim already validated the Realm ABI
- the payload image starts at `0x80200000`
- the payload entry point is payload-owned and may differ from the image base

Current Zephyr payload contract:

- shim jumps to `realm_payload_entry`
- `realm_payload_entry` publishes:
  - `0x19`: `payload_stub_entry`
  - `0x1a`: `payload_before_reset_prep`
  - `0x1b`: `payload_after_reset_prep`
  - `0x1c`: `payload_after_highest_init`
  - `0x1d`: `payload_after_el1_init`
  - `0x1e`: `payload_before_z_prep_c`
  - `0x1f`: `payload_unexpected_el`
- the payload stub then branches into Zephyr `z_prep_c`

The payload still owns:

- normal guest initialization
- later runtime attestation logic
- later page-sharing requests

## Planned Implementation Phases

### Phase 1: Shim skeleton

- separate source tree under `src/realm_shim/`
- standalone build script
- Linux `Image`-compatible entry
- status page phase writes
- no payload jump yet, or jump to a fixed contract address

### Phase 2: Fixed payload contract

- define a fixed payload load address
- define a fixed payload entry address
- document how the payload image is placed there

### Phase 3: Adapter handoff

- move more Realm-specific work from Zephyr into the shim
- keep Zephyr using only a thin payload-facing contract for what truly must remain inside the OS

## Relationship To The Current Realm Adapter Layer

The current in-Zephyr `Realm Adapter Layer` remains useful as an experiment and as a way to discover what the shim must eventually absorb.

Recommended long-term split:

- standalone shim:
  - boot-time translation
  - early status publication
  - basic Realm config handoff
- optional thin payload-facing runtime helper:
  - only if a small OS-local helper is unavoidable
  - should remain outside the Zephyr kernel if possible

That gives us:

- reusable boot translation outside the OS
- minimal payload assumptions
