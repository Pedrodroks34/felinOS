# FelinOS/Gato — Roadmap de Expansão Massiva

## Visão Geral

Transformar o FelinOS de um hobby OS educacional em um sistema production-ready com:
- SMP real, memory management avançado, block layer moderno
- Network stack production-grade (TCP CUBIC, IPv6, netfilter, eBPF)
- VFS completo com dentry/inode cache, mount namespaces
- Page cache unificado + file-backed mmap
- Filesystems modernos (ext4, btrfs)
- DRM/KMS + virtio-GPU
- Userspace musl + busybox
- Suporte amplo de hardware

---

## Fases de Implementação

### Fase 0: Infraestrutura Base (SEMANA 1-2) ✅ CONCLUÍDA
- [x] Estrutura `felinos-ports/` com toolchain musl cross-compiler
- [x] Build system para ports (Makefile, config.mk)
- [x] Bootstrap musl libc + headers para target x86_64-felinos
- [x] Busybox config minimal + patches para syscalls FelinOS
- [x] Initramfs/userspace layout (/bin, /sbin, /usr/bin, /lib)
- [x] QEMU test harness para userspace
- [x] Init system (s6)
- [x] Devel tools (make, pkgconf)
- [x] Kernel skeletons para Fases 1-11
- [x] Integração `make ports-*` no Makefile raiz
- [x] Correção crítica: patches musl syscall numbers (off-by-one fix)
- [x] Headers felinos públicos (vfs.h, net.h, syscall.h)

### Fase 1: Memory Management Avançado (SEMANA 3-5)
- [ ] **SLUB allocator** (per-cpu partial slabs, NUMA-aware, slab merging)
- [ ] **Page cache unificado** (`struct page`, address_space, radix tree / xarray)
- [ ] **File-backed mmap** (VM_SHARED, VM_MAYSHARE, fault handler, writeback)
- [ ] **Writeback infrastructure** (dirty inodes, bdi, flusher threads, wb_workfn)
- [ ] **KSM** (Kernel Samepage Merging) — stable tree, merge scan
- [ ] **Zswap / zram** (compressed swap in RAM, zbud/z3fold)
- [ ] **Huge pages** (2MB/1GB, hugetlbfs, THP infrastructure)
- [ ] **NUMA support** (node distance, memory policy, `mbind`, `set_mempolicy`)
- [ ] **Cgroups v2 memory controller** (memory.max, memory.high, OOM killer per cgroup)

### Fase 2: SMP Real + Scheduler Avançado (SEMANA 4-6)
- [ ] **Per-CPU run queues** (struct rq per CPU, no global lock)
- [ ] **Load balancing** (periodic + idle balance, NUMA-aware)
- [ ] **CFS-style scheduler** (vruntime, rb-tree, sched_entity, cfs_rq)
- [ ] **Scheduling classes** (CFS, RT, DL, idle, stop)
- [ ] **CPU affinity** (`sched_setaffinity`, `cpuset`, `isolcpus`)
- [ ] **Cgroups v2 cpu controller** (cpu.max, cpu.weight, cpu.idle)
- [ ] **IRQ affinity** + `/proc/irq/*/smp_affinity`
- [ ] **Tickless / NO_HZ** (dynticks, scheduler tick broadcast)

### Fase 3: Block Layer Moderno + DMA (SEMANA 5-8)
- [ ] **blk-mq** (multi-queue, request allocation, tagging, hctx)
- [ ] **Request merging** (front/back merge, plug, bio merging)
- [ ] **I/O schedulers** (none, mq-deadline, bfq, kyber)
- [ ] **DMA mapping API** (dma_map_single, dma_map_sg, dma_sync, IOMMU stub)
- [ ] **AHCI DMA** (command lists, PRDT, scatter-gather, NCQ)
- [ ] **ATA PIO → DMA transition** (libata-style port ops)
- [ ] **NVMe driver** (admin queue, I/O queues, PRP/SGL, namespaces)
- [ ] **TRIM/DISCARD** (blkdev_issue_discard, fstrim syscall)
- [ ] **Block integrity** (DIF/DIX, PI metadata)
- [ ] **dm-linear, dm-stripe, dm-crypt** (device-mapper core)

