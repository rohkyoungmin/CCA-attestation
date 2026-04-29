# ARM CCA V-ECU Platform

The codebase is centered on Arm FVP and a four-world stack:

- Normal World: Linux host, plus KVM guests such as AGL
- Realm World: TF-RMM with Zephyr Realm VM
- Secure World: Hafnium and OP-TEE
- Root World: TF-A

The project has been tested on Ubuntu 22.04 on both `x86_64` and `aarch64`, with at least 16 GB RAM recommended.

## Repository Layout

- [`scripts/`](./scripts): environment setup, build, run, and benchmark entrypoints
- [`docs/`](./docs): client usage, Realm shim design, and attestation demo guides
- [`src/sc_client/`](./src/sc_client): local monitor client and kernel driver
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
./scripts/build.sh paper-demo
```

For the paper-facing AGL + Zephyr Realm attestation demo, the recommended build
entrypoint is:

```bash
./scripts/build.sh paper-demo
```

This builds:

- patched arm64 `lkvm-static`
- AGL userspace attestation verifier
- Realm shim + Zephyr mini-shell bundle

The `kvmtool-cca` source tree lives under `dev_workspace/` and is not vendored
into this repository. The required local changes are stored as
[`patches/kvmtool-cca-realm-zephyr-attestation.patch`](./patches/kvmtool-cca-realm-zephyr-attestation.patch)
and are applied automatically by:

```bash
./scripts/build-kvmtool-arm64.sh
```

The resulting binary is `dev_workspace/kvmtool-cca/lkvm-static`.

### 3. Launch FVP

```bash
./scripts/bootfvp.sh
```

The FVP Linux login is:

- user: `root`
- password: `root`

## Running the Platform

The commands below are self-contained. A new user should be able to reproduce
the current AGL + Zephyr Realm attestation experiment from this README alone.

### Runtime Port Map

| Host telnet port | FVP UART | Guest / role |
|------------------|----------|--------------|
| `5000` | `ttyAMA0` | FVP Normal World Linux / KVM host shell |
| `5001` | `ttyAMA1` | AGL Normal World KVM guest |
| `5002` | `ttyAMA2` | Zephyr Realm mini-shell |
| `5003` | `ttyAMA3` | Optional fallback serial port |

The FVP Linux login is:

- user: `root`
- password: `root`

### Full Reproduction Flow

Use four terminals on the host.

#### Terminal A: boot FVP and build/upload artifacts

Start FVP:

```bash
cd /home/csos/NDSS/CCA-attestation
./scripts/bootfvp.sh
```

Build the paper-facing artifacts:

```bash
cd /home/csos/NDSS/CCA-attestation
./scripts/build.sh paper-demo
```

Upload the runtime artifacts to FVP Linux:

```bash
./scripts/upload-agl-realm-attest-demo.sh root@192.168.122.33
```

If the FVP Linux guest does not already have `/root/guest-Image` and
`/root/agl.ext4`, upload the AGL kernel and disk explicitly:

```bash
./scripts/upload-agl-realm-attest-demo.sh root@192.168.122.33 \
  --agl-kernel /path/to/AGL/Image \
  --agl-disk /path/to/agl.ext4
```

#### Terminal B: FVP Linux shell on port 5000

Connect to FVP Linux:

```bash
telnet localhost 5000
```

Run the full one-shot experiment launcher:

```bash
/root/run-agl-realm-attest-demo.sh --iterations 50 --agl-timeout 420
```

The launcher performs these steps:

- installs `/root/agl_attest_verifier` into `/root/agl.ext4`
- starts AGL on telnet `5001`
- starts the Zephyr Realm mini-shell on telnet `5002`
- starts AGL on a tap network
- uses FVP Linux tap IP `192.168.34.1`
- uses AGL guest IP `192.168.34.15`
- waits until `/root/realm-vecu1.log` contains `realm AGL verifier connected`

If you prefer to start AGL first and inspect it manually, run this instead in
the FVP Linux shell:

```bash
/root/stop-lkvm-vm.sh realm-vecu1
/root/stop-lkvm-vm.sh agl-normal

/root/install-agl-attest-verifier-rootfs.sh \
  --disk /root/agl.ext4 \
  --verifier /root/agl_attest_verifier \
  --host-ip 192.168.34.1 \
  --port 7777

