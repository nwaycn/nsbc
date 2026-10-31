# NSBC core daemon build (REQ-ENV/LANG: Linux x86_64, GCC 4.8 compatible)
# =====================================================================
# Copyright 2024-2026 李浩 (Li Hao), 上海宁卫信息技术有限公司
# Contact: 1354608370@qq.com
# Licensed under the Apache License, Version 2.0. See LICENSE in project root.
# =====================================================================
# FreeSWITCH-style dual control:
#   build-time  ./configure [--with-modules=a,b,c] [--without-modules=x,y,z]
#           reads conf/modules.conf -> splits into STATIC/DYNAMIC groups
#           generates .config/config.mk + src/core/builtin_modules.gen.inc + build/mod/mod.load
#   runtime  nsbc.yaml modules.enable / modules.disable section
# =====================================================================
#
#   ./configure                    # read default modules.conf + make
#   make                           # build nsbcd + selected .so
#   make check                     # build and run unit tests
#   make install                   # install nsbcd + .so + conf
#   make clean                     # clean build/ (including .so)
#
# Optional flags (passed via ./configure or env vars):
#   NSBC_WITH_OPENSSL=1   SIP TLS + DTLS-SRTP (-lssl -lcrypto)
#   NSBC_WITH_SRTP=1      libsrtp2 media encryption (-lsrtp2)
#   NSBC_WITH_SQLITE=1    SQLite WAL (-lsqlite3)
#   NSBC_WITH_BCG729=1    G.729 Annex B (bcg729 vendored source)
#   NSBC_WITH_DLOPEN=1    dynamic module loading (default on, off skips building mod_*.so)
#   NSBC_WITH_CURL=1      cURL —— real ASR/TTS for ai_dialog (Whisper + ElevenLabs), pkg-config lookup
#                         when libcurl not installed, set 0 for mock mode injecting silent frames

CC      ?= gcc
BUILD   ?= build
CONFIG_DIR ?= .
NPROC   ?= $(shell nproc 2>/dev/null || echo 2)

# enabled by default (./configure also overrides)
NSBC_WITH_OPENSSL ?= 1
NSBC_WITH_SRTP    ?= 1
NSBC_WITH_BCG729  ?= 1
NSBC_WITH_DLOPEN  ?= 1
NSBC_WITH_CURL    ?= 0   # off by default: production ASR/TTS needs libcurl (remote machine needs pkg-config libcurl)
NSBC_WITH_SQLITE  ?= 0   # architectural hard constraint: C core nsbcd does not access DB; SQLite is on Rust Web side
                         # when set to 0, sqlite3 code in dbal/cdr_wal is skipped by #ifdef NSBC_WITH_SQLITE
                         # when set to 1, system must have libsqlite3-dev and LDLIBS must include -lsqlite3

# =====================================================================
# configure artifacts take precedence (-include allows missing file -> fallback to defaults below)
# =====================================================================
# config.mk lives in .config/ (project root); make clean only deletes build/ not .config/.
# stale gen.inc defense: if builtin_modules.gen.inc exists but config.mk is missing
# (configure artifacts out of sync), auto-delete gen.inc so builtin.c falls back to handwritten.
# Otherwise __has_include picks up the stale gen.inc, causing STATIC modules (like oam) not to register.
CONFIG_MK := $(CONFIG_DIR)/config.mk
ifeq ($(wildcard src/core/builtin_modules.gen.inc),)
    # gen.inc doesn't exist -> builtin.c uses handwritten fallback, OK
else ifeq ($(wildcard $(CONFIG_MK)),)
    $(warning [Makefile] src/core/builtin_modules.gen.inc exists but $(CONFIG_MK) missing — stale configure output, deleting gen.inc)
    $(shell rm -f src/core/builtin_modules.gen.inc)
endif

# =====================================================================
# Auto-configure: if config.mk missing and configure script exists, run it.
# Skip for 'make clean' — cleaning should not regenerate config.
# =====================================================================
HAVE_CONFIGURE := $(wildcard ./configure)
HAVE_CONFIG_MK := $(wildcard $(CONFIG_MK))

