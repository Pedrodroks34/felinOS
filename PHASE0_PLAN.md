# FelinOS — Plano de Implementação: Fase 0 (Infraestrutura Base)

## Estado Atual (Análise do Código Fonte)

Após inspecionar o repositório, identificamos que **a Fase 0 já foi iniciada** e possui uma estrutura significativa. Abaixo o que existe e o que falta.

### ✅ Já Implementado (parcialmente)

| Item | Localização | Status |
|------|-------------|--------|
| Estrutura `felinos-ports/` | `felinos-ports/` (Makefile, config.mk, README.md) | ✅ Estrutura completa |
| Config global de build | `felinos-ports/config.mk` | ✅ Bem comentado, variáveis para TARGET, CC, CFLAGS, etc. |
| Build system para ports | `felinos-ports/Makefile` | ✅ Targets: toolchain, base, system, devel, mkrootfs, test-rootfs |
| Patches musl (3 arquivos) | `felinos-ports/base/musl/patches/0001..0003.patch` | ⚠️ Existem mas precisam validação |
| Patches busybox | `felinos-ports/base/busybox/patches/` | ❌ Diretório não existe |
| Busybox felinos_defconfig | `felinos-ports/base/busybox/felinos_defconfig` | ✅ Existe |
| musl source extraído | `felinos-ports/build/musl/` | ✅ Build dir existe |
| musl tarball | `felinos-ports/downloads/musl-1.2.5.tar.gz` | ✅ Baixado |
| Toolchain Makefile | `felinos-ports/toolchain/Makefile` | ✅ Estrutura completa (binutils, GCC, headers) |
| felinos-base layout | `felinos-ports/base/felinos-base/Makefile` | ✅ Existe |
| s6 init system | `felinos-ports/system/s6/Makefile` | ✅ Existe |
| GNU Make port | `felinos-ports/devel/make/Makefile` | ✅ Existe |
| pkgconf port | `felinos-ports/devel/pkgconf/Makefile` | ✅ Existe |
| cross-env.sh | `tools/cross-env.sh` | ✅ Script de cross-compilação |
| mkrootfs.py | `tools/mkrootfs.py` | ✅ Gerador de rootfs |
| Integração `make ports-*` | Root `Makefile` (linhas 189-226) | ✅ Alvo targets ports-* |

### ❌ Faltando / Quebrado

