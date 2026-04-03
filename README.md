# ARM CCA V-ECU Platform

The codebase is centered on Arm FVP and a four-world stack:

- Normal World: Linux host, plus KVM guests such as AGL
- Realm World: TF-RMM with Zephyr Realm VM
- Secure World: Hafnium and OP-TEE
- Root World: TF-A

The project has been tested on Ubuntu 22.04 on both `x86_64` and `aarch64`, with at least 16 GB RAM recommended.

## Repository Layout

- [`scripts/`](./scripts): environment setup, build, run, and benchmark entrypoints
- [`docs/`](./docs): environment notes, client usage, VM and measurement guides
- [`src/sc_client/`](./src/sc_client): local SCRUTINIZER client and kernel driver
- [`src/vecu_zephyr/`](./src/vecu_zephyr): Zephyr Realm V-ECU application
- [`src/vecu_attest/`](./src/vecu_attest): attestation token generation and verification microbenchmarks
- [`src/vecu_comm/`](./src/vecu_comm): inter-V-ECU communication benchmark

## Quick Start

### 1. Fetch external dependencies

This downloads and initializes the external workspaces under `dev_workspace`, including toolchains, FVP, Hafnium, TF-RMM, OpenCSD, Zephyr, AGL, `kvmtool-cca`, and `libfdt-src`.

```bash
./scripts/env.sh all
```

You can still fetch individual components if needed:

```bash
./scripts/env.sh zephyr
./scripts/env.sh agl
./scripts/env.sh kvmtool
```

### 2. Build all components

This runs the project build entrypoint and includes the main firmware, monitor-side components, OP-TEE userspace pieces, Zephyr V-ECU targets, and AGL image build.

```bash
./scripts/build.sh all
```

You can also build a specific target:

```bash
./scripts/build.sh tf-a
./scripts/build.sh vecu-zephyr
./scripts/build.sh agl
```

### 3. Launch FVP

```bash
./scripts/bootfvp.sh
```

The FVP Linux login is:

- user: `root`
- password: `root`

## Running the Platform

For the detailed host/FVP/VM workflow, see [`docs/env-setup.md`](./docs/env-setup.md).

Typical flow:

1. Boot FVP with [`bootfvp.sh`](./scripts/bootfvp.sh).
2. Connect to FVP Linux over `telnet localhost 5000`.
3. Upload Zephyr and AGL artifacts to the FVP guest.
4. Connect to AGL over `telnet localhost 5001`.
5. Start the AGL guest with [`run-vecu-agl.sh`](./scripts/run-vecu-agl.sh).

"Now Trying - Not running yet" :

6. Connect to Zephyr Realm over `telnet localhost 5002`.
7. Start the Zephyr Realm VM with [`run-vecu-zephyr.sh`](./scripts/run-vecu-zephyr.sh).