### Fase 4: VFS Completo (SEMANA 6-9)
- [ ] **Dentry cache** (dentry LRU, d_hash, d_compare, DCACHE_RCUACCESS)
- [ ] **Inode cache** (inode LRU, I_DIRTY_SYNC/DATAS, writeback)
- [ ] **RCU path walk** (LOOKUP_RCU, sequence counters, mount_lock)
- [ ] **Mount namespaces** (shared subtree, peer groups, propagation)
- [ ] **Bind mounts** + **overlayfs** (lower/upper/work, copy-up, redirect)
- [ ] **File locking** (posix locks, flock, lease, lockd/NFS stub)
- [ ] **Fanotify / inotify** (fs notification, marks, groups)
- [ ] **io_uring support** (io_uring_setup, SQ/CQ rings, async syscalls)

### Fase 5: Network Stack Production (SEMANA 7-11)
- [ ] **TCP CUBIC** (HyStart, fast convergence, limited slow start)
- [ ] **TCP BBR** (pacing, delivery rate, probeRTL, startup/drain/probeBW/probeRTT)
- [ ] **Window scaling, SACK, timestamps, ECN, FACK, TSO/LRO**
- [ ] **IPv6 completo** (NDP, SLAAC, DAD, router solicitation/advertisement, MLD)
- [ ] **Netfilter core** (nf_hook, nf_conntrack, nf_nat, nf_tables, nftables)
- [ ] **XDP / eBPF networking** (xdp_frame, bpf_prog_run, map helpers)
- [ ] **QoS / tc** (htb, fq_codel, cake, clsact, flower)
- [ ] **TLS/kTLS** (kernel TLS offload, rx/tx skb crypto)
- [ ] **WireGuard** (noise protocol, roaming, crypto queue)
- [ ] **VLAN, bonding, team, bridge, VXLAN, GRE, Geneve**

### Fase 6: Filesystems Modernos (SEMANA 10-16)
- [ ] **ext4** (extents, delayed allocation, journal checksum, inline data, encryption)
- [ ] **btrfs** (CoW B-tree, subvolumes, snapshots, RAID 0/1/10/5/6, compression, send/receive)
- [ ] **f2fs** (log-structured, multi-head logging, GC, checkpoint, encryption)
- [ ] **erofs** (readonly, compression LZ4/ZSTD, tail packing, fsverity)
- [ ] **fscrypt** (per-file encryption, policy, keyring integration)
- [ ] **fs-verity** (Merkle tree, digest verification, signed hashes)

### Fase 7: DRM/KMS + virtio-GPU (SEMANA 12-16)
- [ ] **DRM core** (device, file, auth, gem, prime, mode_config)
- [ ] **KMS** (crtc, encoder, connector, plane, atomic modeset, properties)
- [ ] **GEM/TTM** (buffer objects, migration, placement, fence, reservation)
- [ ] **virtio-GPU** (virglrenderer protocol, resource create, submit cmd, cursor)
- [ ] **Simple framebuffer driver** (bochs dispi, virtio-gpu fallback)
- [ ] **Userspace**: libdrm, mesa (virgl), wayland, weston

### Fase 8: Power Management Avançado (SEMANA 10-13)
- [ ] **CPUIdle** (C-states, menu governor, TEO, latency/ residency)
- [ ] **CPUFreq** (P-states, intel_pstate, amd_pstate, schedutil governor)
- [ ] **Suspend-to-RAM (S3)** + **Hibernate (S4)** (snapshot, resume, swsusp)
- [ ] **Runtime PM** (device pm_domain, autosuspend, rpm_idle/suspend/resume)
- [ ] **Thermal zones** (thermal_cooling_device, governors: step_wise, fair_share)
- [ ] **ACPI battery** (_BST, _BIF, _BTP), AC adapter, lid switch
- [ ] **CPU hotplug** (online/offline, smpboot threads)

