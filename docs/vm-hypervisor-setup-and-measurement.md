# VM-Hypervisor 확장 및 성능 측정 가이드

> **목적:** SCRUTINIZER 아키텍처 위에 Hypervisor를 추가하여 Realm VM(Zephyr RTOS), Normal VM(AGL, Linux)을 구동하고, VM 간 Attestation 및 V-ECU 통신 성능을 측정하는 방법을 기술한다.
>
> **주의:** 이 문서는 설계/구현 가이드이며, 현재 코드를 직접 수정하지 않는다.

> **⚠️ FVP 환경 성능 측정 제약 (반드시 숙지):**
> ARM FVP(Functional Virtual Platform)는 하드웨어의 기능(Functionality)을 소프트웨어로 시뮬레이션하는 모델이며, **Cycle-accurate 에뮬레이터가 아니다.**
> `clock_gettime`, `k_cycle_get_64()`, `time -v` 등으로 측정한 절대 수치(μs, ms)는 호스트 PC의 성능과 FVP 시뮬레이션 오버헤드가 뒤섞인 값이므로 **실제 ARM 하드웨어에서의 성능을 대표하지 않는다.**
> 본 프레임워크에서 FVP 측정값은 **"보안 기능 적용 전/후의 상대적 오버헤드 비율(Relative Overhead)"** 을 증명하는 데 사용해야 하며, 논문 등에서는 반드시 *"FVP 환경에서의 논리적 오버헤드 추이"* 임을 명시해야 한다. 자세한 내용은 [§7.0 측정 전략](#70-fvp-환경에서의-측정-전략-절대값-vs-상대값)을 참고.

---

## 목차

1. [목표 아키텍처](#1-목표-아키텍처)
2. [현재 vs 목표 구조 비교](#2-현재-vs-목표-구조-비교)
3. [Realm VM: Zephyr RTOS 구동](#3-realm-vm-zephyr-rtos-구동)
4. [Normal World Hypervisor 추가](#4-normal-world-hypervisor-추가)
5. [Normal VM: AGL 구동](#5-normal-vm-agl-구동)
6. [Normal VM: Linux 구동](#6-normal-vm-linux-구동)
7. [Attestation 성능 측정](#7-attestation-성능-측정)
8. [V-ECU 간 통신 성능 측정](#8-v-ecu-간-통신-성능-측정)
9. [FVP 실행 환경 수정 포인트 요약](#9-fvp-실행-환경-수정-포인트-요약)

**⚠️ 현실적 난관 요약 (구현 전 반드시 확인):**
| 번호 | 항목 | 난이도 | 섹션 |
|---|---|---|---|
| 1 | FVP 성능 측정 — 절대값 신뢰 불가 | 🔴 높음 | §7.0 |
| 2 | Zephyr Realm 부팅 — RSI 초기화 / Granule 전환 필요 | 🔴 높음 | §3.2, §3.5 |
| 3 | Realm↔Normal 통신 — NS Shared Memory Delegation 필수 | 🔴 높음 | §6.3 |
| 4 | ~~FVP 위 AAOS~~ → **AGL로 교체** (Yocto 기반, 경량, ARM64 공식 지원) | 🟢 낮음 | §5 |

---

## 1. 목표 아키텍처

```
┌──────────────────────────────────────────────────────────────────┐
│                     ARM FVP (Base RevC-2xAEMvA)                 │
│                                                                  │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │                   EL3: TF-A + SCRUTINIZER Monitor        │    │
│  └─────────────────────────────────────────────────────────┘    │
│                                                                  │
│  ┌────────────────────┐    ┌──────────────────────────────┐     │
│  │   REALM WORLD      │    │       SECURE WORLD           │     │
│  │                    │    │                              │     │
│  │  Realm-EL2: TF-RMM │    │  S-EL2: Hafnium (SPM)       │     │
│  │  ┌──────────────┐  │    │  S-EL1: OPTEE-OS             │     │
│  │  │ Realm-EL1:   │  │    │                              │     │
│  │  │ Zephyr RTOS  │  │    └──────────────────────────────┘     │
│  │  │  (V-ECU 1)   │  │                                         │
│  │  └──────────────┘  │    ┌──────────────────────────────┐     │
│  └────────────────────┘    │      NORMAL WORLD            │     │
│                             │                              │     │
│                             │  Normal-EL2: KVM Hypervisor │     │
│                             │  (Type-2, Linux-KVM 기반)   │     │
│                             │                              │     │
│                             │  ┌───────────┐ ┌──────────┐ │     │
│                             │  │Normal-EL1:│ │Normal-EL1│ │     │
│                             │  │    AGL    │ │  Linux   │ │     │
│                             │  │ (V-ECU 2) │ │(V-ECU 3) │ │     │
│                             │  └───────────┘ └──────────┘ │     │
│                             └──────────────────────────────┘     │
└──────────────────────────────────────────────────────────────────┘
```

### V-ECU 역할 정의

| VM | World | EL | 역할 |
|---|---|---|---|
| Zephyr RTOS | Realm | EL1 (Realm) | 실시간 제어 V-ECU (ADAS, 센서 처리 등) |
| AGL (Automotive Grade Linux) | Normal | EL1 (Guest) | 인포테인먼트 V-ECU (클러스터, IVI 등) |
| Linux (Buildroot 기반) | Normal | EL1 (Guest) | 범용 연산 V-ECU (Edge gateway, OTA 등) |

---

## 2. 현재 vs 목표 구조 비교

| 레이어 | 현재 (SCRUTINIZER) | 목표 (확장) | 변경 사항 |
|---|---|---|---|
| EL3 | TF-A + SCRUTINIZER Monitor | 동일 | 없음 |
| S-EL2 | Hafnium (SPM) | 동일 | 없음 |
| S-EL1 | OPTEE-OS | 동일 | 없음 |
| Realm-EL2 | TF-RMM | 동일 | 없음 |
| **Realm-EL1** | *없음* | **Zephyr RTOS VM** | **신규 추가** |
| **Normal-EL2** | *없음* | **KVM (Linux-KVM)** | **신규 추가** |
| **Normal-EL1** | Linux (직접 부팅) | **AGL VM + Linux VM** | **구조 변경** |

### 핵심 변경 원칙
- **Realm World:** TF-RMM이 이미 Realm VM 관리 가능 → Zephyr를 Realm VM Image로 패키징
- **Normal World:** 기존 Linux를 KVM Hypervisor로 올리고, 그 위에 AGL/Linux를 Guest VM으로 실행
- **EL3/Secure World:** 변경 불필요 (SCRUTINIZER Monitor 유지)

---

## 3. Realm VM: Zephyr RTOS 구동

### 3.1 개념

TF-RMM(Realm Management Monitor)은 ARM CCA(Confidential Computing Architecture)의 Realm world를 관리한다. Realm VM은 Host Linux에서 KVM + KVMTOOL 또는 `lkvm` 명령으로 생성한다. 단, Realm VM은 **Normal World의 KVM에서 RMI(Realm Management Interface)를 통해** TF-RMM에 Realm 생성을 요청한다.

```
Normal-EL1 (KVM Host)  →  RMI call  →  Realm-EL2 (TF-RMM)  →  Realm-EL1 (Zephyr)
```

### 3.2 Zephyr RTOS 포팅 요건

**필요 조건:**
- Zephyr의 `CONFIG_ARM64=y` 빌드 타겟 필요
- AArch64 EL1 에서 동작하는 bare-metal 이미지
- Realm VM 진입 시 register 초기 상태 준수 (ARM CCA 스펙)

**Zephyr 빌드 설정 (zephyr용 `prj.conf`):**
```kconfig
CONFIG_ARM64=y
CONFIG_QEMU_CORTEX_A53=y   # FVP 타겟 시 AEMv8 보드 설정으로 교체
CONFIG_SERIAL=y
CONFIG_UART_CONSOLE=y
CONFIG_STDOUT_CONSOLE=y
CONFIG_PRINTK=y

# V-ECU 역할 시 필요한 RTOS 기능
CONFIG_POSIX_API=y
CONFIG_NETWORKING=y
CONFIG_NET_TCP=y
```

**빌드 커맨드:**
```bash
# Zephyr SDK 설치 후
west build -b qemu_cortex_a53 samples/hello_world \
    -- -DCONFIG_ARM64=y

# 결과물: build/zephyr/zephyr.bin (flat binary)
# Realm VM 이미지로 패키징
```

**주의:** FVP 타겟 BSP가 없으므로 `CONFIG_BOARD_*` 설정을 FVP AEMv8 기준으로 직접 작성하거나, QEMU virt 보드 설정을 최대한 활용해야 한다.

### 3.5 ⚠️ Zephyr Realm 부팅 시 필수 초기화 (구현 난관 #2)

Zephyr가 일반 EL1 OS로 동작하는 것과 달리, **Realm VM으로 진입할 때는 ARM CCA 스펙에 따른 추가 초기화가 반드시 필요**하다. 이를 빠뜨리면 TF-RMM이 Realm 진입을 거부하거나 메모리 접근 폴트가 발생한다.

#### 3.5.1 Granule Transition (메모리 상태 전환)

ARM CCA에서 모든 물리 메모리 페이지(Granule, 4KB)는 아래 상태 중 하나를 가진다:

```
Undelegated (기본, Normal World 소유)
    │
    │  RMI_GRANULE_DELEGATE  (KVM Host → TF-RMM 호출)
    ▼
Delegated (TF-RMM 관리 하에 있으나 미할당)
    │
    │  RMI_REC_CREATE / RMI_DATA_CREATE 등
    ▼
Realm (Realm VM에 할당됨, 암호화·격리)
```

**구현 위치:** Zephyr 부트 코드(`arch/arm64/core/reset.S` 또는 신규 `realm_init.S`)에서 커널 진입 전에 수행해야 한다.

```c
/*
 * realm_boot_init.c
 * Realm VM 진입 직후 (EL1 entry point) 에서 호출
 * lkvm --realm이 Zephyr를 로드한 뒤 Realm-EL1으로 제어를 넘기는 시점
 */

#define RSI_VERSION             0xC4000190
#define RSI_FEATURES            0xC4000191
#define RSI_REALM_CONFIG        0xC4000196  /* IPA space 크기 등 쿼리 */
#define RSI_IPA_STATE_SET       0xC4000197  /* IPA 영역을 Shared/Private 설정 */
#define RSI_IPA_STATE_GET       0xC4000198

/* IPA(Intermediate Physical Address) 상태 */
#define RSI_IPA_STATE_PRIVATE   0  /* Realm 전용 (암호화) */
#define RSI_IPA_STATE_SHARED    1  /* NS(Non-Secure) Shared, Normal World와 공유 가능 */

typedef struct { uint64_t a0, a1, a2, a3; } rsi_result_t;

static inline rsi_result_t rsi_call(uint64_t fn,
                                    uint64_t a1, uint64_t a2, uint64_t a3) {
    rsi_result_t r;
    asm volatile(
        "mov x0, %4\n"
        "mov x1, %5\n"
        "mov x2, %6\n"
        "mov x3, %7\n"
        "hvc #0\n"
        "mov %0, x0\n"
        "mov %1, x1\n"
        "mov %2, x2\n"
        "mov %3, x3\n"
        : "=r"(r.a0), "=r"(r.a1), "=r"(r.a2), "=r"(r.a3)
        : "r"(fn), "r"(a1), "r"(a2), "r"(a3)
        : "x0","x1","x2","x3","memory"
    );
    return r;
}

void realm_boot_init(void) {
    rsi_result_t r;

    /* 1) RSI 버전 확인 */
    r = rsi_call(RSI_VERSION, 0, 0, 0);
    /* r.a1 = 지원 버전 (major.minor 인코딩) */

    /* 2) Realm IPA 설정 확인 (IPA 공간 크기 등) */
    r = rsi_call(RSI_REALM_CONFIG, /* config_addr_ipa */ 0, 0, 0);
    /*
     * config_addr_ipa가 가리키는 구조체에
     * ipa_width (Realm IPA 비트 폭) 등이 채워진다.
     */

    /*
     * 3) Shared Memory 영역 선언
     *    Normal World (KVM/virtio)와 통신할 버퍼를 RSI_IPA_STATE_SHARED로 설정.
     *    이 과정 없이 Normal World가 해당 IPA를 읽으면 GPF(Granule Protection Fault) 발생.
     *    자세한 내용은 §6.3 참조.
     */
    uint64_t shared_ipa_base = ZEPHYR_SHARED_MEM_IPA;   /* 예: 0x88000000 */
    uint64_t shared_ipa_size = ZEPHYR_SHARED_MEM_SIZE;   /* 예: 0x100000 (1MB) */
    r = rsi_call(RSI_IPA_STATE_SET,
                 shared_ipa_base,
                 shared_ipa_size,
                 RSI_IPA_STATE_SHARED);
    /* r.a0 == 0 이면 성공 */
}
```

#### 3.5.2 Zephyr 커널에 통합하는 방법

Zephyr `arch/arm64/core/prep_c.c`의 `z_prep_c()` 함수(C 진입점) 앞에서 `realm_boot_init()`을 호출하도록 추가한다:

```c
/* arch/arm64/core/prep_c.c (수정 예시) */
extern void realm_boot_init(void);   /* 신규 추가 */

void z_prep_c(void)
{
#ifdef CONFIG_ARM_CCA_REALM          /* 신규 Kconfig 옵션으로 조건부 컴파일 */
    realm_boot_init();
#endif
    bg_thread_main();
}
```

**신규 Kconfig 옵션 (`arch/arm64/Kconfig` 추가):**
```kconfig
config ARM_CCA_REALM
    bool "ARM CCA Realm VM support"
    depends on ARM64
    help
      Enable ARM CCA (Confidential Computing Architecture) Realm
      initialization via RSI (Realm Service Interface). Required when
      running Zephyr as a Realm VM under TF-RMM.
```

### 3.3 Realm VM 생성 (KVM/lkvm 사용)

ARM CCA를 지원하는 `kvmtool` 또는 `lkvm`을 사용:

```bash
# Normal World의 Linux (KVM Host)에서 실행
# kvmtool을 사용한 Realm VM 생성 (TF-RMM + KVM 지원 필요)

lkvm run \
    --realm \                         # Realm VM 플래그
    --kernel /path/to/zephyr.bin \   # Zephyr 이미지
    --mem 64M \                       # 메모리 크기
    --cpus 1 \
    --console serial \
    --name zephyr-realm-vecu
```

**RMI 흐름:**
```
lkvm (Normal-EL1)
  → ioctl(KVM_CREATE_VM, KVM_CAP_ARM_RME)    # KVM에 Realm 요청
  → KVM (Normal-EL2)
  → HVC → TF-RMM (Realm-EL2)               # RMI: RMI_REALM_CREATE
  → Realm-EL1에 Zephyr 진입
```

### 3.4 수정이 필요한 파일들

| 파일 | 수정 내용 |
|---|---|
| `scripts/config.sh` | Zephyr SDK 경로, Zephyr 이미지 경로 변수 추가 |
| `scripts/build.sh` | `zephyr` 빌드 타겟 추가 |
| `scripts/build-zephyr.sh` | (신규) Zephyr west 빌드 스크립트 |
| `scripts/bootfvp.sh` | Realm VM 자동 시작 명령 추가 (FVP 부팅 후 lkvm 실행) |

---

## 4. Normal World Hypervisor 추가

### 4.1 선택: Linux-KVM을 Normal-EL2 Hypervisor로 사용

현재 Normal-EL1에서 직접 부팅하는 Linux를 **KVM Host**로 사용한다. Linux 커널에서 `CONFIG_KVM=y`를 활성화하면 Linux가 Normal-EL2로 올라가며 Type-2 Hypervisor 역할을 한다.

**ARM64 KVM 동작 원리:**
```
Boot: TF-A → Linux 커널 (EL2에서 시작)
        └→ KVM 활성화 → EL2 스스로 Hypervisor 모드
        └→ Guest VM 생성 → EL1로 진입 (AAOS, Linux Guest)
```

### 4.2 Linux KVM 커널 설정 변경

`configs/linux.config`에 아래 추가:

```kconfig
# KVM Hypervisor (Normal-EL2)
CONFIG_VIRTUALIZATION=y
CONFIG_KVM=y
CONFIG_KVM_ARM_HOST=y

# VirtIO (Guest↔Host 통신)
CONFIG_VIRTIO=y
CONFIG_VIRTIO_PCI=y
CONFIG_VIRTIO_NET=y
CONFIG_VIRTIO_BLK=y
CONFIG_VIRTIO_CONSOLE=y

# 9P 파일시스템 공유 (Host↔Guest)
CONFIG_NET_9P=y
CONFIG_NET_9P_VIRTIO=y
CONFIG_9P_FS=y

# VFIO (PCI passthrough, 선택)
CONFIG_VFIO=y
CONFIG_VFIO_PCI=y
```

**TF-A 빌드 시 `ARM_LINUX_KERNEL_AS_BL33=1` 이미 설정되어 있어 Linux가 EL2에서 진입하므로 KVM 활성화에 별도 EL3 수정 불필요.**

### 4.3 Host 루트파일시스템에 필요한 패키지

`host-fs.ext4` 마운트 후 추가 (`scripts/mountfs.sh` 활용):

```bash
# KVM 도구
apt install -y qemu-system-arm kvmtool
# 또는 소스 빌드
git clone https://git.kernel.org/pub/scm/linux/kernel/git/will/kvmtool.git
cd kvmtool && make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
```

---

## 5. Normal VM: AGL 구동

### 5.1 AGL(Automotive Grade Linux)를 선택하는 이유

AGL은 Linux Foundation이 주도하는 차량용 오픈소스 Linux 배포판으로, **Yocto Project 기반**이다. AAOS 대비 아래와 같은 장점이 있어 FVP + KVM 연구 환경에 훨씬 적합하다.

| 항목 | AAOS (Android Automotive) | **AGL** |
|---|---|---|
| 기반 | AOSP (Android 프레임워크 전체) | Yocto / OpenEmbedded |
| 이미지 크기 | ~4 GB+ | **~300–500 MB** |
| FVP 부팅 시간 | 수십 분 이상 | **~1–2분** |
| ARM64 KVM 지원 | Cuttlefish 경유, 복잡 | **qemuarm64 BSP 공식 제공** |
| 차량 표준 스택 | 별도 AAOS HAL 구성 필요 | SOME/IP, DDS, VSS 기본 탑재 |
| 빌드 시스템 | repo + AOSP make | **bitbake (Yocto)** |
| V-ECU 역할 재현 | Android 앱 기반 | **아토믹한 systemd 서비스로 구성 가능** |

AGL은 `agl-ivi-demo` 또는 `agl-baseline` 이미지 타겟으로 인포테인먼트·클러스터 V-ECU 역할을 직접 수행할 수 있으며, SOME/IP(vsomeip), DDS(Cyclone DDS), COVESA VSS 등 차량 통신 스택이 기본 포함된다.

### 5.2 AGL 이미지 빌드

**빌드 머신 요건:** Ubuntu 22.04 또는 Debian 12, RAM 8GB+, 디스크 100GB+

```bash
# 1) repo 도구 설치
mkdir -p ~/.bin
curl https://storage.googleapis.com/git-repo-downloads/repo > ~/.bin/repo
chmod a+rx ~/.bin/repo
export PATH="${HOME}/.bin:${PATH}"

# 2) AGL 소스 동기화 (Porcupine 또는 최신 릴리스 브랜치)
mkdir agl-workspace && cd agl-workspace
repo init -u https://gerrit.automotivelinux.org/gerrit/AGL/AGL-repo \
          -b porcupine   # 또는 최신 릴리스 태그
repo sync -j$(nproc)

# 3) 빌드 환경 초기화
source meta-agl/scripts/aglsetup.sh \
    -m qemuarm64 \          # ARM64 QEMU 타겟 (KVM Guest와 동일한 BSP)
    -b build-agl-arm64 \
    agl-demo \              # IVI 데모 이미지 (인포테인먼트 V-ECU 역할)
    agl-devel \             # 개발 도구 포함
    agl-netboot             # 네트워크 부팅 지원

# 4) bitbake 빌드
bitbake agl-demo-platform

# 결과물 위치: build-agl-arm64/tmp/deploy/images/qemuarm64/
#   - agl-demo-platform-qemuarm64.ext4     ← rootfs (VM 디스크 이미지)
#   - Image                                ← Linux 커널 (ARM64)
#   - agl-demo-platform-qemuarm64.qemuboot.conf
```

**경량 이미지 타겟 비교:**

| bitbake 타겟 | 크기 | 포함 내용 | 권장 용도 |
|---|---|---|---|
| `agl-image-minimal` | ~80 MB | 최소 부팅만 | 통신 오버헤드 측정 baseline |
| `agl-image-weston` | ~200 MB | Wayland/Weston UI | 기본 IVI 검증 |
| `agl-demo-platform` | ~400 MB | vsomeip, DDS, 데모 앱 | **V-ECU 역할 재현 권장** |

### 5.3 AGL VM 실행 (KVM Host에서)

```bash
# QEMU-KVM으로 AGL Guest VM 실행 (Normal-EL1 Guest)
AGL_IMG=/path/to/agl-demo-platform-qemuarm64.ext4
AGL_KERNEL=/path/to/Image    # AGL 빌드 결과물의 ARM64 커널

qemu-system-aarch64 \
    -M virt,virtualization=on,gic-version=3 \
    -cpu host \
    -enable-kvm \
    -m 1G \
    -smp 2 \
    -kernel ${AGL_KERNEL} \
    -append "root=/dev/vda rw console=ttyAMA0 systemd.unified_cgroup_hierarchy=0" \
    -drive file=${AGL_IMG},if=virtio,format=raw \
    -netdev type=tap,id=net0,ifname=tap_agl \
    -device virtio-net-pci,netdev=net0,mac=52:54:00:aa:bb:01 \
    -serial stdio \
    -name agl-vecu &
```

**주의사항:**
- FVP 위에서 KVM Nested Virtualization이 필요 → `bootfvp.sh`에 `cluster0.has_nested_virt=1` 추가
- AGL은 systemd 기반이므로 `console=ttyAMA0` 없이 부팅하면 로그 확인 불가
- `agl-image-minimal` 타겟은 vsomeip/DDS 미포함 → 통신 측정 시 `agl-demo-platform` 사용

### 5.4 AGL에서 V-ECU 역할 서비스 구성

AGL의 systemd 서비스로 V-ECU 통신 벤치마크 프로그램을 자동 시작하도록 설정:

```ini
# /etc/systemd/system/vecu-comm.service (AGL rootfs 내)
[Unit]
Description=V-ECU Communication Benchmark
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/bin/vecu_comm --mode server --port 8888
Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target
```

**vsomeip 기반 V-ECU 통신 설정 (SOME/IP, 선택):**
```json
// /etc/vsomeip/vecu-someip.json
{
    "unicast": "10.0.1.10",
    "logging": { "level": "warning" },
    "applications": [{ "name": "vecu-agl", "id": "0x1234" }],
    "services": [{
        "service": "0x1111",
        "instance": "0x0001",
        "unreliable": "30509"
    }]
}
```

### 5.5 수정이 필요한 파일들

| 파일 | 수정 내용 |
|---|---|
| `scripts/config.sh` | `AGL_IMG`, `AGL_KERNEL` 경로 변수 추가 |
| `scripts/bootfvp.sh` | FVP 파라미터에 `cluster0.has_nested_virt=1` 추가 |
| `scripts/build-agl.sh` | (신규) AGL repo sync + bitbake 래퍼 스크립트 |
| `scripts/mountfs.sh` | AGL 이미지를 host-fs에 복사하는 로직 추가 |

---

## 6. Normal VM: Linux 구동

### 6.1 Guest Linux 이미지 준비

기존 SCRUTINIZER에서 빌드되는 Linux 커널(`src/linux/arch/arm64/boot/Image`)을 Guest VM 커널로 재사용 가능. 루트파일시스템은 별도 경량 이미지(buildroot, Alpine Linux ARM64) 사용 권장.

**buildroot로 경량 Guest Linux 루트파일시스템 빌드:**
```bash
make qemu_aarch64_virt_defconfig
make menuconfig
# → Target → AArch64 little endian
# → Filesystem → ext4
# → System → 네트워킹 도구 활성화
make -j$(nproc)
# 결과: output/images/rootfs.ext4
```

### 6.2 Linux VM 실행 (KVM Host에서)

```bash
# QEMU-KVM으로 Linux Guest VM 실행
qemu-system-aarch64 \
    -M virt,virtualization=on,gic-version=3 \
    -cpu host \
    -enable-kvm \
    -m 1G \
    -smp 1 \
    -kernel /path/to/guest-linux/Image \
    -append "root=/dev/vda rw console=ttyAMA0 ip=dhcp" \
    -drive file=/path/to/guest-rootfs.ext4,if=virtio,format=raw \
    -netdev type=tap,id=net1,ifname=tap_linux \
    -device virtio-net-pci,netdev=net1 \
    -serial stdio \
    -name linux-vecu &
```

### 6.3 VM 간 네트워킹 — ⚠️ Realm↔Normal 통신의 메모리 격리 문제 (구현 난관 #3)

#### 문제: Realm 메모리는 암호화되어 Normal World가 접근 불가

ARM CCA에서 Realm VM의 모든 메모리는 **하드웨어 수준에서 암호화(MECID 기반)** 되어 있어,
Normal World(KVM Host, AAOS VM, Linux VM)에서 Realm의 IPA를 직접 읽으면
**GPF(Granule Protection Fault)** 가 발생하고 접근이 차단된다.

따라서 **Virtio-net, Virtio-console 등 virtio 기반 통신은 NS Shared Memory 영역을 명시적으로 선언해야만 동작**한다.

#### 해결: NS Shared Memory Delegation

통신 버퍼로 사용할 IPA 영역을 `RSI_IPA_STATE_SET(..., RSI_IPA_STATE_SHARED)`으로 선언하면,
해당 물리 메모리 범위가 TF-RMM에 의해 **Non-Secure Shared** 상태로 전환되고
Normal World에서 읽기/쓰기가 허용된다.

```
Realm VM (Zephyr)          TF-RMM              KVM Host (Normal)
     │                        │                       │
     │  RSI_IPA_STATE_SET     │                       │
     │  (buf_ipa, size,       │                       │
     │   SHARED) ────────────▶│                       │
     │                        │ Granule 상태           │
     │                        │ → NS Shared           │
     │◀─────────── OK ────────│                       │
     │                        │                       │
     │  virtio ring write     │                       │
     │  (buf에 데이터 씀) ──────────────────────────▶  │
     │                        │  (GPF 없이 정상 접근)  │
```

#### 구현 단계

**Step 1 — Zephyr 부팅 시 Shared Buffer IPA 선언 (§3.5 코드 활용):**

```c
/* realm_boot_init() 내부 (§3.5.1 코드의 Step 3) */

/* Virtio 링 버퍼 + 디스크립터 테이블이 위치할 영역을 Shared로 선언 */
#define VIRTIO_SHARED_IPA_BASE  0x88000000ULL   /* lkvm이 Virtio ring을 배치할 IPA */
#define VIRTIO_SHARED_IPA_SIZE  0x00200000ULL   /* 2MB */

rsi_result_t r = rsi_call(RSI_IPA_STATE_SET,
                           VIRTIO_SHARED_IPA_BASE,
                           VIRTIO_SHARED_IPA_SIZE,
                           RSI_IPA_STATE_SHARED);
if (r.a0 != 0) {
    /* 실패 시 Realm 부팅 중단 또는 fallback */
    panic("RSI_IPA_STATE_SET failed: %lx\n", r.a0);
}
```

**Step 2 — lkvm(KVM Host)에서 Virtio 백엔드를 해당 IPA에 배치:**

lkvm의 `--virtio-transport=mmio` 옵션과 함께 Virtio MMIO 주소를 Step 1에서 선언한 IPA 범위 내에 지정한다.

```bash
lkvm run \
    --realm \
    --kernel /path/to/zephyr.bin \
    --mem 64M \
    --cpus 1 \
    --network mode=tap,tapif=tap_realm \
    --virtio-transport=mmio \
    --virtio-mmio-base=0x88000000 \   # Step 1의 VIRTIO_SHARED_IPA_BASE와 일치
    --name zephyr-realm-vecu
```

**Step 3 — Zephyr virtio-net 드라이버 설정:**

Zephyr의 virtio-net 드라이버는 `CONFIG_VIRTIO_MMIO=y` + 디바이스트리(또는 정적 설정)로 위 MMIO 주소를 가리켜야 한다.

```kconfig
# prj.conf 추가
CONFIG_VIRTIO=y
CONFIG_VIRTIO_MMIO=y
CONFIG_NET_L2_ETHERNET=y
CONFIG_NET_TCP=y
```

#### Normal VM 간 브리지 구성 (AAOS ↔ Linux ↔ Realm)

```bash
# KVM Host에서 Linux 브리지 구성
ip link add br_vecu type bridge
ip link set br_vecu up
ip addr add 10.0.1.1/24 dev br_vecu

# Normal VM (AAOS, Linux) TAP 인터페이스 → 브리지 연결
ip tuntap add tap_aaos mode tap
ip tuntap add tap_linux mode tap
ip link set tap_aaos master br_vecu
ip link set tap_linux master br_vecu
ip link set tap_aaos up
ip link set tap_linux up

# Realm VM (Zephyr) TAP 인터페이스 → 동일 브리지 연결
# (Virtio-net 백엔드가 tap_realm을 사용하도록 lkvm --network에 지정)
ip tuntap add tap_realm mode tap
ip link set tap_realm master br_vecu
ip link set tap_realm up

# VM 내부 IP 할당 계획
# KVM Host  (br_vecu): 10.0.1.1
# AAOS VM   (tap_aaos): 10.0.1.10
# Linux VM  (tap_linux): 10.0.1.20
# Zephyr    (tap_realm, Shared Mem virtio): 10.0.1.30
```

> **주의:** Shared Memory 선언 없이 위 브리지를 구성하면 tap_realm으로 나가는 패킷은
> Zephyr 측 virtio 링 버퍼(Realm 메모리)에 접근하지 못해 통신이 불가능하다.
> §3.5의 `RSI_IPA_STATE_SET` 호출이 선행되어야 한다.

---

## 7. Attestation 성능 측정

### 7.0 FVP 환경에서의 측정 전략: 절대값 + 상대값 병행

FVP는 Cycle-accurate 모델이 아니므로 `clock_gettime` 측정값에는 **호스트 PC 성능 + 시뮬레이션 오버헤드**가 혼재된다. 그러나 이는 측정을 포기할 이유가 아니라 **두 가지 측정을 병행하여 각각의 의미를 명확히 구분해서 보고**할 이유다.

#### 측정 전략 요약

| 측정 종류 | 방법 | 의미 | 논문 활용 |
|---|---|---|---|
| **① 절대 clock 측정** | `clock_gettime(CLOCK_MONOTONIC_RAW)` | FVP 환경에서의 실제 소요 시간 (시뮬레이션 포함) | 구현 동작 검증, 단계별 소요 시간 분포 |
| **② 상대 오버헤드 비율** | (실험군 − 베이스라인) / 베이스라인 × 100% | 보안 기능이 추가한 순수 오버헤드 | FVP 한계를 배제한 아키텍처 주장 근거 |
| **③ 단계별 구성 비율** | 생성:전송:검증 각각의 비율 | 병목 지점 분석 | 최적화 방향 제시 |

**두 가지 모두 수집하고, 논문에서 구분해서 기술하면 된다:**

```
절대값 → "FVP 시뮬레이션 환경에서 Attestation 전체 사이클은 평균 X ms였으며,
          이는 FVP 특성상 실 하드웨어 수치와 다를 수 있다."

상대값 → "Attestation 비활성화 대비 활성화 시 V-ECU 통신 latency는 Y% 증가했으며,
          이 비율은 FVP 시뮬레이션 오버헤드에 독립적이다."
```

#### 권장 측정 페어 (베이스라인 + 실험군)

```
베이스라인 (B):  Attestation 없이 V-ECU 간 메시지 전송 (순수 통신 latency)
실험군 A:        Attestation ON, TLS 없이 (Attestation overhead만 분리)
실험군 B:        Attestation ON + TLS handshake (실제 운용 시나리오)

→ 절대값: B, A, B의 clock 수치 모두 수집
→ 상대값: (A − B) / B × 100%,  (B − B) / B × 100%
```

#### 논문 기술 예시 문구

```
"All experiments were conducted on ARM FVP (Base RevC-2xAEMvA),
a functional simulator rather than a cycle-accurate model.
We report both absolute timing values (to characterize end-to-end
latency within the simulation environment) and relative overhead
ratios (to isolate the cost of security mechanisms independently
of simulation fidelity)."
```

#### 측정 품질 높이는 설정

- **타이머:** `CLOCK_MONOTONIC_RAW` — NTP/adjtime 조정 영향 없음 (FVP 내부 virt-timer와 무관하게 host monotonic clock 사용)
- **Warmup:** 최소 10회 실행 후 측정 시작 (JIT 효과, cache cold-start 배제)
- **반복 횟수:** N ≥ 50 (통계적 유의성 확보)
- **FVP 부하:** 측정 중 Host PC 다른 작업 최소화
- **이상치 처리:** IQR 기반 필터링 (1.5×IQR 초과 제거) 또는 P5~P95 구간 보고

```c
/* 모든 측정에 공통 적용하는 타이머 래퍼 */
#include <time.h>
#include <stdint.h>

static inline int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

/* 사용 예: */
int64_t t0 = now_us();
/* ... 측정 대상 코드 ... */
int64_t elapsed = now_us() - t0;
printf("ELAPSED: %lld us\n", (long long)elapsed);
```

#### 자동화 측정 + 상대/절대값 동시 수집 스크립트 구조

```python
# scripts/analyze_perf.py  (공통 분석기)
import csv, sys, statistics

def analyze(label, data_baseline, data_exp):
    """절대값과 상대 오버헤드를 함께 출력"""
    n = len(data_exp)
    mean_b = statistics.mean(data_baseline)
    mean_e = statistics.mean(data_exp)
    overhead_pct = (mean_e - mean_b) / mean_b * 100

    print(f"\n=== {label} ===")
    print(f"  [절대값] Baseline : mean={mean_b:.1f} us, "
          f"stdev={statistics.stdev(data_baseline):.1f} us")
    print(f"  [절대값] Exp      : mean={mean_e:.1f} us, "
          f"stdev={statistics.stdev(data_exp):.1f} us")
    print(f"  [상대값] Overhead : {overhead_pct:+.2f}%")
    print(f"  [분포]   P50={sorted(data_exp)[n//2]} "
          f"P95={sorted(data_exp)[int(n*0.95)]} "
          f"P99={sorted(data_exp)[int(n*0.99)]} us")
```

---

### 7.1 Attestation 개요

ARM CCA에서 각 Realm VM은 **CCA Attestation Token** (CCA = Confidential Computing Architecture)을 가진다. 이 토큰은 Realm의 무결성을 증명하며, COSE_Sign1 구조로 인코딩된 EAT(Entity Attestation Token)이다.

**측정 대상 흐름:**
```
V-ECU (Realm/VM)                    Verifier (예: Linux VM 또는 외부)
     │                                      │
     │──(1) 토큰 생성 (RSI_ATTEST_TOKEN)──▶│
     │                                      │
     │──(2) 토큰 전송 (네트워크/채널) ─────▶│
     │                                      │
     │                (3) 검증 처리 ────────│
```

### 7.2 (1) 토큰 생성 시간 측정

**Realm VM (Zephyr) 내부에서 측정:**

Zephyr에서 RSI(Realm Service Interface) 호출로 Attestation Token을 요청한다.

```c
/* Zephyr 내부 측정 코드 (vecu_attest.c) */
#include <zephyr/kernel.h>

/* ARM RSI Attestation Token 요청 */
#define RSI_ATTEST_TOKEN_INIT   0xC4000194
#define RSI_ATTEST_TOKEN_CONTINUE 0xC4000195

static void measure_token_generation(void) {
    uint64_t challenge[8] = { /* 64-byte nonce */ };
    uint8_t token_buf[4096];
    uint64_t granule_pa = /* token_buf의 physical address */;

    /* 시작 타임스탬프 */
    uint64_t t_start = k_cycle_get_64();

    /* RSI 호출로 토큰 요청 */
    arm_smccc_res_t res;
    arm_smccc_hvc(RSI_ATTEST_TOKEN_INIT,
                  granule_pa, (uint64_t)challenge, 64,
                  0, 0, 0, 0, &res);

    /* 완료까지 반복 (토큰이 클 수 있어 여러 청크) */
    while (res.a0 == RSI_INCOMPLETE) {
        arm_smccc_hvc(RSI_ATTEST_TOKEN_CONTINUE,
                      granule_pa, 0, 0,
                      0, 0, 0, 0, &res);
    }

    uint64_t t_end = k_cycle_get_64();
    uint64_t elapsed_cycles = t_end - t_start;
    uint64_t elapsed_us = k_cyc_to_us_floor64(elapsed_cycles);

    printk("ATTEST_TOKEN_GEN: %llu us\n", elapsed_us);
}
```

**측정 항목:**
- `RSI_ATTEST_TOKEN_INIT` ~ 마지막 `RSI_ATTEST_TOKEN_CONTINUE` 완료까지의 사이클 수
- Zephyr: `k_cycle_get_64()` 또는 ARM `CNTVCT_EL0` 레지스터 직접 읽기
- Linux/AGL 측 Verifier: `clock_gettime(CLOCK_MONOTONIC_RAW)` 사용 (§7.0의 `now_us()`)

**Zephyr 내부 측정 코드에 `CNTVCT_EL0` 직접 읽기 추가:**
```c
/* ARM Generic Timer — FVP에서도 동작하는 가장 정밀한 타이머 */
static inline uint64_t read_cntvct(void) {
    uint64_t val;
    asm volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}

static inline uint64_t read_cntfrq(void) {
    uint64_t val;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(val));
    return val;  /* FVP 기본값: 100MHz (10ns/tick) */
}

static void measure_token_generation(void) {
    /* ... RSI 호출 코드 (위와 동일) ... */

    uint64_t t_start_cyc = read_cntvct();
    /* RSI_ATTEST_TOKEN_INIT / CONTINUE 루프 */
    uint64_t t_end_cyc = read_cntvct();

    uint64_t freq = read_cntfrq();
    uint64_t elapsed_us = (t_end_cyc - t_start_cyc) * 1000000ULL / freq;

    /* 절대값 출력 */
    printk("ATTEST_TOKEN_GEN_ABS: %llu us (cycles=%llu freq=%llu)\n",
           elapsed_us, t_end_cyc - t_start_cyc, freq);
}
```

**반복 측정 스크립트 (Host에서) — 절대값 수집:**
```bash
#!/bin/bash
# scripts/measure_attest_gen.sh
N=100
LOG=debug/logs/attest_gen_$(date +%Y%m%d_%H%M%S).csv
echo "iteration,gen_us" > $LOG

for i in $(seq 1 $N); do
    val=$(grep "ATTEST_TOKEN_GEN_ABS" /dev/ttyZephyr | head -1 \
          | awk -F'[: ]+' '{print $2}')
    echo "$i,$val" >> $LOG
done

python3 -c "
import csv, statistics
data = [int(r['gen_us']) for r in csv.DictReader(open('$LOG'))]
print(f'N={len(data)}')
print(f'[절대값] Mean={statistics.mean(data):.1f} us  Stdev={statistics.stdev(data):.1f} us')
print(f'[절대값] Min={min(data)} us  Max={max(data)} us')
print(f'[분포]   P50={sorted(data)[len(data)//2]}  P95={sorted(data)[int(len(data)*0.95)]}')
"
```

### 7.3 (2) 토큰 전송 시간 측정 (V-ECU → Verifier)

**전송 채널 옵션:**

| 채널 | 설명 | 측정 방법 |
|---|---|---|
| Virtio-net | VM 간 가상 NIC | `now_us()` + 소켓 send/recv |
| Shared Memory | IVSHMEM / 직접 매핑 | mmap + 폴링 또는 doorbell |
| VSOCK | VM↔Host 소켓 | `AF_VSOCK` 소켓 |

**TCP 소켓 기반 측정 코드 (`CLOCK_MONOTONIC_RAW` 사용):**

```c
/* vecu_token_send.c (V-ECU 측 - Sender) */
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* §7.0의 공통 래퍼 */
static inline int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

#define VERIFIER_IP   "10.0.1.20"   /* Verifier VM (Linux) IP */
#define VERIFIER_PORT 7777

void send_attestation_token(uint8_t *token, size_t token_len) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(VERIFIER_PORT),
    };
    inet_pton(AF_INET, VERIFIER_IP, &addr.sin_addr);
    connect(sock, (struct sockaddr*)&addr, sizeof(addr));

    /* 절대값 측정 시작 */
    int64_t t0 = now_us();

    uint32_t len_net = htonl((uint32_t)token_len);
    send(sock, &len_net, sizeof(len_net), 0);
    send(sock, token, token_len, MSG_WAITALL);

    /* ACK 수신 (Verifier가 완전 수신 후 회신) */
    uint8_t ack;
    recv(sock, &ack, 1, MSG_WAITALL);

    int64_t elapsed = now_us() - t0;

    /* 절대값 출력 — 베이스라인·실험군 모두 동일 형식으로 기록 */
    fprintf(stdout, "ATTEST_TOKEN_SEND_ABS: %lld us (size=%zu bytes)\n",
            (long long)elapsed, token_len);

    close(sock);
}
```

```c
/* vecu_verifier_recv.c (Verifier 측 - Receiver) */
static inline int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

void recv_and_ack(int client_sock) {
    uint32_t len_net;
    recv(client_sock, &len_net, sizeof(len_net), MSG_WAITALL);
    uint32_t token_len = ntohl(len_net);

    int64_t t0 = now_us();
    uint8_t *token_buf = malloc(token_len);
    recv(client_sock, token_buf, token_len, MSG_WAITALL);
    int64_t recv_us = now_us() - t0;

    uint8_t ack = 1;
    send(client_sock, &ack, 1, 0);

    fprintf(stdout, "ATTEST_TOKEN_RECV_ABS: %lld us (size=%u bytes)\n",
            (long long)recv_us, token_len);

    verify_token(token_buf, token_len);
    free(token_buf);
}
```

### 7.4 (3) 검증 시간 측정

검증은 **Verifier VM (Linux VM)** 에서 수행한다. ARM CCA 레퍼런스 구현인 `veraison`을 사용하거나, `libveraison-apiclient`를 직접 호출한다.

**로컬 검증 (오프라인, 테스트용) — `CLOCK_MONOTONIC_RAW` 적용:**

```c
/* token_verify.c */
#include <time.h>

/* RATS(Remote ATtestation procedureS) 라이브러리 사용 예시 */
/* https://github.com/veraison/c-apiclient */

static inline int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

int verify_cca_token(uint8_t *token, size_t len) {
    int64_t t_total = now_us();

    /* 1) CBOR 파싱 */
    int64_t t0 = now_us();
    cca_token_t parsed;
    int ret = cca_token_parse(token, len, &parsed);
    int64_t parse_us = now_us() - t0;
    if (ret != 0) return -1;

    /* 2) 서명 검증 (COSE_Sign1) */
    t0 = now_us();
    ret = cca_token_verify_signature(&parsed, /* public_key */ NULL);
    int64_t sig_us = now_us() - t0;

    /* 3) Claim 검증 (nonce, platform state 등) */
    t0 = now_us();
    ret = cca_token_verify_claims(&parsed);
    int64_t claim_us = now_us() - t0;

    int64_t total_us = now_us() - t_total;

    /* 절대값 — 단계별 세분화 출력 */
    fprintf(stdout,
        "ATTEST_VERIFY_ABS: total=%lld us "
        "(parse=%lld sig=%lld claim=%lld)\n",
        (long long)total_us,
        (long long)parse_us,
        (long long)sig_us,
        (long long)claim_us);

    return ret;
}
```

**Veraison 서버를 통한 원격 검증 (REST API):**
```bash
# Verifier VM에서 Veraison 서버 실행
docker run -p 8080:8080 ghcr.io/veraison/services:latest

# 검증 요청 (토큰을 Base64로 인코딩하여 POST)
TOKEN_B64=$(base64 -w0 /tmp/cca_token.cbor)
time curl -X POST http://localhost:8080/challenge-response/v1/newSession \
    -H "Content-Type: application/vnd.parallax.cca-realm-provisioning-token+cbor" \
    -d "$TOKEN_B64"
```

### 7.5 Attestation 측정 자동화 스크립트

베이스라인(Attestation OFF)과 실험군(Attestation ON)을 **같은 N으로 수집**하여 절대값 + 상대 오버헤드를 동시에 보고한다.

```bash
#!/bin/bash
# scripts/measure_attestation.sh
# 절대값(baseline/exp 모두) + 상대 오버헤드 자동 수집

ITERATIONS=50
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOG_BASE="debug/logs/attest_baseline_${TIMESTAMP}.csv"
LOG_EXP="debug/logs/attest_exp_${TIMESTAMP}.csv"

echo "iteration,gen_us,send_us,verify_us,total_us" > $LOG_BASE
echo "iteration,gen_us,send_us,verify_us,total_us" > $LOG_EXP

collect_one() {
    local logfile=$1
    local mode=$2   # "baseline" or "attestation"
    for i in $(seq 1 $ITERATIONS); do
        # 각 VM에서 최신 측정값 수집 (측정 프로그램이 파일에 기록)
        GEN=$(ssh root@10.0.1.30   "cat /tmp/attest_gen_latest.txt")    # Zephyr (Realm)
        SEND=$(ssh root@10.0.1.10  "cat /tmp/attest_send_latest.txt")   # AGL V-ECU
        VERIFY=$(ssh root@10.0.1.20 "cat /tmp/attest_verify_latest.txt") # Linux Verifier
        TOTAL=$((GEN + SEND + VERIFY))
        echo "$i,$GEN,$SEND,$VERIFY,$TOTAL" >> $logfile
        echo "[$mode $i/$ITERATIONS] gen=${GEN} send=${SEND} verify=${VERIFY} total=${TOTAL} (us)"
    done
}

echo "=== Phase 1: Baseline (Attestation OFF) ==="
ssh root@10.0.1.10 "vecu_comm --disable-attestation &"
collect_one $LOG_BASE "baseline"

echo ""
echo "=== Phase 2: Experiment (Attestation ON) ==="
ssh root@10.0.1.10 "vecu_comm --enable-attestation &"
collect_one $LOG_EXP "exp"

echo ""
echo "=== Analysis ==="
python3 scripts/analyze_attest.py $LOG_BASE $LOG_EXP
```

```python
# scripts/analyze_attest.py
# 절대값 + 상대 오버헤드 병행 분석
import csv, sys, statistics

def load(path):
    with open(path) as f:
        return list(csv.DictReader(f))

def stats(data, col):
    vals = [int(r[col]) for r in data]
    s = sorted(vals)
    n = len(vals)
    return {
        'n': n,
        'mean': statistics.mean(vals),
        'stdev': statistics.stdev(vals),
        'min': min(vals),
        'p50': s[n//2],
        'p95': s[int(n*0.95)],
        'p99': s[int(n*0.99)],
        'max': max(vals),
    }

baseline = load(sys.argv[1])
exp      = load(sys.argv[2])

print("\n" + "="*65)
print("ATTESTATION PERFORMANCE — ABSOLUTE + RELATIVE OVERHEAD")
print("="*65)

for col, label in [('gen_us',    'Token Generation'),
                   ('send_us',   'Token Transfer  '),
                   ('verify_us', 'Verification    '),
                   ('total_us',  'TOTAL           ')]:
    b = stats(baseline, col)
    e = stats(exp,      col)
    ovh = (e['mean'] - b['mean']) / b['mean'] * 100 if b['mean'] else 0

    print(f"\n[{label}]")
    print(f"  Baseline  : mean={b['mean']:8.1f} us  stdev={b['stdev']:6.1f}  "
          f"P50={b['p50']}  P95={b['p95']}  P99={b['p99']}")
    print(f"  Experiment: mean={e['mean']:8.1f} us  stdev={e['stdev']:6.1f}  "
          f"P50={e['p50']}  P95={e['p95']}  P99={e['p99']}")
    print(f"  ─────────────────────────────────────────────────────────")
    print(f"  Overhead  : {ovh:+.2f}%  "
          f"(delta mean = {e['mean']-b['mean']:+.1f} us)")

# 단계별 구성 비율 (실험군 기준)
total_mean = statistics.mean([int(r['total_us']) for r in exp])
print(f"\n[단계별 구성 비율 (Attestation ON 기준)]")
for col, label in [('gen_us','Generation'),('send_us','Transfer'),('verify_us','Verify')]:
    m = statistics.mean([int(r[col]) for r in exp])
    print(f"  {label:12s}: {m/total_mean*100:5.1f}%  ({m:.1f} us)")
```

---

## 8. V-ECU 간 통신 성능 측정

### 8.1 측정 항목 요약

| 항목 | 측정 도구 | 단위 |
|---|---|---|
| Handshake Overhead | `clock_gettime` | μs |
| CPU/MEM Overhead (프로세스 전체) | `time -v` (GNU time) | % / KB |
| 메시지 당 Latency | `clock_gettime` (RTT/2) | μs |
| 메시지 당 CPU Overhead | `/proc/self/stat` | jiffies/msg |

### 8.2 Handshake Overhead 측정

V-ECU 간 TLS handshake (또는 DTLS) 오버헤드를 측정한다.

**TLS Handshake 측정 코드:**
```c
/* measure_handshake.c */
#include <time.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

void measure_tls_handshake(const char *server_ip, int port) {
    struct timespec t_start, t_end;
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());

    /* mTLS: 클라이언트 인증서 설정 (V-ECU Identity) */
    SSL_CTX_use_certificate_file(ctx, "/etc/vecu/cert.pem", SSL_FILETYPE_PEM);
    SSL_CTX_use_PrivateKey_file(ctx, "/etc/vecu/key.pem", SSL_FILETYPE_PEM);
    SSL_CTX_load_verify_locations(ctx, "/etc/vecu/ca.pem", NULL);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);

    int sock = /* TCP connect to server_ip:port */;
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, sock);

    clock_gettime(CLOCK_MONOTONIC_RAW, &t_start);
    int ret = SSL_connect(ssl);   /* Handshake 시작 ~ 완료 */
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_end);

    if (ret != 1) {
        fprintf(stderr, "TLS handshake failed\n");
        return;
    }

    long elapsed_us = (t_end.tv_sec - t_start.tv_sec) * 1000000L
                    + (t_end.tv_nsec - t_start.tv_nsec) / 1000L;

    fprintf(stdout, "TLS_HANDSHAKE: %ld us\n", elapsed_us);

    /* 사용 cipher suite 출력 */
    fprintf(stdout, "Cipher: %s\n", SSL_get_cipher(ssl));

    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(sock);
}
```

**연속 측정 (재연결 없이 Session Resumption 비교):**
```bash
# 신규 핸드쉐이크 vs Session Resumption 비교
openssl s_time -connect 10.0.1.10:443 -new    -time 10  # 신규
openssl s_time -connect 10.0.1.10:443 -reuse  -time 10  # 재사용
```

### 8.3 CPU/MEM Overhead 측정 (`time -v`)

GNU `time -v`는 프로세스 전체의 CPU 시간, 메모리 사용량, context switch 등을 측정한다.

**V-ECU 통신 프로그램 전체 측정:**
```bash
# GNU time 설치
apt install -y time

# V-ECU 통신 프로그램을 time -v로 래핑
/usr/bin/time -v ./vecu_comm \
    --mode client \
    --server 10.0.1.10 \
    --port 8888 \
    --messages 1000 \
    --msg-size 256 \
    2>&1 | tee debug/logs/vecu_comm_time_v.log
```

**`time -v` 출력 중 핵심 항목:**
```
Command being timed: "./vecu_comm ..."
User time (seconds): 0.12          ← 유저 공간 CPU 시간
System time (seconds): 0.03        ← 커널 공간 CPU 시간
Percent of CPU this job got: 15%   ← CPU 점유율
Maximum resident set size (kbytes): 4096  ← 최대 메모리 사용량 (KB)
Major (requiring I/O) page faults: 0
Minor (reclaiming a frame) page faults: 128
Voluntary context switches: 1002   ← 자발적 컨텍스트 스위치
Involuntary context switches: 47   ← 비자발적 컨텍스트 스위치
```

**자동화 스크립트:**
```bash
#!/bin/bash
# scripts/measure_vecu_comm.sh

MSG_SIZES=(64 256 1024 4096)    # 메시지 크기 변화
ITERATIONS=5

for SIZE in "${MSG_SIZES[@]}"; do
    echo "=== Message Size: ${SIZE} bytes ==="
    for i in $(seq 1 $ITERATIONS); do
        /usr/bin/time -v ./vecu_comm \
            --messages 100 --msg-size $SIZE \
            2>&1 | grep -E "User time|System time|CPU this job|Maximum resident"
    done
    echo ""
done
```

### 8.4 메시지 당 Latency 측정

**RTT 기반 단방향 Latency (RTT/2):**

```c
/* vecu_latency.c */
#include <time.h>
#include <stdint.h>

#define WARMUP_ROUNDS   10
#define MEASURE_ROUNDS  1000

void measure_message_latency(int sock) {
    uint8_t msg[256];
    uint8_t echo[256];
    long rtt_us[MEASURE_ROUNDS];

    /* Warmup (cache/connection 안정화) */
    for (int i = 0; i < WARMUP_ROUNDS; i++) {
        send(sock, msg, sizeof(msg), 0);
        recv(sock, echo, sizeof(echo), MSG_WAITALL);
    }

    /* 측정: CLOCK_MONOTONIC_RAW 사용 */
    for (int i = 0; i < MEASURE_ROUNDS; i++) {
        struct timespec ts_send, ts_recv;
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts_send);
        send(sock, msg, sizeof(msg), 0);
        recv(sock, echo, sizeof(echo), MSG_WAITALL);
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts_recv);

        rtt_us[i] = (ts_recv.tv_sec - ts_send.tv_sec) * 1000000L
                  + (ts_recv.tv_nsec - ts_send.tv_nsec) / 1000L;
    }

    /* 통계 출력 */
    long sum = 0;
    long min_val = rtt_us[0], max_val = rtt_us[0];
    for (int i = 0; i < MEASURE_ROUNDS; i++) {
        sum += rtt_us[i];
        if (rtt_us[i] < min_val) min_val = rtt_us[i];
        if (rtt_us[i] > max_val) max_val = rtt_us[i];
    }

    fprintf(stdout, "MSG_LATENCY (RTT/2):\n");
    fprintf(stdout, "  N=%d, Size=%zu bytes\n", MEASURE_ROUNDS, sizeof(msg));
    fprintf(stdout, "  Mean RTT : %ld us → One-way: %ld us\n", sum/MEASURE_ROUNDS, (sum/MEASURE_ROUNDS)/2);
    fprintf(stdout, "  Min RTT  : %ld us\n", min_val);
    fprintf(stdout, "  Max RTT  : %ld us\n", max_val);
}
```

### 8.5 메시지 당 CPU Overhead 측정

```c
/* vecu_cpu_overhead.c */
#include <stdio.h>
#include <stdint.h>

/* /proc/self/stat에서 CPU 시간 읽기 */
typedef struct {
    unsigned long utime;  /* user mode jiffies */
    unsigned long stime;  /* kernel mode jiffies */
} cpu_stat_t;

static cpu_stat_t read_cpu_stat(void) {
    cpu_stat_t s = {0};
    FILE *f = fopen("/proc/self/stat", "r");
    if (!f) return s;
    /* 필드 순서: pid comm state ppid ... utime stime ... */
    fscanf(f, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
           &s.utime, &s.stime);
    fclose(f);
    return s;
}

void measure_cpu_per_message(int sock, int n_messages, size_t msg_size) {
    uint8_t *msg = calloc(1, msg_size);
    uint8_t *buf = calloc(1, msg_size);

    cpu_stat_t before = read_cpu_stat();

    for (int i = 0; i < n_messages; i++) {
        send(sock, msg, msg_size, 0);
        recv(sock, buf, msg_size, MSG_WAITALL);
    }

    cpu_stat_t after = read_cpu_stat();

    unsigned long cpu_jiffies = (after.utime + after.stime)
                              - (before.utime + before.stime);
    /* 1 jiffy = 10ms (HZ=100) 또는 4ms (HZ=250) */
    double cpu_ms_per_msg = (double)cpu_jiffies * 10.0 / n_messages;

    fprintf(stdout, "CPU_PER_MSG: %.4f ms/msg (%d msgs, size=%zu)\n",
            cpu_ms_per_msg, n_messages, msg_size);

    free(msg);
    free(buf);
}
```

### 8.6 통합 측정 스크립트 (절대값 + 상대 오버헤드 병행)

베이스라인(보안 없음)과 실험군(TLS + Attestation)을 동일 조건으로 측정하여 **절대 clock 수치와 상대 오버헤드 비율을 모두 수집**한다.

```bash
#!/bin/bash
# scripts/measure_vecu_all.sh

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
OUTDIR="debug/logs/vecu_comm_${TIMESTAMP}"
mkdir -p $OUTDIR

AGL_IP="10.0.1.10"   # AGL V-ECU
PORT=8888

run_phase() {
    local label=$1   # "baseline" or "tls" or "tls_attest"
    local flags=$2   # extra flags for vecu_comm
    local subdir=$OUTDIR/$label
    mkdir -p $subdir

    echo ""
    echo "=== Phase: $label ==="

    # -- 1. Handshake --
    for i in $(seq 1 50); do
        ./vecu_comm --mode handshake-only --server $AGL_IP --port $PORT $flags \
            2>&1 | grep "TLS_HANDSHAKE"
    done | tee $subdir/handshake.log

    # -- 2. time -v (CPU/MEM) --
    for SIZE in 256 4096; do
        /usr/bin/time -v ./vecu_comm \
            --messages 1000 --msg-size $SIZE \
            --server $AGL_IP --port $PORT $flags \
            2>&1 | tee $subdir/time_v_${SIZE}.log
    done

    # -- 3. Latency (절대 clock, CLOCK_MONOTONIC_RAW) --
    for SIZE in 64 256 1024 4096; do
        ./vecu_comm --mode latency \
            --messages 500 --msg-size $SIZE \
            --server $AGL_IP --port $PORT $flags \
            2>&1 | grep "MSG_LATENCY" | tee -a $subdir/latency.log
    done

    # -- 4. CPU per message --
    for SIZE in 64 256 1024 4096; do
        ./vecu_comm --mode cpu-overhead \
            --messages 1000 --msg-size $SIZE \
            --server $AGL_IP --port $PORT $flags \
            2>&1 | grep "CPU_PER_MSG" | tee -a $subdir/cpu_overhead.log
    done
}

run_phase "baseline"     "--no-tls --no-attestation"
run_phase "tls_only"     "--tls    --no-attestation"
run_phase "tls_attest"   "--tls    --attestation"

echo ""
echo "=== Analysis ==="
python3 scripts/analyze_vecu.py $OUTDIR
```

```python
# scripts/analyze_vecu.py
# 절대값 + 상대 오버헤드 병행 분석
import os, sys, statistics, glob, re

outdir = sys.argv[1]
phases = ['baseline', 'tls_only', 'tls_attest']

def parse_handshake(path):
    vals = []
    for line in open(path):
        m = re.search(r'TLS_HANDSHAKE:\s*(\d+)', line)
        if m: vals.append(int(m.group(1)))
    return vals

def parse_latency(path):
    # "MSG_LATENCY ... Mean RTT : X us" 파싱
    result = {}
    for line in open(path):
        m = re.search(r'size=(\d+).*Mean RTT\s*:\s*(\d+)', line)
        if m: result[int(m.group(1))] = int(m.group(2))
    return result

def parse_timev(path):
    d = {}
    for line in open(path):
        for key in ['User time', 'System time', 'Percent of CPU', 'Maximum resident']:
            if key in line:
                d[key] = line.strip()
    return d

print("\n" + "="*70)
print("V-ECU COMMUNICATION PERFORMANCE — ABSOLUTE + RELATIVE OVERHEAD")
print("="*70)

# ── Handshake ──
print("\n[1] Handshake Overhead")
print(f"  {'Phase':<15} {'N':>5} {'Mean(us)':>10} {'Stdev':>8} {'P95':>8} {'vs baseline':>12}")
base_hs_mean = None
for phase in phases:
    p = f"{outdir}/{phase}/handshake.log"
    if not os.path.exists(p): continue
    vals = parse_handshake(p)
    if not vals: continue
    s = sorted(vals); n = len(vals)
    mean = statistics.mean(vals)
    if base_hs_mean is None: base_hs_mean = mean
    ovh = f"{(mean-base_hs_mean)/base_hs_mean*100:+.1f}%" if base_hs_mean else "—"
    print(f"  {phase:<15} {n:>5} {mean:>10.1f} {statistics.stdev(vals):>8.1f} "
          f"{s[int(n*0.95)]:>8}  {ovh:>12}")

# ── Message Latency ──
print("\n[2] Message Latency RTT (CLOCK_MONOTONIC_RAW)")
base_lat = {}
for phase in phases:
    p = f"{outdir}/{phase}/latency.log"
    if not os.path.exists(p): continue
    lats = parse_latency(p)
    for size, rtt in sorted(lats.items()):
        if phase == 'baseline': base_lat[size] = rtt
        ovh = f"{(rtt - base_lat.get(size, rtt)) / base_lat.get(size, rtt) * 100:+.1f}%" \
              if size in base_lat else "—"
        print(f"  {phase:<15} size={size:>5}B  RTT={rtt:>7} us  vs_baseline={ovh}")

# ── CPU/MEM (time -v) ──
print("\n[3] CPU/MEM Overhead (time -v, msg_size=256)")
for phase in phases:
    p = f"{outdir}/{phase}/time_v_256.log"
    if not os.path.exists(p): continue
    d = parse_timev(p)
    print(f"\n  {phase}:")
    for v in d.values(): print(f"    {v}")

# ── CPU per message ──
print("\n[4] CPU per Message")
for phase in phases:
    p = f"{outdir}/{phase}/cpu_overhead.log"
    if not os.path.exists(p): continue
    print(f"\n  {phase}:")
    for line in open(p): print(f"    {line.strip()}")
```

---

## 9. FVP 실행 환경 수정 포인트 요약

아래는 코드 수정 시 변경이 필요한 파일 목록이다.

### 9.1 신규 추가 파일

| 파일 | 역할 |
|---|---|
| `scripts/build-zephyr.sh` | Zephyr RTOS west 빌드 |
| `scripts/build-agl.sh` | AGL repo sync + bitbake 래퍼 |
| `scripts/build-guest-linux.sh` | Guest Linux (buildroot) 빌드 |
| `scripts/measure_attestation.sh` | Attestation 3단계 자동 측정 (절대+상대) |
| `scripts/measure_vecu_all.sh` | V-ECU 통신 전체 측정 (baseline/tls/tls_attest 3 phase) |
| `scripts/analyze_attest.py` | Attestation 절대값 + 상대 오버헤드 분석 |
| `scripts/analyze_vecu.py` | V-ECU 통신 절대값 + 상대 오버헤드 분석 |
| `src/vecu_attest/vecu_attest.c` | Zephyr용 Attestation 토큰 생성/전송 (CNTVCT_EL0 타이머) |
| `src/vecu_comm/vecu_comm.c` | V-ECU 간 통신 벤치마크 (CLOCK_MONOTONIC_RAW) |
| `src/vecu_comm/token_verify.c` | Verifier 측 토큰 검증 (단계별 세분화 측정) |

### 9.2 수정 필요 파일

| 파일 | 수정 내용 |
|---|---|
| `scripts/config.sh` | Zephyr SDK 경로, `AGL_IMG`/`AGL_KERNEL`, Guest Linux 경로 변수 추가 |
| `scripts/build.sh` | `zephyr`, `guest-linux` 빌드 타겟 추가 |
| `scripts/bootfvp.sh` | `cluster0.has_nested_virt=1`, VM 자동 기동 명령 추가 |
| `configs/linux.config` | `CONFIG_KVM=y`, `CONFIG_VIRTIO=y` 등 활성화 |

### 9.3 수정 불필요 파일

| 파일 | 이유 |
|---|---|
| `src/tf-a/` 전체 | EL3 SCRUTINIZER Monitor 변경 불필요 |
| `src/optee_os/` | Secure World 변경 불필요 |
| `configs/hafnium.bin` | Hafnium SPM 변경 불필요 |
| `configs/rmm.img` | TF-RMM이 이미 Realm VM을 지원 |

---

## 참고 자료

**ARM CCA / Realm:**
- [ARM CCA Architecture Specification (DEN0096)](https://developer.arm.com/documentation/den0096/latest/)
- [ARM RSI (Realm Service Interface) Specification](https://developer.arm.com/documentation/den0137/latest/) — §3.5의 RSI 호출 상세
- [ARM RMM (Realm Management Monitor) Specification](https://developer.arm.com/documentation/den0137/) — Granule Transition 상태 머신
- [TF-RMM Source & Guide](https://tf-rmm.readthedocs.io/en/latest/)
- [ARM CCA Threat Model](https://developer.arm.com/documentation/den0118/latest/) — NS Shared Memory 보안 고려사항

**Zephyr RTOS:**
- [Zephyr RTOS ARM64 지원](https://docs.zephyrproject.org/latest/boards/arm64/)
- [Zephyr prep_c.c (부팅 진입점)](https://github.com/zephyrproject-rtos/zephyr/blob/main/arch/arm64/core/prep_c.c)

**KVM / Virtualization:**
- [Linux KVM ARM64](https://www.kernel.org/doc/html/latest/virt/kvm/index.html)
- [kvmtool (lkvm) — Realm VM 지원 브랜치](https://git.kernel.org/pub/scm/linux/kernel/git/will/kvmtool.git)
- [KVM_CAP_ARM_RME API](https://www.kernel.org/doc/html/latest/virt/kvm/api.html) — Realm VM ioctl

**AGL (Automotive Grade Linux):**
- [AGL 공식 사이트](https://www.automotivelinux.org/)
- [AGL Getting Started (Yocto/bitbake)](https://docs.automotivelinux.org/en/porcupine/)
- [AGL qemuarm64 BSP 설정](https://docs.automotivelinux.org/en/porcupine/#01_Getting_Started/02_Building_AGL_Image/04_Build_for_QEMU/)
- [vsomeip (SOME/IP 구현체)](https://github.com/COVESA/vsomeip)

**Attestation 검증:**
- [Veraison Attestation Verification](https://github.com/veraison/services)
- [RATS (Remote ATtestation procedureS) — RFC 9334](https://www.rfc-editor.org/rfc/rfc9334)
- [EAT (Entity Attestation Token) — RFC 9165](https://www.rfc-editor.org/rfc/rfc9165)

**성능 측정 도구:**
- [GNU time -v manual](https://man7.org/linux/man-pages/man1/time.1.html)
- [clock_gettime(2) — CLOCK_MONOTONIC_RAW](https://man7.org/linux/man-pages/man2/clock_gettime.2.html)
- [/proc/self/stat 필드 설명](https://man7.org/linux/man-pages/man5/proc.5.html)
