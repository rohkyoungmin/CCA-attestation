# Zephyr Realm Adapter Layer Design

This document defines the translation layer between Arm CCA Realm execution rules and the Zephyr RTOS runtime model.

This document now describes an **intermediate experimental path**, not the preferred end-state architecture.

The preferred end state is:

- standalone Realm shim outside the guest OS
- no Zephyr kernel code modifications required for boot translation

The goal is to avoid spreading Realm-specific details across all Zephyr application code.

## Why This Layer Exists

Linux CCA guests already implement a Realm-aware guest contract:

- early `RSI_VERSION`
- early `RSI_REALM_CONFIG`
- Realm-aware memory setup
- shared or private page transitions
- attestation token requests

Zephyr does not provide those pieces out of the box.

That means the Zephyr-on-Realm problem is not only an application issue. It is a boot/runtime adaptation problem. The in-Zephyr adapter was introduced to discover the missing contracts quickly, but the target design is to push as much of that contract as possible into a separate shim.

## Design Goal

Split the system into four levels:

1. `kvmtool` + `RMM` + Realm entry
2. Zephyr ARM64 boot code
3. Realm Adapter Layer
4. Zephyr V-ECU application

In this experimental layout, the adapter layer is the translator between level 2 and level 4.

## Layer Breakdown

### Layer 0: Raw RSI ABI

Files:

- `src/vecu_zephyr/src/rsi.h`
- `src/vecu_zephyr/src/rsi.c`
- `src/vecu_zephyr/src/rsi_asm.S`

Responsibilities:

- define RSI function IDs
- issue raw `SMC` calls
- expose low-level helpers:
  - version query
  - realm config query
  - IPA state changes
  - attestation token requests
  - status page mapping primitive

This layer should stay close to the Realm ABI.

### Layer 1: Realm Adapter Layer

Files:

- `src/vecu_zephyr/src/realm_adapter.h`
- `src/vecu_zephyr/src/realm_adapter.c`

Responsibilities:

- keep a Zephyr-facing Realm runtime state:
  - ABI version
  - `ipa_bits`
  - shared alias bit
  - status page pointer
  - ready flag
- provide Zephyr-friendly APIs:
  - `realm_adapter_init()`
  - `realm_adapter_publish_status()`
  - `realm_adapter_heartbeat()`
  - `realm_adapter_prepare_comm_ctrl()`
  - `realm_adapter_attest_once()`
  - `realm_adapter_publish_shared_buffers()`
- implement the boot status hook used by early Zephyr boot code

This is the current experimental translation layer.

### Layer 1a: Boot Shim Hooks

Files currently involved:

- `dev_workspace/zephyr/zephyr/arch/arm64/core/reset.c`
- `dev_workspace/zephyr/zephyr/arch/arm64/core/prep_c.c`
- `dev_workspace/zephyr/zephyr/arch/arm64/core/boot.h`

Responsibilities:

- perform minimal Realm-aware detection before normal app logic
- publish ultra-early boot state
- keep the system debuggable even before `main()`

This is not yet a separate binary loader. Right now it is a thin shim embedded into Zephyr early boot code, and that is explicitly not the final target architecture.

### Layer 2: Zephyr V-ECU App

File:

- `src/vecu_zephyr/src/main.c`

Responsibilities:

- define the V-ECU workflow
- request attestation through the adapter
- publish results through the adapter

The app should not need to know raw RSI details unless absolutely necessary.

## Current Contract Between Layers

### Boot Code -> Adapter

Boot code can publish progress with:

```c
z_arm64_realm_boot_status_hook(phase, value0, value1);
```

This lets early ARM64 code report:

- `mmu_ready`
- `pre_cstart`

without depending on normal Zephyr app startup.

### Adapter -> App

The app can use:

- `realm_adapter_init()`
- `realm_adapter_state()`
- `realm_adapter_attest_once()`
- `realm_adapter_publish_shared_buffers()`

This keeps the app mostly independent from raw RSI calls.

## Phased Implementation Plan

### Phase A: Adapter Skeleton

Goal:

- move current ad hoc Realm app logic behind adapter APIs

Done when:

- app no longer directly uses most raw RSI helpers for normal control flow

### Phase B: Early Boot Visibility

Goal:

- make boot progress visible before `main()`

Required:

- keep status page working
- keep `reset.c` and `prep_c.c` probes minimal
- confirm whether the guest reaches:
  - EL1 RSI config
  - MMU init
  - `z_cstart()`
  - `main()`

### Phase C: Shared or Private Buffer API

Goal:

- turn the current fixed-page sharing into a reusable adapter mechanism

Required:

- single-page share helper
- optional unshare helper
- fixed token and control buffers

### Phase D: Attestation Path

Goal:

- generate and publish a token through the adapter

Required:

- token generation timing
- token publish timing
- host-visible completion signal

## What This Layer Does Not Solve By Itself

The adapter layer is necessary, but not sufficient, for a complete Realm Zephyr port.

Still outside this layer:

- full Realm-safe device mapping policy in Zephyr MMU internals
- long-term share or unshare lifecycle management
- a separate external Realm loader binary
- host-side protocol and benchmark orchestration

## Why This Experimental Architecture Helps

- it makes the Zephyr port easier to reason about
- it reduces direct coupling between app code and raw RSI ABI details
- it gives a reusable shape for future RTOS support beyond Zephyr
- it fits the research narrative better:
  - CCA-specific mechanisms live in an explicit adaptation layer
  - V-ECU logic stays above that layer

## Target Architecture Note

The preferred architecture going forward is described in:

- `docs/realm-shim-design.md`

That design aims to:

- keep Zephyr kernel code unmodified
- move boot-time translation to a standalone shim binary
- reuse the same shim contract across multiple guest payloads