| Item | Problema |
|------|----------|
| **Toolchain binário** | `felinos-ports/toolchain/bin/` **não existe** — cross-compiler nunca foi buildado |
| **userspace/** | Diretório **não existe** — SYSROOT não foi populado |
| **Busybox patches** | Diretório `felinos-ports/base/busybox/patches/` não existe |
| **Busybox tarball** | `busybox-1.36.1.tar.bz2` não está em `downloads/` |
| **Headers do kernel** | `kernel/include/` não existe — `config.mk` referencia `-I$(KERNEL_DIR)/include` |
| **Test harness QEMU userspace** | Nenhum script `tools/runtests_userspace.py` para bootar rootfs e validar busybox |
| **felinos.cross** | Arquivo de cross-file para meson não existe |
| **Validável** | `make ports-toolchain && make ports-base && make ports-test` nunca foi executado com sucesso |

---

## Plano de Ação — Fase 0 (Ordem de Execução)

### Etapa 0: Pré-requisitos (Host)

```bash
# Verificar dependências de host necessárias
sudo apt install -y build-essential wget tar xz-utils bzip2 gzip python3 \
    mkfs.ext4 sudo qemu-system-x86
```

### Etapa 1: Criar diretório `kernel/include/` e headers públicos

O `config.mk` referencia `-I$(KERNEL_DIR)/include`, mas esse diretório não existe. Precisamos criá-lo e copiar os headers públicos do kernel (syscall.h, etc.).

```bash
mkdir -p kernel/include
cp kernel/syscall.h kernel/include/felinos/syscall.h
```

Também criar headers compatíveis POSIX que musl/busybox precisam (`fcntl.h`, `sys/stat.h`, `unistd.h`, etc.) que mapeiam para as syscalls FelinOS.

### Etapa 2: Criar patches do Busybox

O diretório `felinos-ports/base/busybox/patches/` precisa existir com patches que:
1. Corrigem as syscall numbers para o ABI FelinOS (int 0x80)
2. Ajustam a config para ser estática
3. Removem dependências de threads que não existem

Exemplo de conteúdo do patch mínimo:
```
--- a/include/syscalls.h
+++ b/include/syscalls.h
@@ -1,3 +1,7 @@
+#include <felinos/syscall.h>
+#ifdef __NR_felinOS_read
+#define __NR_read __NR_felinOS_read
+#endif
```

### Etapa 3: Build do Toolchain (cross-compiler)

```bash
make ports-toolchain
```

Isto executa: linux headers → binutils → GCC bootstrap → musl headers → GCC final.

**Dependências:** GCC 14.1, binutils 2.42, musl 1.2.5 sources. Tudo já está em `downloads/`.

### Etapa 4: Build Base System (musl + busybox + layout)

```bash
make ports-base
```

Após isso, o diretório `userspace/` deve ser populado com:
- `userspace/usr/include/` — headers musl + kernel
- `userspace/usr/lib/` — libc.a (musl estático)
- `userspace/bin/` — busybox + symlinks (sh, ash, ls, cat, etc.)
- `userspace/etc/` — passwd, group, fstab, inittab, profile
- `userspace/init` — script de init

### Etapa 5: Test Harness QEMU para Userspace

Criar `tools/runtests_userspace.py` (ou estender `runtests.py`):

```python
# Pseudo-código
def test_userspace():
    # 1. Gerar rootfs
    os.system("make ports-mkrootfs")
    
    # 2. Bootar QEMU com kernel + rootfs
    qemu_cmd = [
        "qemu-system-x86_64", "-kernel", "gato.bin",
        "-drive", f"file=rootfs.img,format=raw,if=ide",
        "-serial", "stdio", "-nographic"
    ]
    
    # 3. Esperar prompt do shell busybox
    # 4. Executar: ls, echo, ps, cat /etc/os-release
    # 5. Se tudo OK, exit 0
```

### Etapa 6: Validação End-to-End

```bash
make ports-toolchain   # Build toolchain
make ports-base        # Build musl + busybox
make ports-mkrootfs    # Gerar imagem
make ports-test        # Bootar em QEMU e validar
```

Critério de sucesso (M0): `make run` → shell busybox ash funcional com `ls`, `cat`, `ps`, `echo`.

---

## Checklist de Implementação

- [ ] Criar `kernel/include/` com headers públicos (syscall.h, vfs.h, net.h)
- [ ] Criar `felinos-ports/base/busybox/patches/` com patches de syscall ABI
- [ ] Criar `felinos-ports/base/busybox/` tarball download/extract/patch funcional
- [ ] Validar que patches musl 0001-0003 aplicam cleanly
- [ ] Executar `make ports-toolchain` com sucesso (cross-compiler funcional)
- [ ] Executar `make ports-base` com sucesso (musl + busybox buildados)
- [ ] Criar `tools/runtests_userspace.py` para testar boot + shell
- [ ] Executar `make ports-mkrootfs` + `make ports-test` end-to-end
- [ ] Validar M0: busybox ash shell functional após boot

---

## Riscos e Mitigações

| Risco | Probabilidade | Mitigação |
|-------|---------------|-----------|
| GCC 14.1.0 não compila com headers FelinOS | Média | Usar `--without-headers` no bootstrap, instalar musl headers depois |
| Patches musl não aplicam (mudança de versão) | Média | Atualizar patches conforme necessário, usar `patch -p1 --dry-run` |
| musl não linkava contra kernel syscalls | Alta | Validar ABI com `hello.c` sample antes de busybox |
| busybox config incompatível com musl | Média | Começar com `defconfig` reduzido, adicionar applets incrementalmente |
| QEMU não bootar rootfs (sem /init correto) | Alta | Garantir `userspace/init` aponta para busybox init |

---

*Documento gerado automaticamente — atualizar conforme progresso.*
