#!/bin/bash
# measure_all.sh — Full V-ECU Attestation + Communication Benchmark
#
# Runs all measurement phases and produces a consolidated report:
#   1. CCA Attestation Token Generation (attest_gen)
#   2. Attestation Token Verification   (token_verify)
#   3. V-ECU Inter-VM Communication     (vecu_comm)
#      Phase 0: Baseline TCP
#      Phase 1: TLS 1.3 handshake + echo
#      Phase 2: TLS + CCA attestation token verify
#
# Output: results/measurement_YYYYMMDD_HHMMSS.txt

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
PROJ_DIR="$(dirname "$SCRIPT_DIR")"
ATTEST_DIR="$PROJ_DIR/src/vecu_attest"
COMM_DIR="$PROJ_DIR/src/vecu_comm"
RESULTS_DIR="$PROJ_DIR/results"

GREEN='\033[0;32m'
RED='\033[0;31m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

log()       { printf "${GREEN}[*] $1${NC}\n"; }
log_error() { printf "${RED}[!] $1${NC}\n"; }
log_head()  { printf "${CYAN}${BOLD}$1${NC}\n"; }

TIMESTAMP=$(date '+%Y%m%d_%H%M%S')
mkdir -p "$RESULTS_DIR"
OUTFILE="$RESULTS_DIR/measurement_${TIMESTAMP}.txt"

# Tee all output to file
exec > >(tee -a "$OUTFILE") 2>&1

log_head "========================================================"
log_head " SCRUTINIZER — V-ECU Attestation & Communication Benchmark"
log_head " $(date '+%Y-%m-%d %H:%M:%S')"
log_head "========================================================"
echo ""
echo "System info:"
echo "  Host   : $(uname -a)"
echo "  CPU    : $(grep 'model name' /proc/cpuinfo | head -1 | cut -d: -f2 | xargs)"
echo "  Cores  : $(nproc)"
echo "  RAM    : $(free -h | awk '/^Mem/{print $2}')"
echo "  OpenSSL: $(openssl version)"
echo ""

# ----------------------------------------------------------------
# Step 1: Build
# ----------------------------------------------------------------
log_head "--- Step 1: Build ---"

log "Building attest_gen and token_verify..."
make -C "$ATTEST_DIR" -j$(nproc)
if [ $? -ne 0 ]; then
    log_error "attest build failed. Check: sudo apt install libssl-dev"
    exit 1
fi

log "Building vecu_comm..."
make -C "$COMM_DIR" -j$(nproc)
if [ $? -ne 0 ]; then
    log_error "vecu_comm build failed."
    exit 1
fi

# ----------------------------------------------------------------
# Step 2: Attestation Token Generation
# ----------------------------------------------------------------
log_head ""
log_head "--- Step 2: CCA Attestation Token Generation ---"
log "Running attest_gen (200 iterations, ECDSA-P384 + CBOR)..."
"$ATTEST_DIR/attest_gen"
if [ $? -ne 0 ]; then
    log_error "attest_gen failed."
    exit 1
fi
echo ""
log "Token written to /tmp/cca_token.bin"
log "Token size: $(wc -c < /tmp/cca_token.bin 2>/dev/null || echo 'N/A') bytes (including 4-byte length header)"

# ----------------------------------------------------------------
# Step 3: Attestation Token Verification
# ----------------------------------------------------------------
log_head ""
log_head "--- Step 3: CCA Attestation Token Verification ---"
log "Running token_verify (200 iterations, CBOR parse + ECDSA verify + claim check)..."
"$ATTEST_DIR/token_verify"
if [ $? -ne 0 ]; then
    log_error "token_verify failed."
    exit 1
fi

# ----------------------------------------------------------------
# Step 4: V-ECU Communication Benchmark
# ----------------------------------------------------------------
log_head ""
log_head "--- Step 4: V-ECU Inter-VM Communication Benchmark ---"
log "Running vecu_comm (Phase 0: TCP / Phase 1: TLS / Phase 2: TLS+Attest)..."
log "Note: Phase 1 & 2 fork 200 server processes each — takes ~30-60s total"
echo ""
"$COMM_DIR/vecu_comm"
if [ $? -ne 0 ]; then
    log_error "vecu_comm failed."
    exit 1
fi

# ----------------------------------------------------------------
# Step 5: Summary
# ----------------------------------------------------------------
log_head ""
log_head "========================================================"
log_head " MEASUREMENT COMPLETE"
log_head "========================================================"
echo ""
echo "Full results saved to: $OUTFILE"
echo ""
echo "Key metrics to report:"
echo "  Attestation:"
echo "    - Token generation mean (us)"
echo "    - Token verification mean (us): CBOR + SigVerify + ClaimCheck breakdown"
echo "  Communication (논리적 오버헤드 추이):"
echo "    - TCP baseline RTT (us)"
echo "    - TLS handshake overhead vs TCP (%)"
echo "    - TLS per-msg RTT overhead vs TCP (%)"
echo "    - TLS+Attest session overhead vs TCP (%)"
echo ""
echo "V-ECU Topology simulated:"
echo "  V-ECU1 (Zephyr/Realm-EL1) ←TCP→     V-ECU2 (AGL/Normal)    [PORT 9001]"
echo "  V-ECU1 (Zephyr/Realm-EL1) ←TLS→     V-ECU2 (AGL/Normal)    [PORT 9002]"
echo "  V-ECU1 (Zephyr/Realm-EL1) ←TLS+CCA→ V-ECU3 (Linux/Normal)  [PORT 9003]"
