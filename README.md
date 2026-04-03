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
4. Start the Zephyr Realm VM with [`run-vecu-zephyr.sh`](./scripts/run-vecu-zephyr.sh).
5. Start the AGL guest with [`run-vecu-agl.sh`](./scripts/run-vecu-agl.sh).

## Client

For monitor testing, build and upload the local client and kernel module:

```bash
cd src/sc_client
make
scp ./sc_user_client root@192.168.122.33:~/

cd driver
make
scp ./sc_manager.ko root@192.168.122.33:~/
```

Inside FVP Linux:

```bash
insmod sc_manager.ko
sc_user_client -h
```

More usage examples are documented in [`docs/client.md`](./docs/client.md).

## Measurements

For the attestation and V-ECU communication microbenchmarks, run:

```bash
./scripts/measure_all.sh
```

This covers:

- CCA attestation token generation
- token verification
- baseline TCP, TLS, and TLS-plus-attestation communication phases

## Notes

- `env.sh all` prepares external sources and tools. It does not create every build artifact by itself.
- `build.sh all` performs the project builds, but some targets such as AGL can take a long time on the first run.
- Several large assets and generated outputs are intentionally kept out of version control.