### Fase 9: eBPF + Observability (SEMANA 12-16)
- [ ] **eBPF VM** (64-bit registers, helper calls, JIT x86-64, verifier)
- [ ] **BPF maps** (hash, array, lru_hash, lpm_trie, ringbuf, queue/stack)
- [ ] **Program types** (socket_filter, kprobe, tracepoint, xdp, cgroup_skb, lsm)
- [ ] **BTF** (BPF Type Format, CO-RE, pahole integration)
- [ ] **ftrace** (function tracer, graph tracer, trace_events, hist triggers)
- [ ] **perf_events** (PMU, hardware/software events, callchain, aux trace)
- [ ] **kprobes/uprobes** (breakpoint, kretprobe, uprobe, register/unregister)
- [ ] **LSM-BPF** (security hooks via eBPF programs)

### Fase 10: Security & Isolation (SEMANA 13-16)
- [ ] **Capabilities** (capable(), cap_capable, file capabilities, ambient)
- [ ] **Seccomp-BPF** (TSYNC, notify, user_notif_fd)
- [ ] **Namespaces** (pid, net, mnt, uts, ipc, cgroup, user, time)
- [ ] **User namespaces** (uid/gid mapping, /proc/*/uid_map/gid_map)
- [ ] **LSM hooks** (SELinux/AppArmor stubs, landlock)
- [ ] **Kernel hardening** (KPTI, RETPOLINE, CET, KASLR, STACKLEAK, PAGE_TABLE_ISOLATION)

### Fase 11: Hardware Support (CONTÍNUO)
- [ ] **PCIe hotplug** (ACPII, SHPC, DPC, AER)
- [ ] **USB stack** (xHCI/EHCI, hub, HID, mass storage, serial, video)
- [ ] **WiFi** (mac80211, cfg80211, iwlwifi/ath9k/mt76 stubs)
- [ ] **Bluetooth** (hci, l2cap, sco, rfcomm, bnep)
- [ ] **Thunderbolt** (NHI, domain, switch, PCIe tunneling)
- [ ] **SCSI/SAS** (scsi_mod, libsas, target/initiator)
- [ ] **DM-Multipath / iSCSI / NVMe-oF**
- [ ] **Sound** (ALSA core, HDA, USB audio, SoC/DAI)

### Fase 12: Userspace Ecosystem (SEMANA 2-8, PARALELO)
- [ ] **musl libc** (port syscalls, TLS, stack protector, malloc, locale, iconv)
- [ ] **busybox** (ash, coreutils, init, modprobe, udhcpc, ntpd, syslogd)
- [ ] **init system** (systemd-minimal ou s6/openrc, unit files, socket activation)
- [ ] **package manager** (apk/opkg stub, felinos-pkg, repo index)
- [ ] **Development tools** (make, pkg-config, cmake, ninja, git, ssh)
- [ ] **Containers** (runc, crun, cni plugins, crictl)

---

## Estrutura de Diretórios Proposta

```
felinos-src/
├── kernel/                    # Kernel existente
│   ├── mm/                    # NOVO: slub.c, pagecache.c, writeback.c, huge.c, zswap.c, ksm.c, numa.c
│   ├── sched/                 # NOVO: fair.c, rt.c, dl.c, idle.c, cpufreq.c, numa_balancing.c
│   ├── block/                 # NOVO: blk-mq.c, blk-core.c, elevator.c, blk-dma.c, nvme.c
│   ├── fs/                    # EXPANDIDO: dcache.c, inode.c, namespace.c, overlayfs.c, io_uring.c
│   ├── net/                   # EXPANDIDO: tcp_cubic.c, tcp_bbr.c, ipv6/, netfilter/, xdp/, tc/, tls/, wireguard/
│   ├── drivers/
│   │   ├── gpu/drm/           # NOVO: drm_core.c, kms.c, gem.c, ttm.c, virtio_gpu.c
│   │   ├── power/             # EXPANDIDO: cpuidle.c, cpufreq.c, thermal.c, suspend.c
│   │   ├── usb/               # NOVO: xhci.c, ehci.c, hub.c, hid.c, storage.c
│   │   ├── net/wireless/      # NOVO: mac80211.c, cfg80211.c
│   │   ├── scsi/              # NOVO: scsi_mod.c, libsas.c
│   │   └── sound/             # NOVO: alsa/, hda/, usb/
│   ├── bpf/                   # NOVO: verifier.c, jit_x86.c, maps.c, helpers.c, btf.c
│   ├── trace/                 # NOVO: ftrace.c, perf.c, kprobes.c, trace_events.c
│   ├── security/              # NOVO: capabilities.c, seccomp.c, namespace.c, lsm.c, landlock.c
│   └── cgroup/                # NOVO: cgroup_v2.c, cpu.c, memory.c, io.c, pids.c
├── felinos-ports/             # NOVO: Ports tree estilo BSD
│   ├── Makefile
│   ├── config.mk
│   ├── toolchain/
│   │   ├── musl/              # musl source + patches + build scripts
│   │   ├── linux-headers/     # UAPI headers for felinos
│   │   └── cross-compile.sh   # Build cross-toolchain
│   ├── base/
│   │   ├── musl/              # musl build recipe
│   │   ├── busybox/           # busybox build recipe + config + patches
│   │   └── felinos-base/      # Base filesystem layout, init scripts
│   ├── system/
│   │   ├── systemd/           # systemd build (minimal)
│   │   ├── s6/                # s6 supervision suite
│   │   └── openrc/            # OpenRC init
│   ├── devel/
│   │   ├── make/              # GNU make
│   │   ├── pkgconf/           # pkg-config
│   │   ├── cmake/             # CMake
│   │   ├── ninja/             # Ninja
│   │   ├── git/               # Git
│   │   └── openssh/           # OpenSSH / dropbear
│   └── pkg/                   # Binary package cache
├── userspace/                 # NOVO: Installed userspace rootfs
│   ├── bin/
│   ├── sbin/
│   ├── lib/                   # musl libc.so, ld-musl.so
│   ├── usr/
│   │   ├── bin/
│   │   ├── lib/
│   │   ├── include/
│   │   └── share/
│   └── etc/
│       ├── init.d/
│       ├── fstab
│       ├── passwd
│       └── group
├── tools/
│   ├── runtests.py            # Existente
│   ├── mkrootfs.py            # NOVO: Build initramfs/rootfs from ports
│   ├── pkg.py                 # NOVO: Package manager
│   └── cross-env.sh           # NOVO: Cross-compilation environment
└── ROADMAP.md                 # Este arquivo
```

---

## Dependências Entre Fases

```
Fase 0 (Ports/Toolchain)
    │
    ├───► Fase 1 (MM) ◄──────────────┐
    │                                │
    ├───► Fase 2 (SMP) ──────────────┤
    │                                │
    ├───► Fase 3 (Block) ────────────┤
    │                                │
    ├───► Fase 4 (VFS) ──────────────┼──► Fase 6 (FS: ext4, btrfs)
    │                                │
    ├───► Fase 5 (Net) ──────────────┤
    │                                │
    ├───► Fase 7 (DRM) ──────────────┤
    │                                │
    ├───► Fase 8 (Power) ────────────┤
    │                                │
    ├───► Fase 9 (eBPF) ─────────────┤
    │                                │
    └───► Fase 10 (Security) ────────┘
                │
                ▼
        Fase 12 (Userspace) ←──────── Fase 0
```

---

## Marcos de Entrega (Milestones)

| Marco | Descrição | Critério de Sucesso |
|-------|-----------|---------------------|
| **M0** | Toolchain musl + busybox bootando | `make run` → shell busybox ash funcional |
| **M1** | Page cache + file mmap | `mmap(MAP_SHARED)` em arquivo funciona, `msync` persiste |
| **M2** | SMP real | `make run-smp` → 4 CPUs rodando tasks, `htop` mostra distribuição |
| **M3** | blk-mq + AHCI DMA | `dd if=/dev/hda of=/dev/null bs=1M` > 100 MB/s |
| **M4** | VFS RCU + mount ns | `unshare -m` isola mounts, `pivot_root` funciona |
| **M5** | TCP CUBIC + IPv6 | `iperf3` atinge line rate, `ping6` funciona |
| **M6** | ext4 montável | `mkfs.ext4 /dev/hdb && mount -t ext4 /dev/hdb /mnt` |
| **M7** | virtio-GPU + DRM | `modetest` mostra connectors, Weston inicia |
| **M8** | Suspend/resume S3 | `systemctl suspend` → resume com estado preservado |
| **M9** | eBPF + ftrace | `bpftrace -e 'tracepoint:syscalls:sys_enter_write { @[comm] = count(); }'` |
| **M10** | Containers | `runc run mycontainer` → shell isolado com pid/net/mnt ns |

---

## Riscos e Mitigações

| Risco | Probabilidade | Impacto | Mitigação |
|-------|---------------|---------|-----------|
| Escopo excessivo | Alta | Crítico | Entregar por marcos (M0-M10), não features isoladas |
| Complexidade VFS RCU | Alta | Alto | Começar com dentry cache simples, RCU depois |
| eBPF verifier/JIT | Muito Alta | Alto | Usar ubpf/rbpf como base, portar verifier Linux incrementalmente |
| ext4/btrfs do zero | Alta | Crítico | Portar código Linux (GPLv2 compatível) com adaptações mínimas |
| DRM/KMS complexidade | Alta | Médio | Começar com virtio-GPU simples, DRM core mínimo |
| Manutenção long-term | Média | Alto | CI/CD rigoroso, testes `selftest` expandidos, documentação |

---

## Próximos Passos Imediatos (Fase 1 - Memory Management)

1. **SLUB allocator** (`kernel/mm/slub.c`) - substituir buddy allocator atual
2. **Page cache unificado** (`kernel/mm/pagecache.c`) - struct page, address_space, radix tree
3. **File-backed mmap** - estender `sys_mmap` em `kernel/user.c` para MAP_SHARED
4. **Writeback infrastructure** (`kernel/mm/writeback.c`) - dirty inodes, flusher threads
5. **Testes**: `make ports-toolchain && make ports-base && make test` validar M0

---

## Convenções de Código

- **Estilo**: Linux kernel coding style (Lindent, 8 tabs, no braces em single-line)
- **Locking**: `lockdep` annotations desde o início (`lock_class_key`, `lockdep_assert_held`)
- **Memory**: `__GFP_*` flags, `kmalloc_array`, `kzalloc`, `kvmalloc` para large
- **Error handling**: `ERR_PTR` / `IS_ERR` / `PTR_ERR` em todo novo código
- **Documentation**: Kernel-doc comments (`/** ... */`) em todas funções públicas
- **Testing**: Cada subsistema novo tem `*_selftest.c` integrado ao `selftest` command

---

## Referências de Código Fonte (para port/estudo)

| Subsistema | Repositório | Arquivos-chave |
|------------|-------------|----------------|
| SLUB | linux/mm/slub.c | `slab_alloc`, `slab_free`, `kmem_cache_create` |
| Page cache | linux/mm/filemap.c | `filemap_read`, `filemap_write`, `read_cache_pages` |
| Writeback | linux/mm/page-writeback.c | `wb_writeback`, `balance_dirty_pages`, `writeback_inodes_sb` |
| blk-mq | linux/block/blk-mq.c | `blk_mq_alloc_request`, `blk_mq_start_request`, `__blk_mq_run_hw_queue` |
| CFS | linux/kernel/sched/fair.c | `pick_next_task_fair`, `set_next_entity`, `update_curr` |
| Dentry | linux/fs/dcache.c | `d_lookup`, `d_alloc`, `d_invalidate`, `__d_drop` |
| RCU path | linux/fs/namei.c | `link_path_walk`, `walk_component`, `lookup_fast` |
| TCP CUBIC | linux/net/ipv4/tcp_cubic.c | `cubic_update`, `cubic_root`, `tcp_cubic_cwnd` |
| eBPF | linux/kernel/bpf/ | `bpf_check`, `bpf_prog_run`, `bpf_jit_compile` |
| DRM | linux/drivers/gpu/drm/ | `drm_dev_init`, `drm_atomic_commit`, `drm_gem_object_init` |
| virtio-GPU | linux/drivers/gpu/drm/virtio/ | `virtio_gpu_driver`, `virtio_gpu_object_create` |
| musl | musl-libc.org | `src/thread/x86_64/syscall.s`, `arch/x86_64/bits/syscall.h` |

---

*Documento vivo — atualizar conforme marcos são atingidos.*