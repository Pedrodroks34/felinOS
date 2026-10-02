# FelinOS Ports Configuration
# ===========================
# Configuração global para cross-compilação e build de ports
#
# Apenas variáveis e helpers aqui -- os targets (toolchain, base, clean,
# sysroot-prepare, etc.) ficam só no Makefile. Antes este arquivo também os
# definia, duplicados quase ao pé da letra do Makefile; como o Makefile faz
# `include config.mk` e DEPOIS redefine os mesmos nomes de target, o make
# sempre ficava com a versão do Makefile e descartava esta em silêncio (dá
# pra ver isso com `make -n`: "warning: overriding recipe for target ...").
# Então as duas cópias nunca estavam de fato ambas em uso, só uma delas por
# vez dependendo da ordem -- e a cópia descartada aqui ainda referenciava
# $(TOOLCHAIN_SUBDIRS)/$(BASE_SUBDIRS)/etc., que só existem no Makefile
# (nunca são definidos aqui), então mesmo se ela "vencesse" quebraria.

# -----------------------------------------------------------------------------
# Diretórios
# -----------------------------------------------------------------------------
# Precisa vir ANTES de qualquer variável abaixo que o referencie (CROSS_PREFIX,
# TARGET_CFLAGS, ...): com `:=` o make expande o lado direito IMEDIATAMENTE,
# na hora da atribuição, não quando a variável é usada depois. Com
# PORTS_DIR/SYSROOT/KERNEL_DIR definidos só lá embaixo (como antes), toda
# variável que os referenciava mais acima pegava eles vazios -- por exemplo
# CROSS_PREFIX virava "/toolchain/bin/x86_64-felinos-" (faltando o caminho
# do projeto na frente, um caminho absoluto a partir da raiz do sistema) em
# vez do caminho real dentro de felinos-ports/.
#
# PORTS_DIR também NÃO pode vir de $(CURDIR): cada port (base/musl,
# base/busybox, ...) faz `include ../../config.mk` a partir do SEU PRÓPRIO
# diretório, e CURDIR é o diretório de onde o make foi invocado -- ou seja,
# PORTS_DIR virava "felinos-ports/base/musl" em vez de "felinos-ports" (só
# "funcionava" por coincidência quando rodado direto da raiz de
# felinos-ports/). Em vez disso, usamos onde ESTE arquivo está de verdade:
# MAKEFILE_LIST tem o caminho usado no `include` (ex.: "../../config.mk"),
# então dir()+abspath() dá o diretório real de config.mk não importa de
# qual profundidade ele foi incluído.
PORTS_DIR       := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
KERNEL_DIR      := $(PORTS_DIR)/../kernel
SYSROOT         := $(PORTS_DIR)/../userspace
BUILD_DIR       := $(PORTS_DIR)/build
DIST_DIR        := $(PORTS_DIR)/dist
PKG_DIR         := $(PORTS_DIR)/pkg
DOWNLOAD_DIR    := $(PORTS_DIR)/downloads

# -----------------------------------------------------------------------------
# Arquitetura e Target
# -----------------------------------------------------------------------------
ARCH            := x86_64
TARGET          := x86_64-felinos
KERNEL_VERSION  := 0.2

# -----------------------------------------------------------------------------
# Toolchain
# -----------------------------------------------------------------------------
# Prefixo do cross-compiler (será construído em toolchain/)
CROSS_PREFIX    := $(PORTS_DIR)/toolchain/bin/$(TARGET)-

CC              := $(CROSS_PREFIX)gcc
CXX             := $(CROSS_PREFIX)g++
AS              := $(CROSS_PREFIX)as
LD              := $(CROSS_PREFIX)ld
AR              := $(CROSS_PREFIX)ar
RANLIB          := $(CROSS_PREFIX)ranlib
STRIP           := $(CROSS_PREFIX)strip
OBJCOPY         := $(CROSS_PREFIX)objcopy
OBJDUMP         := $(CROSS_PREFIX)objdump
NM              := $(CROSS_PREFIX)nm
READELF         := $(CROSS_PREFIX)readelf

# Flags base para target FelinOS (sem libc, freestanding)
TARGET_CFLAGS   := -m64 -mcmodel=small -mno-red-zone -mgeneral-regs-only \
                   -std=gnu11 -O2 -pipe \
                   -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
                   -fcf-protection=none -fno-asynchronous-unwind-tables \
                   -fno-builtin -fno-strict-aliasing \
                   -Wall -Wextra -Wno-unused-parameter \
                   -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
                   -nostdinc -isystem $(SYSROOT)/usr/include \
                   -I$(KERNEL_DIR)/include

TARGET_CXXFLAGS := $(TARGET_CFLAGS) -std=gnu++20 -fno-exceptions -fno-rtti

TARGET_LDFLAGS  := -nostdlib -static -Wl,--build-id=none -Wl,-z,max-page-size=0x1000

# -----------------------------------------------------------------------------
# Versões dos pacotes base
# -----------------------------------------------------------------------------
MUSL_VERSION    := 1.2.5
BUSYBOX_VERSION := 1.36.1
LINUX_HEADERS_VERSION := 6.6
BINUTILS_VERSION := 2.42
GCC_VERSION     := 14.1.0
GMP_VERSION     := 6.3.0
MPFR_VERSION    := 4.2.1
MPC_VERSION     := 1.3.1
ISL_VERSION     := 0.24

# -----------------------------------------------------------------------------
# Paralelismo
# -----------------------------------------------------------------------------
JOBS            ?= $(shell nproc 2>/dev/null || echo 4)

# -----------------------------------------------------------------------------
# Utilitários
# -----------------------------------------------------------------------------
WGET            := wget -c --no-check-certificate
GIT             := git
TAR             := tar
PATCH           := patch -p1
MAKE            := make -j$(JOBS)

# -----------------------------------------------------------------------------
# Funções helper
# -----------------------------------------------------------------------------
define download
	@mkdir -p $(DOWNLOAD_DIR)
	@if [ ! -f $(DOWNLOAD_DIR)/$(notdir $(1)) ]; then \
		echo "  DOWNLOAD  $(notdir $(1))"; \
		$(WGET) -O $(DOWNLOAD_DIR)/$(notdir $(1)) $(1); \
	fi
endef

define extract
	@echo "  EXTRACT   $(notdir $(1))"
	@mkdir -p $(BUILD_DIR)/$(2)
	@$(TAR) -xf $(DOWNLOAD_DIR)/$(1) -C $(BUILD_DIR)/$(2) --strip-components=1
endef

define patch_port
	@for p in $(PORT_DIR)/patches/*.patch; do \
		if [ -f "$$p" ]; then \
			echo "  PATCH     $$p"; \
			cd $(BUILD_DIR)/$(PORT_NAME) && $(PATCH) < "$$p"; \
		fi \
	done
endef
