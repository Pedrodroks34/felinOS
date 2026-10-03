# FelinOS Phase 0 Implementation Plan
## Infraestrutura Base (SEMANA 1-2)

---

## Análise do Estado Atual

### ✅ Já Implementado
- Estrutura `felinos-ports/` completa com:
  - `config.mk` - configuração global (paths, versions, flags)
  - `Makefile` - orquestração de build (toolchain, base, system, devel)
  - `toolchain/Makefile` - binutils + GCC bootstrap/final + linux headers
  - `base/musl/Makefile` + 3 patches para syscalls FelinOS
  - `base/busybox/Makefile` + `felinos_defconfig`
  - `base/felinos-base/Makefile` - layout de FS, init scripts, configs
  - `tools/cross-env.sh` - ambiente de cross-compilação
  - `tools/mkrootfs.py` - gerador de rootfs.img
  - `tools/runtests.py` - test harness existente (in-kernel selftest)

### ❌ Faltando / Incompleto
1. **System init systems**: `system/s6/`, `system/openrc/`, `system/systemd/` não existem
2. **Devel tools**: `devel/make`, `devel/pkgconf`, `devel/cmake`, `devel/ninja`, `devel/git`, `devel/openssh` não existem
3. **Busybox patches**: diretório `base/busybox/patches/` existe mas sem patches reais
4. **Integração com Makefile raiz**: Sem targets `make ports-*`
5. **QEMU test harness para userspace**: `runtests.py` testa apenas in-kernel `selftest`
6. **Esqueletos kernel**: Arquivos `.c/.h` vazios para novos subsistemas
7. **BUG CRÍTICO**: Patches do musl têm numeração de syscall **off-by-one** (musl usa 0-based, kernel usa 1-based)

---

## Plano Detalhado de Implementação

### Semana 1: Correções Críticas + Toolchain + Base

#### Dia 1-2: Correção dos Patches musl (CRÍTICO)
**Arquivos a modificar:**
- `felinos-ports/base/musl/patches/0001-felinos-syscall-numbers.patch` - Ajustar todos os números para matchar `kernel/syscall.h` (adicionar +1)
- `felinos-ports/base/musl/patches/0002-felinos-syscall-abi.patch` - Verificar se ABI está correta
- `felinos-ports/base/musl/patches/0003-felinos-kernel-structs.patch` - Verificar structs contra kernel

**Validação:** Build musl e testar syscall simples (exit, write, read)

#### Dia 3-4: Build do Toolchain Completo
**Targets:**
```bash
make -C felinos-ports toolchain
```
**Verificações:**
- binutils 2.42 builda e instala em `felinos-ports/toolchain/bin/`
- GCC 14.1.0 bootstrap builda sem libc
- Linux headers 6.6 UAPI instalados em `userspace/usr/include/`
- musl headers instalados via `base/musl` (dependência do GCC final)
- GCC final builda com musl + threads POSIX
- `x86_64-felinos-gcc --version` funciona

#### Dia 5: Build musl + Busybox
```bash
make -C felinos-ports base/musl
make -C felinos-ports base/busybox
```
**Verificações:**
- `userspace/lib/libc.so` e `userspace/lib/ld-musl-x86_64.so.1` existem
- `userspace/bin/busybox` builda estático
- Symlinks: sh, ash, init criados

#### Dia 6-7: Base Filesystem + Integração Makefile Raiz
```bash
make -C felinos-ports base/felinos-base
make -C felinos-ports mkrootfs
```
**Adicionar ao Makefile raiz (`Makefile`):**
```make
# Ports integration targets
ports-toolchain:
	$(MAKE) -C felinos-ports toolchain

ports-base:
	$(MAKE) -C felinos-ports base

ports-system:
	$(MAKE) -C felinos-ports system

ports-devel:
	$(MAKE) -C felinos-ports devel

ports-full:
	$(MAKE) -C felinos-ports full

ports-mkrootfs:
	$(MAKE) -C felinos-ports mkrootfs

ports-test:
	$(MAKE) -C felinos-ports test-rootfs

ports-clean:
	$(MAKE) -C felinos-ports clean

ports-distclean:
	$(MAKE) -C felinos-ports distclean

ports-info:
	$(MAKE) -C felinos-ports info
```