ifneq ($(filter clean,$(MAKECMDGOALS)),clean)
ifeq ($(HAVE_CONFIG_MK),)
ifeq ($(HAVE_CONFIGURE),./configure)
$(info [Makefile] $(CONFIG_MK) not found — auto-running ./configure ...)
_configure_rc := $(shell mkdir -p $(CONFIG_DIR) && ./configure >&2; echo $$?)
$(if $(filter 0,$(_configure_rc)),,$(error ./configure failed (exit $(_configure_rc)) — fix dependencies or run manually))
endif
endif
endif

-include $(CONFIG_MK)

# =====================================================================
# Architectural hard constraints (highest priority — use override keyword, config.mk cannot override)
# =====================================================================
override NSBC_WITH_SQLITE   := 0   # C core does not touch DB (Rust Web sidecar)
override NSBC_WITH_OPENSSL  := 1   # WebRTC / DTLS-SRTP hard dep, must be enabled
override NSBC_WITH_SRTP     := 1   # DTLS-SRTP media encryption, pairing OpenSSL must be enabled

# CPPFLAGS: filter out sqlite3 macros from config.mk, then force append:
#   - NSBC root header path (config.mk cannot override)
#   - -DNSBC_WITH_OPENSSL -DNSBC_WITH_SRTP —— baked directly in override layer,
#     do not depend on whether later ifeq ($(NSBC_WITH_OPENSSL),1) conditional block triggers
override CPPFLAGS := $(strip $(filter-out -DNSBC_WITH_SQLITE,$(CPPFLAGS)))
override CPPFLAGS += -I src/include -I3rd/include
override CPPFLAGS += -I3rd/bcg729/include -I3rd/bcg729/src
override CPPFLAGS += -DHAVE_BCG729
override CPPFLAGS += -DNSBC_WITH_OPENSSL -DNSBC_WITH_SRTP
override CPPFLAGS += -DNSBC_NO_DB_MODULE   # architectural hard constraint: C core does not touch DB

# linker libs: override layer ensures base libs are complete, don't recompute to preserve existing libs in L160
override LDLIBS += -lm -lpcre2-8 -lpthread -lrt
# filter out sqlite3-related (architectural hard constraint), but leave other libs untouched
override LDLIBS := $(strip $(filter-out -lsqlite3 $(SQLITE_LDLIBS),$(LDLIBS)))
override LDLIBS += -lssl -lcrypto
override LDLIBS += $(shell pkg-config --libs libsrtp2 2>/dev/null || echo -lsrtp2)

# ---- diagnostic output (let user see if config.mk loaded, and final values of key vars) ----
$(info ============== NSBC Makefile Config Dump ==============)
$(info CONFIG_MK       = $(CONFIG_MK))
$(info HAS_CONFIG_MK   = $(if $(wildcard $(CONFIG_MK)),YES,NO))
$(info NSBC_WITH_SQLITE= $(NSBC_WITH_SQLITE))
$(info NSBC_WITH_OPENSSL= $(NSBC_WITH_OPENSSL))
$(info NSBC_WITH_SRTP  = $(NSBC_WITH_SRTP))
$(info CPPFLAGS        = $(CPPFLAGS))
$(info LDLIBS          = $(LDLIBS))
$(info LDFLAGS         = $(LDFLAGS))
$(info ========================================================)


# config.mk generated by ./configure, contains POST_BUILD_COPY etc.
# ?= fallback when configure not run: default copies build/mod -> ./mod
POST_BUILD_COPY ?= rm -rf ./mod && if [ -d build/mod ]; then cp -a build/mod ./mod; fi

SRC_DIRS := $(shell find src -type d)

# ---- Makefile default SRCS (used when configure not run) ----
# configure directly sets STATIC_MOD_SRCS to override the ?= line below
# exclusion rules:
#   src/db/*           — architectural hard constraint: C core does not touch DB (exclude even if NSBC_WITH_SQLITE=0, safe)
#   src/media/rec/*     — media recording encoding (optional)
#   src/mod/*           — dynamic module candidates (STATIC module list specified manually in SRCS_MOD_STATIC)
#   src/media/codec/g*  — codec impl files (g711/g729): g711 hard-linked for recorder (added separately in SRCS_BASE),
#                         g729 goes to mod_codecs.so dynamic module; media_codec.c (registry + transcode engine)
#                         must be kept (core/moat_bridge/obs_control hard-depend on codec_registry_*/transcode_*)
SRCS_CORE ?= $(shell find src -name '*.c' ! -name 'nsbc_cli.c' \
              ! -path 'src/mod/*' ! -path 'src/media/rec/*' \
              ! -path 'src/db/*' ! -path 'src/media/codec/g*.c')