/root/run-vecu-agl.sh \
  --serial-tty /dev/ttyAMA1 \
  --telnet-port 5001 \
  --net-mode tap \
  --net-host-ip 192.168.34.1 \
  --net-guest-ip 192.168.34.15
```

After AGL has booted, start the Realm from the FVP Linux shell:

```bash
LKVM_REALM_AGL_VERIFY=1 \
LKVM_REALM_AGL_VERIFY_PORT=7777 \
/root/run-realm-shim-zephyr.sh --shell-port 5002 /root/realm-zephyr-shim.bin
```

Confirm that the AGL verifier is connected:

```bash
grep "realm AGL verifier connected" /root/realm-vecu1.log
```

#### Terminal C: AGL console on port 5001

Open the AGL console:

```bash
telnet localhost 5001
```

Useful checks inside AGL:

```bash
ip addr
ping -c 1 192.168.34.1
ps | grep agl_attest_verifier
```

If the verifier service did not start automatically, run it manually from AGL:

```bash
/usr/bin/agl_attest_verifier --host 192.168.34.1 --port 7777
```

Expected AGL verifier output:

```text
[agl-verifier] connected
agl_csv,gen=...,token_size=1218,realm_gen_ns=...,parse_ns=...,hash_ns=...,total_ns=...,status=0x00000000,sha256=...
```

#### Terminal D: Zephyr Realm shell on port 5002

Open the Realm mini-shell:

```bash
telnet localhost 5002
```

Run the paper-facing measurement:

```text
normal attest 50
```

Expected success pattern:

```text
csv,1,1218,...,agl_verify_ns,agl_hash_ns,0x0000000000000000
summary,gen_ns,...
summary,publish_ns,...
summary,normal_ack_ns,...
summary,agl_verify_ns,...
summary,total_ns,...
```

For a quick smoke test, one iteration is enough:

```text
normal attest
```

#### FVP-side evidence logs

Inside the FVP Linux shell:

```bash
grep -E "realm AGL verifier connected|realm AGL verifier ack|normal-world attest" /root/realm-vecu1.log
tail -n 80 /root/agl-normal.uart.log
```

Successful runs should show:

```text
realm AGL verifier connected on port 7777
realm AGL verifier ack gen=... token_size=1218 parse_ns=... hash_ns=... total_ns=...
```

#### Safe cleanup

Inside the FVP Linux shell:

```bash
/root/stop-lkvm-vm.sh realm-vecu1
/root/stop-lkvm-vm.sh agl-normal
```

These stop only the named VM and do not kill unrelated `lkvm` guests.

### Upload Runtime Scripts To FVP

The run helpers live in this repository, but they must also be copied into the FVP Linux guest before use. From the host machine:

```bash
scp dev_workspace/kvmtool-cca/lkvm-static root@192.168.122.33:/root/lkvm-static.new
scp scripts/run-vecu-zephyr.sh root@192.168.122.33:/root/run-vecu-zephyr.sh
scp scripts/run-realm-shim-zephyr.sh root@192.168.122.33:/root/run-realm-shim-zephyr.sh
scp scripts/run-vecu-agl.sh root@192.168.122.33:/root/run-vecu-agl.sh
scp scripts/run-realm-linux.sh root@192.168.122.33:/root/run-realm-linux.sh
scp scripts/run-realm-linux-baseline.sh root@192.168.122.33:/root/run-realm-linux-baseline.sh
scp scripts/stop-lkvm-vm.sh root@192.168.122.33:/root/stop-lkvm-vm.sh
scp scripts/read-realm-token.sh root@192.168.122.33:/root/read-realm-token.sh
```

Inside FVP Linux:

```bash
mv /root/lkvm-static.new /root/lkvm-static
chmod +x /root/lkvm-static
chmod +x /root/run-vecu-zephyr.sh
chmod +x /root/run-realm-shim-zephyr.sh
chmod +x /root/run-vecu-agl.sh
chmod +x /root/run-realm-linux.sh
chmod +x /root/run-realm-linux-baseline.sh
chmod +x /root/stop-lkvm-vm.sh
chmod +x /root/read-realm-token.sh
```

### Linux Realm Baseline

For Realm bring-up comparison, the repository also includes a Linux Realm baseline.
The known-good comparison path is [`run-realm-linux-baseline.sh`](./scripts/run-realm-linux-baseline.sh), which launches a Linux Realm guest in the current shell with:

- `virtio-console`
- no disk image
- `128M` RAM

Inside FVP Linux, upload the Linux Image and the helper scripts:

```bash
scp src/linux/arch/arm64/boot/Image root@192.168.122.33:/root/realm-linux-Image
scp scripts/run-realm-linux.sh root@192.168.122.33:/root/run-realm-linux.sh
scp scripts/run-realm-linux-baseline.sh root@192.168.122.33:/root/run-realm-linux-baseline.sh
scp scripts/stop-lkvm-vm.sh root@192.168.122.33:/root/stop-lkvm-vm.sh
```

Then, inside FVP Linux:

```bash
chmod +x /root/run-realm-linux.sh /root/run-realm-linux-baseline.sh /root/stop-lkvm-vm.sh
/root/stop-lkvm-vm.sh realm-linux
/root/run-realm-linux-baseline.sh
```

This baseline is useful for confirming that the FVP, RMM, and `kvmtool-cca` Realm stack is healthy before comparing it against Zephyr Realm bring-up.

### Zephyr Realm Porting TODO

The Linux Realm baseline is the reference for the current Zephyr-on-Realm bring-up work.
The detailed porting checklist lives in [`docs/zephyr-realm-porting-todo.md`](./docs/zephyr-realm-porting-todo.md).

The adapter-layer design for translating between Arm CCA Realm rules and Zephyr runtime expectations lives in
[`docs/zephyr-realm-adapter-design.md`](./docs/zephyr-realm-adapter-design.md).

The preferred long-term architecture, however, is a standalone Realm shim that keeps Zephyr kernel code unmodified:
[`docs/realm-shim-design.md`](./docs/realm-shim-design.md).

The initial standalone shim scaffold can be built with:

```bash
./scripts/build-realm-shim.sh
```

The active shim-first Zephyr test path now uses a bundled image:

```bash
./scripts/build-realm-zephyr-shim-bundle.sh
```

To build the interactive shell-oriented variant instead of the headless
bring-up profile:

```bash
SHELL_MODE=1 ./scripts/build-realm-zephyr-shim-bundle.sh
```

To build the recommended UART mini-shell variant on top of the already proven
`payload_main` baseline:

```bash
MINI_SHELL_MODE=1 ./scripts/build-realm-zephyr-shim-bundle.sh
```

This produces:

- shim entry at `0x80000000`
- Zephyr payload at `0x80200000`
- single bundle image at `src/realm_shim/build/realm_zephyr_shim.bin`

To upload and run that bundle on FVP:

```bash
scp dev_workspace/kvmtool-cca/lkvm-static root@192.168.122.33:/root/lkvm-static.new
scp src/realm_shim/build/realm_zephyr_shim.bin root@192.168.122.33:/root/realm-zephyr-shim.bin
scp scripts/run-vecu-zephyr.sh root@192.168.122.33:/root/run-vecu-zephyr.sh
scp scripts/run-realm-shim-zephyr.sh root@192.168.122.33:/root/run-realm-shim-zephyr.sh
scp scripts/stop-lkvm-vm.sh root@192.168.122.33:/root/stop-lkvm-vm.sh
scp scripts/read-realm-token.sh root@192.168.122.33:/root/read-realm-token.sh
```

Inside FVP Linux:

```bash
chmod +x /root/run-vecu-zephyr.sh /root/run-realm-shim-zephyr.sh /root/stop-lkvm-vm.sh /root/read-realm-token.sh
/root/stop-lkvm-vm.sh realm-vecu1
mv /root/lkvm-static.new /root/lkvm-static
chmod +x /root/lkvm-static
/root/run-realm-shim-zephyr.sh --shell-port 5002 /root/realm-zephyr-shim.bin
```

### Realm Shell Bring-Up

The repository now includes a Realm-safe polling UART driver for the
`lkvm_realm_payload` board and a shell profile that keeps the standalone shim
model intact while enabling an interactive Zephyr shell.

Build the experimental built-in shell bundle on the host:

```bash
SHELL_MODE=1 ./scripts/build-realm-zephyr-shim-bundle.sh
```

Upload the fresh bundle:

```bash
scp dev_workspace/kvmtool-cca/lkvm-static root@192.168.122.33:/root/lkvm-static.new
scp src/realm_shim/build/realm_zephyr_shim.bin root@192.168.122.33:/root/realm-zephyr-shim.bin
scp scripts/read-realm-token.sh root@192.168.122.33:/root/read-realm-token.sh
```

Then inside FVP Linux:

```bash
/root/stop-lkvm-vm.sh realm-vecu1
mv /root/lkvm-static.new /root/lkvm-static
chmod +x /root/lkvm-static
chmod +x /root/read-realm-token.sh
rm -f /root/.lkvm/realm-vecu1.sock /root/realm-vecu1.log /root/realm-vecu1.dtb
/root/run-realm-shim-zephyr.sh --shell-port 5002 /root/realm-zephyr-shim.bin
```

Connect from the host:

```bash
telnet localhost 5002
```

Expected shell prompt:

```text
realm:~$
```

The first custom shell commands are:

```text
realm status
realm ping
```

`realm status` prints the runtime handoff context that the shim passed to the
payload, and `realm ping` is the first command-path validation hook before the
attestation app commands are added.

If the built-in shell profile regresses Realm MMU bring-up, use the mini-shell
variant instead. It keeps the proven `payload_main` path, owns the UART
transport in the app, and forwards entered lines into the Zephyr shell core
through the dummy backend.

Build the mini-shell bundle:

```bash
MINI_SHELL_MODE=1 ./scripts/build-realm-zephyr-shim-bundle.sh
```

Upload and run it the same way:

```bash
scp dev_workspace/kvmtool-cca/lkvm-static root@192.168.122.33:/root/lkvm-static.new
scp src/realm_shim/build/realm_zephyr_shim.bin root@192.168.122.33:/root/realm-zephyr-shim.bin
scp scripts/run-vecu-zephyr.sh root@192.168.122.33:/root/run-vecu-zephyr.sh
scp scripts/run-realm-shim-zephyr.sh root@192.168.122.33:/root/run-realm-shim-zephyr.sh
scp scripts/read-realm-token.sh root@192.168.122.33:/root/read-realm-token.sh
```

Inside FVP Linux:

```bash
/root/stop-lkvm-vm.sh realm-vecu1
mv /root/lkvm-static.new /root/lkvm-static
chmod +x /root/lkvm-static /root/run-vecu-zephyr.sh /root/run-realm-shim-zephyr.sh /root/read-realm-token.sh
rm -f /root/.lkvm/realm-vecu1.sock /root/realm-vecu1.log /root/realm-vecu1.dtb
/root/run-realm-shim-zephyr.sh --shell-port 5002 /root/realm-zephyr-shim.bin
```

Then connect from the host. The default Zephyr Realm mini-shell path is
`/dev/ttyAMA2`, normally exposed by FVP as telnet port `5002`.

```bash
telnet localhost 5002
```

If that port is polluted by TF-A/RMM `SMC_RSI` or `SMC_RMM` tracing, move only
the mini-shell stream to another FVP UART:

```bash
/root/run-realm-shim-zephyr.sh --shell-port 5003 /root/realm-zephyr-shim.bin
```

`--shell-port 5004` is also accepted by the script, but it requires the FVP
model to expose `/dev/ttyAMA4` / telnet `5004`. The current `bootfvp.sh` enables
UART0..UART3, so stock runs normally have ports `5000` through `5003`.

For clean paper measurements, do not pass `--debug`. The token watcher still
runs by default and dumps the token files. Use `--debug` only when you need
kvmtool phase/status logs; those logs are intentionally kept in
`/root/realm-vecu1.log`, not in the mini-shell command path. The `SMC_RSI` and
`SMC_RMM` lines themselves are TF-RMM/TF-A console traces. If they appear on the
same FVP UART as the mini-shell, move the shell with `--shell-port` or rebuild
TF-RMM with lower `RSI_LOG_LEVEL` / `LOG_LEVEL` for a quiet run.

Expected prompt:

```text
realm:~$
```

Mini-shell commands:

```text
help
status
ping
normal hello-normal
normal attest 50
attest
```

The `realm status`, `realm ping`, `realm normal`, and `realm attest` forms are
also supported for compatibility with earlier test bundles. `normal [msg]`
proves Realm-to-Normal-World command dispatch: the Realm writes a request into
the shared control page, the kvmtool Normal World watcher handles it, and the
Realm shell waits for an acknowledgement.

Expected Realm shell output:

```text
realm:~$ normal hello-normal
normal: sending request
normal: ack gen=0x0000000000000001 status=0x0000000000000000
```

Expected Normal World signal in `/root/realm-vecu1.log` or in the foreground
kvmtool console:

```text
Info: realm normal-world request gen=1 req=1 arg0=0x100000000 arg1=0x83e00000 msg="hello-normal"
```

If `normal` prints `normal: no ack`, the most likely causes are an old
`/root/lkvm-static` binary or a run started with `--no-token-watch`. Upload the
new `lkvm-static`, move it into place, and run the VM without
`--no-token-watch`.

`attest` is the paper-facing entry point: it requests a Realm attestation token
through RSI, measures the token generation path, and publishes the resulting
token through the fixed shared buffer path for the host side to consume.

For paper-facing measurements, use `normal attest [N]`. This command performs
the end-to-end Realm-to-AGL verifier path in one loop:

1. generate a CCA attestation token inside the Zephyr Realm with RSI
2. publish the token to the shared Realm-to-Normal buffer
3. notify the Normal World kvmtool watcher
4. send the token to the AGL userspace verifier
5. wait for AGL verifier ack/status
6. print per-iteration CSV rows and summary statistics

Example:

```text
realm:~$ normal attest 50
normal attest: paper measurement
warmup=10,iterations=50
csv,iter,token_size,gen_cycles,gen_ns,publish_cycles,publish_ns,normal_cycles,normal_ns,total_cycles,total_ns,agl_verify_ns,agl_hash_ns,status
csv,1,1218,...
summary,gen_ns,mean_ns=...,min_ns=...,p50_ns=...,p95_ns=...,max_ns=...
summary,publish_ns,mean_ns=...,min_ns=...,p50_ns=...,p95_ns=...,max_ns=...
summary,normal_ack_ns,mean_ns=...,min_ns=...,p50_ns=...,p95_ns=...,max_ns=...
summary,total_ns,mean_ns=...,min_ns=...,p50_ns=...,p95_ns=...,max_ns=...
summary,agl_verify_ns,mean_ns=...,min_ns=...,p50_ns=...,p95_ns=...,max_ns=...
```

The measured fields correspond to the current paper experiment boundary:
`gen_ns` is RSI token generation time inside the Realm, `publish_ns` is copying
the token into the shared buffer and updating the control page,
`normal_ack_ns` is the Realm-to-Normal-to-AGL verifier ack path, `agl_verify_ns`
is the AGL userspace verifier processing time, `agl_hash_ns` is the SHA-256
digest time inside AGL, and `total_ns` is the end-to-end time for the current
Realm-to-AGL attestation path.

`kvmtool` watches the Realm shared token/control pages from Normal World by
default and dumps the latest attestation result to:

```text
/root/realm-vecu1.token.bin
/root/realm-vecu1.token.meta
```

After `attest` succeeds in the Zephyr shell, inspect the host-side dump inside
FVP Linux:

```bash
/root/read-realm-token.sh
```

For a standalone Zephyr Realm proof, only the Zephyr Realm VM needs to run. For
the V-ECU communication and end-to-end overhead experiments, run the Normal
World AGL guest at the same time. The final paper-facing path uses kvmtool only
as the transport endpoint and performs AGL-side token consumption in the
`agl_attest_verifier` userspace process.

The running implementation notes and experiment history live in
[`docs/zephyr-realm-porting-worklog.md`](./docs/zephyr-realm-porting-worklog.md).

### Safe VM Shutdown

The run scripts are designed to avoid killing unrelated guests. To stop a guest by name without touching the others, use [`stop-lkvm-vm.sh`](./scripts/stop-lkvm-vm.sh):

```bash
/root/stop-lkvm-vm.sh realm-linux
/root/stop-lkvm-vm.sh realm-vecu1
/root/stop-lkvm-vm.sh agl-normal
```

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

## Notes

- `env.sh all` prepares external sources and tools. It does not create every build artifact by itself.
- `build.sh all` performs the project builds, but some targets such as AGL can take a long time on the first run.
- Several large assets and generated outputs are intentionally kept out of version control.
