#!/bin/sh
# Inspect the token and metadata dumped by kvmtool's Realm token watcher.
# Run inside the FVP Linux host after `attest` succeeds in the Zephyr Realm.

TOKEN_FILE="${1:-/root/realm-vecu1.token.bin}"
META_FILE="${2:-/root/realm-vecu1.token.meta}"

if [ ! -f "$META_FILE" ]; then
	echo "[ERROR] Token metadata not found: $META_FILE"
	echo "        Run the Realm VM with --debug, then execute 'attest' in the Zephyr shell."
	exit 1
fi

if [ ! -f "$TOKEN_FILE" ]; then
	echo "[ERROR] Token dump not found: $TOKEN_FILE"
	echo "        Metadata exists, but token bytes were not dumped."
	exit 1
fi

token_size="$(sed -n 's/^token_size=//p' "$META_FILE" | tail -n 1)"
actual_size="$(wc -c < "$TOKEN_FILE" | tr -d ' ')"

echo "[*] Realm token metadata"
cat "$META_FILE"
echo ""
echo "[*] Realm token file"
echo "token_file=$TOKEN_FILE"
echo "actual_size=$actual_size"

if [ -n "$token_size" ] && [ "$token_size" != "$actual_size" ]; then
	echo "[WARN] token_size metadata does not match actual file size"
fi

if command -v sha256sum >/dev/null 2>&1; then
	echo ""
	echo "[*] Token SHA-256"
	sha256sum "$TOKEN_FILE"
fi

if command -v od >/dev/null 2>&1; then
	echo ""
	echo "[*] Token first 64 bytes"
	od -An -tx1 -N 64 "$TOKEN_FILE"
fi