# default hand-picked STATIC modules (fallback when no configure)
# note: intentionally excludes src/mod/codecs/*——codecs go DYNAMIC, g711.c hard-linked separately into main binary
SRCS_MOD_STATIC ?= $(shell (find src/mod/endpoints src/mod/outbound_reg -name '*.c'; \
                           find src/mod/dialplans -name '*.c'; \
                           find src/mod/billing -name '*.c'; \
                           find src/mod/recording -name '*.c'; \
                           find src/mod/callcenter -name '*.c'; \
                           find src/media/rec -name '*.c') | sort | uniq)

# STATIC_MOD_SRCS comes from configure's config.mk; use SRCS_MOD_STATIC when no configure
STATIC_MOD_SRCS ?= $(SRCS_MOD_STATIC)

# ---- SRCS_BASE final assembly with defensive cleanup ----
# config.mk may hardcode source lists (bypassing ?= defaults), so filter-out must be applied after final synthesis:
#   1) src/db/dbal.c            —— C core does not touch DB (architectural constraint), sqlite3 full path with no callers
#   2) src/db/dbal_module.c     —— same as above (config.mk may include dbal as a module)
#   3) src/mod/codecs/*.c      —— codec impl files go DYNAMIC module chain, hard-link causes multiple definition
#   4) src/media/codec/g*.c    —— same as above (g711 hard-linked separately, g729 goes DYNAMIC)
# ⚠️ note: src/mod/billing/cdr_wal.c must be KEPT —— billing.c unconditionally calls cdr_wal_*
# functions (cdr_wal_create/append/destroy/replay), billing is a STATIC module link dep.
# sqlite3 code in cdr_wal.c is all inside #ifdef NSBC_WITH_SQLITE, as long as override sets NSBC_WITH_SQLITE=0
# the preprocessor strips the sqlite3 branch, leaving pure-text WAL backend, does not touch DB at all.
STATIC_MOD_SRCS := $(filter-out src/db/%.c src/mod/codecs/%.c src/media/codec/g%.c,$(STATIC_MOD_SRCS))
SRCS_CORE := $(filter-out src/db/%.c src/media/codec/g%.c,$(SRCS_CORE))

# recorder.c (main binary) hard-depends on g711_ulaw_decode / g711_alaw_decode,
# but configure puts codecs in DYNAMIC, g711.c only goes into mod_codecs.so —— main binary link loses symbols.
# Force g711.c to be compiled into nsbcd, decoupled from configure layering.
# Pick whichever actually exists (may be src/media/codec/g711.c or src/mod/codecs/g711.c,
# maybe both —— $(firstword $(wildcard ...)) picks the first).
G711_SRC := $(firstword $(wildcard src/media/codec/g711.c) $(wildcard src/mod/codecs/g711.c))
SRCS_BASE := $(sort $(SRCS_CORE) \
                    $(STATIC_MOD_SRCS) \
                    $(if $(G711_SRC),$(G711_SRC)) \
                    3rd/cjson/cJSON.c)

# ---- final fallback: filter out from SRCS_BASE again ----
# even if filter-out above missed them (config.mk directly sets SRCS_BASE), also exclude db/ here
# cdr_wal.c INTENTIONALLY kept —— billing STATIC module depends on it
SRCS_BASE := $(filter-out src/db/%.c,$(SRCS_BASE))

