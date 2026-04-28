# Runtime Patches

This directory stores small patches for third-party source trees that are
downloaded into `dev_workspace/` and are intentionally not vendored into this
repository.

## `kvmtool-cca-realm-zephyr-attestation.patch`

Applied automatically by:

```bash
./scripts/build-kvmtool-arm64.sh
```

The patch adds the Normal World support needed by the paper-facing demo:

- Realm status and live-PC debug watchers
- Realm shared token/control-page watcher
- AGL verifier TCP handoff used by `normal attest [N]`
- Realm UART shared-alias MMIO traps for the Zephyr mini-shell
- optional Realm entry offset support for bring-up experiments