---

### Semana 2: Init System + Devel Tools + Test Harness + Kernel Skeletons

#### Dia 8-9: Init System (s6)
**Criar: `felinos-ports/system/s6/Makefile`**
- Baixar skarnet.org software (skalibs, execline, s6)
- Build na ordem: skalibs → execline → s6
- Instalar em sysroot
- Criar `/etc/s6/` com service definitions básicas

#### Dia 10: Devel Tools (Mínimo: make, pkgconf)
**Criar: `felinos-ports/devel/make/Makefile`**
**Criar: `felinos-ports/devel/pkgconf/Makefile`**
- GNU Make 4.4
- pkgconf 2.1 (leve, substitui pkg-config)

#### Dia 11: QEMU Test Harness para Userspace
**Criar: `tools/test-userspace.py`**
Baseado em `runtests.py` mas:
- Boota com `-kernel gato.bin -initrd initramfs.img` OU `-drive rootfs.img`
- Espera prompt busybox ash
- Executa suite de testes userspace:
  - Hello world (static linked)
  - Shell commands (ls, cat, echo, mkdir, etc.)
  - Network (ping, wget, nc)
  - Process (fork, execve, wait, kill)
  - FS (mount, stat, readdir)
  - IPC (pipe, socket)

**Formato de saída compatível:**
```
Checks: N  Passed: N  Failed: N
```

#### Dia 12: Kernel Skeletons para Fases 1-11
**Criar arquivos vazios com TODOs em `kernel/`:**
```
kernel/mm/slub.c           # SLUB allocator
kernel/mm/pagecache.c      # Page cache unificado
kernel/mm/writeback.c      # Writeback infrastructure
kernel/mm/huge.c           # Huge pages
kernel/mm/zswap.c          # Zswap/zram
kernel/mm/ksm.c            # KSM
kernel/mm/numa.c           # NUMA support

kernel/sched/fair.c        # CFS scheduler
kernel/sched/rt.c          # RT scheduler
kernel/sched/dl.c          # Deadline scheduler
kernel/sched/idle.c        # Idle scheduler
kernel/sched/cpufreq.c     # CPUFreq
kernel/sched/numa_balancing.c

kernel/block/blk-mq.c      # blk-mq core
kernel/block/blk-core.c    # Block core
kernel/block/elevator.c    # I/O schedulers
kernel/block/blk-dma.c     # DMA mapping
kernel/block/nvme.c        # NVMe driver

kernel/fs/dcache.c         # Dentry cache
kernel/fs/inode.c          # Inode cache
kernel/fs/namespace.c      # Mount namespaces
kernel/fs/overlayfs.c      # OverlayFS
kernel/fs/io_uring.c       # io_uring

kernel/net/tcp_cubic.c     # TCP CUBIC
kernel/net/tcp_bbr.c       # TCP BBR
kernel/net/ipv6/           # IPv6 stack
kernel/net/netfilter/      # Netfilter
kernel/net/xdp/            # XDP/eBPF
kernel/net/tc/             # QoS/tc

kernel/drivers/gpu/drm/    # DRM/KMS core
kernel/drivers/gpu/drm/virtio_gpu.c

kernel/bpf/verifier.c      # eBPF verifier
kernel/bpf/jit_x86.c       # eBPF JIT
kernel/bpf/maps.c          # BPF maps
kernel/bpf/helpers.c       # BPF helpers
kernel/bpf/btf.c           # BTF

kernel/trace/ftrace.c      # ftrace
kernel/trace/perf.c        # perf_events
kernel/trace/kprobes.c     # kprobes/uprobes

kernel/security/capabilities.c
kernel/security/seccomp.c
kernel/security/namespace.c
kernel/security/lsm.c
kernel/security/landlock.c

kernel/cgroup/cgroup_v2.c
kernel/cgroup/cpu.c
kernel/cgroup/memory.c
```

Cada arquivo:
```c
/* TODO: Implementar <subsistema> - Fase X
 * Ref: linux/<path>/<file>.c
 */
#include <kernel/subsys.h>

// TODO: Implementar
```