# vendored bcg729 —— compile into mod_codecs.so, no longer into nsbcd
BCG729_SRCS := $(wildcard 3rd/bcg729/src/*.c)
SRCS := $(SRCS_BASE)

LIB_SRCS := $(filter-out src/core/main.c,$(SRCS))
HDRS     := $(shell find src/include -name '*.h')

CFLAGS  += -std=gnu99 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-array-bounds \
           -Wno-missing-braces -Wno-missing-field-initializers -O2 -g -pthread
GCC_VER := $(shell $(CC) -dumpfullversion -dumpversion 2>/dev/null | awk -F. '{print $$1*100+$$2}')
ifeq ($(shell test $(GCC_VER) -ge 409 && echo 1),1)
CFLAGS += -Wno-format-truncation
endif
ifeq ($(shell test $(GCC_VER) -ge 800 && echo 1),1)
CFLAGS += -Wno-stringop-truncation -Wno-stringop-overflow -Wno-stringop-overread
endif

CPPFLAGS += -I src/include -I3rd/include
LDLIBS  += -lpthread -lrt -lm -lpcre2-8

# dynamic module loading: main binary needs -export-dynamic for .so to reference nsbc global symbols
ifeq ($(NSBC_WITH_DLOPEN),1)
CPPFLAGS += -DNSBC_WITH_DLOPEN
LDLIBS   += -ldl
LDFLAGS  += -Wl,--export-dynamic
endif

ifeq ($(NSBC_WITH_OPENSSL),1)
CPPFLAGS += -DNSBC_WITH_OPENSSL
LDLIBS   += -lssl -lcrypto
endif
ifeq ($(NSBC_WITH_SRTP),1)
_SRTP_CFLAGS := $(shell pkg-config --cflags libsrtp2 2>/dev/null)
_SRTP_LIBS   := $(shell pkg-config --libs   libsrtp2 2>/dev/null)
_SRTP_LIBDIR := $(shell pkg-config --variable=libdir libsrtp2 2>/dev/null)
CPPFLAGS += -DNSBC_WITH_SRTP $(_SRTP_CFLAGS)
LDLIBS   += $(if $(_SRTP_LIBS),$(_SRTP_LIBS),-lsrtp2)
ifneq ($(_SRTP_LIBDIR),)
LDFLAGS  += -Wl,-rpath,$(_SRTP_LIBDIR)
endif
endif
ifeq ($(NSBC_WITH_SQLITE),1)
CPPFLAGS += -DNSBC_WITH_SQLITE
# SQLITE_LDLIBS in config.mk comes from pkg-config (more precise), fallback to hardcoded -lsqlite3
LDLIBS   += $(if $(SQLITE_LDLIBS),$(SQLITE_LDLIBS),-lsqlite3)
endif
ifeq ($(NSBC_WITH_BCG729),1)
CPPFLAGS += -DHAVE_BCG729 -I3rd/bcg729/include -I3rd/bcg729/src
$(BUILD)/3rd/bcg729/src/%.o: CFLAGS += -Wno-error -Wno-unused-parameter -Wno-maybe-uninitialized
endif

# sanitizer variants
ifeq ($(SANITIZE),asan)
CFLAGS  += -fsanitize=address -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address
else ifeq ($(SANITIZE),tsan)
CFLAGS  += -fsanitize=thread -fno-omit-frame-pointer -fPIE
LDFLAGS += -fsanitize=thread -pie
else ifeq ($(SANITIZE),ubsan)
CFLAGS  += -fsanitize=undefined -fno-sanitize-recover=all -Wno-error
LDFLAGS += -fsanitize=undefined
endif

OBJS     := $(patsubst %.c,$(BUILD)/%.o,$(LIB_SRCS))
MAIN_OBJ := $(BUILD)/src/core/main.o
TEST_SRCS := $(sort $(wildcard tests/unit/t_*.c))
TEST_BINS := $(patsubst tests/unit/%.c,$(BUILD)/tests/unit/%,$(TEST_SRCS))

TARGET := $(BUILD)/nsbcd
CLI_SRC := src/tools/nsbc_cli.c
CLI_OBJ := $(BUILD)/src/tools/nsbc_cli.o
CLI_TARGET := $(BUILD)/nsbc_cli

# dynamic module .so targets (DYNAMIC_MODS comes from configure's config.mk)
ifeq ($(NSBC_WITH_DLOPEN),1)
DYNAMIC_SOS := $(patsubst %,$(BUILD)/mod/mod_%.so,$(DYNAMIC_MODS))
endif

# CLI
NSBC_CLI_NO_EDITLINE ?= 0
ifeq ($(NSBC_CLI_NO_EDITLINE),1)
CLI_CPPFLAGS = -DNSBC_CLI_NO_EDITLINE
CLI_LDLIBS   = -lpthread
else
CLI_CPPFLAGS =
CLI_LDLIBS   = -lpthread -ledit
endif

.PHONY: all check clean asan tsan ubsan install cli modules web web-acme web-check web-clean build-acme

# mod.load must be generated before .so build — configure always writes it,
# but as a safety net we also add an explicit Make fallback here.
# configure path: build/mod/mod.load (generated at L665-677 of configure)
# nsbcd reads:   exe_dir/mod/mod.load  (exe_dir = dirname(/proc/self/exe))
# install copy:   $(PREFIX)/etc/nsbc/mod/mod.load
ifeq ($(NSBC_WITH_DLOPEN),1)
all: $(TARGET) $(CLI_TARGET) $(DYNAMIC_SOS) $(BUILD)/mod/mod.load
else
all: $(TARGET) $(CLI_TARGET)
endif

# ================================================================
# dynamic module build rule: each mod_xxx.so independently built with -shared -fPIC
# MOD_SRC_xxx variables from configure specify source paths (falls back to find by default)
# $* stem is mod_billing -> strip prefix mod_ -> billing to build MOD_SRC_ variable name
#
# only ai_dialog module needs cURL —— match by MOD_STEM:
#   is ai_dialog and NSBC_WITH_CURL=1 -> pkg-config libcurl + -DHAVE_REAL_ASR_TTS
#   other modules don't get curl headers / -lcurl (avoid all .so depending on libcurl.so)
# ================================================================
ifeq ($(NSBC_WITH_DLOPEN),1)
MOD_STEM = $(subst mod_,,$*)

ifeq ($(NSBC_WITH_CURL),1)
AI_DIALOG_LDLIBS   := $(shell pkg-config --libs libcurl 2>/dev/null)
AI_DIALOG_CPPFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null) -DHAVE_REAL_ASR_TTS
else
AI_DIALOG_LDLIBS   :=
AI_DIALOG_CPPFLAGS :=
endif

# only ai_dialog module gets curl flag
# only av module gets FFmpeg LDLIBS (configure sets AV_LDLIBS when FFmpeg detected)
# only codecs module gets bcg729 compatibility flag (third-party code has warnings)
$(BUILD)/mod/%.so:
	@mkdir -p $(dir $@)
	@echo "  MOD $@"
ifeq ($(MOD_STEM),ai_dialog)
	$(CC) -shared -fPIC -fvisibility=hidden \
	      $(CFLAGS) $(CPPFLAGS) $(AI_DIALOG_CPPFLAGS) -o $@ \
	      $(MOD_SRC_$(MOD_STEM)) \
	      $(LDLIBS) $(AI_DIALOG_LDLIBS)
else ifeq ($(MOD_STEM),av)
	$(CC) -shared -fPIC -fvisibility=hidden \
	      $(CFLAGS) $(CPPFLAGS) -o $@ \
	      $(MOD_SRC_$(MOD_STEM)) \
	      $(LDLIBS) $(AV_LDLIBS)
else ifeq ($(MOD_STEM),codecs)
	$(CC) -shared -fPIC -fvisibility=hidden \
	      $(CFLAGS) -Wno-error -Wno-unused-parameter -Wno-maybe-uninitialized \
	      $(CPPFLAGS) -o $@ \
	      $(MOD_SRC_$(MOD_STEM)) \
	      $(LDLIBS)
else
	$(CC) -shared -fPIC -fvisibility=hidden \
	      $(CFLAGS) $(CPPFLAGS) -o $@ \
	      $(MOD_SRC_$(MOD_STEM)) \
	      $(LDLIBS)
endif
endif

modules: $(DYNAMIC_SOS)

$(TARGET): $(OBJS) $(MAIN_OBJ) | $(DYNAMIC_SOS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)
	cp -f build/nsbcd .
	$(POST_BUILD_COPY)

# Fallback: generate build/mod/mod.load if configure didn't run
# (configure always writes it at $(BUILD)/mod/mod.load; this is just a safety net)
ifeq ($(NSBC_WITH_DLOPEN),1)
$(BUILD)/mod/mod.load:
	@mkdir -p $(dir $@)
	@if [ ! -f $@ ] && [ -n "$(DYNAMIC_MODS)" ]; then \
	  for m in $(DYNAMIC_MODS); do echo "mod_$$m.so" >> $@; done; \
	  echo "  GEN $@ (Make fallback, $(words $(DYNAMIC_MODS)) modules)"; \
	fi
endif

$(CLI_OBJ): $(CLI_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(CLI_CPPFLAGS) -c -o $@ $<

$(CLI_TARGET): $(CLI_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(CLI_LDLIBS)
	cp -f build/nsbc_cli .

$(BUILD)/3rd/cjson/cJSON.o: CPPFLAGS += -I3rd/include/cjson
$(BUILD)/3rd/cjson/cJSON.o: CFLAGS += -Wno-error

$(BUILD)/%.o: %.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(BUILD)/tests/unit/%: tests/unit/%.c $(OBJS) $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -I src/core $(LDFLAGS) -o $@ $< $(OBJS) $(LDLIBS)

check: $(TEST_BINS) $(TARGET)
	@fail=0; for t in $(TEST_BINS); do \
	  echo "== $$t"; $$t || fail=1; \
	done; if [ $$fail -ne 0 ]; then echo "UNIT TESTS FAILED"; exit 1; fi
	@echo "ALL UNIT TESTS PASSED"

asan:
	$(MAKE) --no-print-directory SANITIZE=asan BUILD=$(BUILD)-asan -j$(NPROC) check
tsan:
	$(MAKE) --no-print-directory SANITIZE=tsan BUILD=$(BUILD)-tsan -j$(NPROC) check
ubsan:
	$(MAKE) --no-print-directory SANITIZE=ubsan BUILD=$(BUILD)-ubsan -j$(NPROC) check

PREFIX ?= /usr/local
DESTDIR ?=

# WITH_WEB=1 (default)  → install 同时构建安装 nsbc-web (需要 Rust stable toolchain)
# WITH_WEB=0             → install 只装 C core (无 Rust 环境的服务器)
WITH_WEB ?= 1

# ============================================================
# nsbc-web 快捷目标（根目录直接调 web/Makefile 对应 target）
#   make web          → cd web && make build         (内网默认)
#   make web-acme     → cd web && make build-acme     (公网 ACME)
#   make web-check    → cd web && make check
#   make web-clean    → cd web && make clean
#   make build-acme   → C core all + web-acme (一键全编译 + 产物到根目录)
# 注意: web-acme/build-acme 都会把 nsbc-web + static 复制到项目根，
#       和 nsbcd/nsbc_cli 并列，这样 ./nsbc-web -c conf/nsbc.yaml 直接跑
# ============================================================
web:
	@echo "=== [nsbc-web] build (intranet mode) ==="
	$(MAKE) -C web build
	cp -f web/target/release/nsbc-web .
	cp -rf web/static .
	@echo "✅ nsbc-web + static → ./ (和 nsbcd/nsbc_cli 并列)"

web-acme:
	@echo "=== [nsbc-web] build-acme (public ACME TLS-ALPN-01) ==="
	$(MAKE) -C web build-acme
	cp -f web/target/release/nsbc-web .
	cp -rf web/static .
	@echo "✅ nsbc-web + static → ./ (和 nsbcd/nsbc_cli 并列)"

web-check:
	$(MAKE) -C web check

web-clean:
	$(MAKE) -C web clean

# build-acme: 一键编 C core + web-acme (公网部署最常用)
build-acme: all web-acme

install: $(TARGET) $(CLI_TARGET)
	install -d $(DESTDIR)$(PREFIX)/sbin $(DESTDIR)/etc/nsbc
	install -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/sbin/nsbcd
	install -m 0755 $(CLI_TARGET) $(DESTDIR)$(PREFIX)/sbin/nsbc_cli
	install -m 0644 configs/nsbc.yaml $(DESTDIR)/etc/nsbc/nsbc.yaml
ifeq ($(NSBC_WITH_DLOPEN),1)
	install -d $(DESTDIR)$(PREFIX)/lib/nsbc
	@if [ -d $(BUILD)/mod ]; then \
	  for so in $(BUILD)/mod/*.so; do install -m 0644 $$so $(DESTDIR)$(PREFIX)/lib/nsbc/; done; \
	fi
	install -d $(DESTDIR)$(PREFIX)/etc/nsbc/mod
	@if [ -f $(BUILD)/mod/mod.load ]; then install -m 0644 $(BUILD)/mod/mod.load $(DESTDIR)$(PREFIX)/etc/nsbc/mod/mod.load; fi
endif
ifeq ($(WITH_WEB),1)
	@echo ""
	@echo "=== WITH_WEB=1 → build + install nsbc-web ==="
	$(MAKE) -C web build
	$(MAKE) -C web install PREFIX=$(PREFIX) DESTDIR=$(DESTDIR)
	@echo "✅ nsbc-web 已安装到 $(DESTDIR)$(PREFIX)/web/  (二进制 + static/)"
else
	@echo "WITH_WEB=0 → 跳过 nsbc-web。如需安装: cd web && make install"
endif

clean:
	rm -rf build
	$(MAKE) -C web clean
