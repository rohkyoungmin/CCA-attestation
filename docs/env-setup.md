# 실험 환경 세팅

## 포트 구성

| 포트 | 대상 |
|------|------|
| 5000 | FVP Normal World Linux |
| 5001 | AGL KVM 게스트 (V-ECU2) |
| 5002 | Zephyr Realm VM (V-ECU1) |

---

## 1. FVP 부팅 (호스트)

```bash
cd <repo-root>
./scripts/bootfvp.sh
```

---

## 2. FVP Linux 접속

```bash
telnet localhost 5000
# ID: root / PW: root
```

---

## 3. Zephyr 빌드 (호스트)

```bash
cd <repo-root>/dev_workspace/zephyr
west build -b lkvm_realm <repo-root>/src/vecu_zephyr --pristine
```

---

## 4. FVP에 바이너리 업로드 (호스트)

```bash
# Zephyr
scp dev_workspace/zephyr/build/zephyr/zephyr.bin root@192.168.122.33:/root/realm-zephyr.bin

# AGL (있을 경우)
scp <path>/Image root@192.168.122.33:/root/guest-Image
scp <path>/agl-image-minimal-qemuarm64.rootfs.ext4 root@192.168.122.33:/root/agl.ext4
```

---

## 5. VM 실행 (FVP 5000 쉘 안에서)

### Zephyr Realm VM (V-ECU1)

```bash
# 백그라운드 - 출력이 telnet 5002로
/root/run-vecu-zephyr.sh

# 포그라운드 - 출력이 현재 터미널에 바로 출력 (디버깅용)
lkvm-realm run --realm -k /root/realm-zephyr.bin -m 64 -c 1 --console serial --name realm-vecu1
```

### AGL VM (V-ECU2)

```bash
/root/run-vecu-agl.sh
```

---

## 6. VM 콘솔 접속 (호스트)

```bash
telnet localhost 5001   # AGL
telnet localhost 5002   # Zephyr
```

telnet 종료: `Ctrl + ]` → `quit`

---

## 7. VM 정리 (FVP 5000 쉘)

```bash
kill -9 $(ps | grep lkvm | grep -v grep | awk '{print $1}') 2>/dev/null
rm -f /root/.lkvm/realm-vecu1.sock /root/.lkvm/agl-normal.sock
```