#### Dia 13-14: Testes de Integração + Documentação
**Teste completo:**
```bash
make clean && make
make ports-toolchain
make ports-base
make ports-mkrootfs
make ports-test
```
**Validar M0:** "Toolchain musl + busybox bootando → shell busybox ash funcional"

**Atualizar documentação:**
- `ROADMAP.md` - marcar checkboxes da Fase 0
- `README.md` - atualizar instruções de build com ports
- `felinos-ports/README.md` - verificar se está atualizado

---

## Checklist de Entregáveis Fase 0

| Item | Status | Target |
|------|--------|--------|
| Patch musl syscall numbers corrigido | ❌ | Dia 1-2 |
| Patch musl syscall ABI verificado | ❌ | Dia 1-2 |
| Patch musl kernel structs verificado | ❌ | Dia 1-2 |
| Toolchain cross-compiler funcional | ❌ | Dia 3-4 |
| musl libc buildado e instalado | ❌ | Dia 5 |
| Busybox buildado e instalado | ❌ | Dia 5 |
| Base filesystem layout criado | ❌ | Dia 6-7 |
| Integração Makefile raiz (ports-*) | ❌ | Dia 6-7 |
| Init system s6 | ❌ | Dia 8-9 |
| Devel tools (make, pkgconf) | ❌ | Dia 10 |
| QEMU test harness userspace | ❌ | Dia 11 |
| Kernel skeletons fases 1-11 | ❌ | Dia 12 |
| Teste M0 passa (shell ash funcional) | ❌ | Dia 13-14 |
| Documentação atualizada | ❌ | Dia 13-14 |

---

## Riscos e Mitigações

| Risco | Probabilidade | Impacto | Mitigação |
|-------|---------------|---------|-----------|
| Toolchain build falha (host deps) | Média | Alto | Documentar deps exatas; CI cache |
| musl patches não aplicam | Alta | Crítico | Testar patches isoladamente primeiro |
| Syscall number mismatch | **Já existe** | **Crítico** | **Corrigir no Dia 1** |
| Busybox config missing features | Baixa | Médio | Usar defconfig testado; patch incremental |
| mkrootfs precisa sudo | Média | Baixo | Fallback para initramfs.cpio.gz |
| QEMU test harness flaky | Média | Médio | Timeouts generosos; retry logic |

---

## Dependências de Host (Build Machine)

```bash
# Debian/Ubuntu
apt install build-essential wget tar xz-utils bzip2 gzip python3 sudo \
    texinfo flex bison libgmp-dev libmpfr-dev libmpc-dev libisl-dev

# Arch
pacman -S base-devel wget tar xz bzip2 gzip python sudo \
    texinfo flex bison gmp mpfr mpc isl
```

---

## Comandos de Validação Rápida

```bash
# 1. Verificar toolchain
make -C felinos-ports toolchain
felinos-ports/toolchain/bin/x86_64-felinos-gcc --version

# 2. Verificar musl
make -C felinos-ports base/musl
ls -la userspace/lib/libc.so userspace/lib/ld-musl-x86_64.so.1

# 3. Verificar busybox
make -C felinos-ports base/busybox
felinos-ports/toolchain/bin/x86_64-felinos-readelf -h userspace/bin/busybox

# 4. Verificar rootfs
make -C felinos-ports mkrootfs
ls -la rootfs.img

# 5. Teste M0 - boot com rootfs
make -C felinos-ports test-rootfs
# Deve chegar em prompt: root@felinos:~#
# Digitar: echo "hello from userspace"
# Digitar: poweroff

# 6. Teste harness userspace
python3 tools/test-userspace.py
# Deve mostrar: Checks: N  Passed: N  Failed: 0
```

---

## Próximos Passos Pós-Fase 0

Após M0 validado, iniciar **Fase 1 (Memory Management)**:
1. SLUB allocator (`kernel/mm/slub.c`)
2. Page cache unificado (`kernel/mm/pagecache.c`)
3. File-backed mmap (estender `user.c` sys_mmap)
4. Writeback infrastructure

A infraestrutura de ports estará pronta para compilar programas userspace mais complexos necessários para testar MM (ex: `stress-ng`, `fio`, benchmarks customizados).