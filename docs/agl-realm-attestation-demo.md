# AGL + Zephyr Realm Attestation Demo

This is the paper-facing end-to-end path for the current V-ECU attestation
experiment.

## What This Demo Runs

| Port | Console | Role |
|------|---------|------|
| 5000 | FVP Linux | KVM host / orchestration shell |
| 5001 | AGL Normal VM | Normal World V-ECU console |
| 5002 | Zephyr Realm VM | Realm V-ECU mini-shell |

End-to-end path:

1. Zephyr Realm generates a real CCA RSI attestation token.
2. Zephyr publishes the token through the Realm shared buffer.
3. kvmtool copies the token from Realm memory and sends it to the AGL VM.
4. `agl_attest_verifier` runs inside AGL, validates the token structure,
   computes SHA-256, measures AGL-side processing time, and returns an ack.
5. The Realm shell prints CSV rows that include both Realm-side and AGL-side
   timing fields.

Current verifier scope: the AGL userspace verifier checks size bounds,
definite-length CBOR structure, and SHA-256 digest timing. Full CCA COSE
signature and certificate-chain validation requires platform attestation key
material and should be added as the next cryptographic hardening step.

## Host Build And Upload

Run this on the host, not inside FVP:

```bash
cd /home/csos/NDSS/CCA-attestation
./scripts/build-agl-attest-verifier.sh
./scripts/build-kvmtool-arm64.sh
MINI_SHELL_MODE=1 ./scripts/build-realm-zephyr-shim-bundle.sh
./scripts/upload-agl-realm-attest-demo.sh root@192.168.122.33 \
  --agl-kernel /path/to/AGL/Image \
  --agl-disk /path/to/agl.ext4
```

If `/root/guest-Image` and `/root/agl.ext4` are already present in FVP Linux,
the AGL options can be omitted:

```bash
./scripts/upload-agl-realm-attest-demo.sh root@192.168.122.33
```

The upload script installs these FVP-side files:

- `/root/lkvm-static`
- `/root/realm-zephyr-shim.bin`
- `/root/agl_attest_verifier`
- `/root/install-agl-attest-verifier-rootfs.sh`
- `/root/run-agl-realm-attest-demo.sh`
- VM helper scripts such as `run-vecu-agl.sh`, `run-realm-shim-zephyr.sh`, and
  `stop-lkvm-vm.sh`

## FVP Linux One-Shot Start

Connect to FVP Linux:

```bash
telnet localhost 5000
```

Inside FVP Linux:

```bash
/root/run-agl-realm-attest-demo.sh --iterations 50
```

That command:

- installs `agl_attest_verifier` into `/root/agl.ext4`
- starts AGL Normal VM on `telnet localhost 5001`
- starts Zephyr Realm mini-shell on `telnet localhost 5002`
- boots AGL on a tap network by default
  - FVP Linux tap-side IP: `192.168.34.1`
  - AGL guest IP: `192.168.34.15`
- waits until the Realm mini-shell is ready
- waits until kvmtool receives a TCP connection from the AGL verifier

This wait can take several minutes because AGL must boot far enough to launch
its service. The default AGL verifier timeout is 300 seconds:

```bash
/root/run-agl-realm-attest-demo.sh --iterations 50 --agl-timeout 420
```

If you run AGL manually before starting the Realm, use the same tap settings:

```bash
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

## Run The Measurement

Open the AGL console from a host terminal:

```bash
telnet localhost 5001
```

Open the Realm console from another host terminal:

```bash
telnet localhost 5002
```

In the Realm shell:

```text
help
normal attest 50
```

Expected Realm output shape:

```text
normal attest: paper measurement
warmup=10,iterations=50
csv,iter,token_size,gen_cycles,gen_ns,publish_cycles,publish_ns,normal_cycles,normal_ns,total_cycles,total_ns,agl_verify_ns,agl_hash_ns,status
csv,1,1218,...
summary,gen_ns,mean_ns=...
summary,publish_ns,mean_ns=...
summary,normal_ack_ns,mean_ns=...
summary,total_ns,mean_ns=...
summary,agl_verify_ns,mean_ns=...
```

Expected AGL verifier line:

```text
agl_csv,gen=...,token_size=1218,realm_gen_ns=...,parse_ns=...,hash_ns=...,total_ns=...,status=0x00000000,sha256=...
```

Useful FVP-side logs:

```bash
tail -f /root/agl-normal.uart.log
tail -f /root/realm-vecu1.log
```

## Success Criteria

- `telnet localhost 5001` shows AGL boot or an AGL shell/login prompt.
- `telnet localhost 5002` shows `Realm mini shell ready`.
- `/root/realm-vecu1.log` shows `realm AGL verifier connected`.
- `normal attest 50` prints CSV rows with `agl_verify_ns` and `agl_hash_ns`.
- AGL console or `/root/agl-normal.uart.log` shows `agl_csv,...status=0x00000000`.
- `/root/realm-vecu1.token.bin` and `/root/realm-vecu1.token.meta` exist after
  a standalone `attest` command or generic token dump path.

## Safe Cleanup

Inside FVP Linux:

```bash
/root/stop-lkvm-vm.sh realm-vecu1
/root/stop-lkvm-vm.sh agl-normal
```

These commands stop only the named VM and do not kill unrelated guests.
