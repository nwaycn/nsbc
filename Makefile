# NSBC 核心守护进程构建（REQ-ENV/LANG：Linux x86_64，GCC 4.8 兼容）
# =====================================================================
# FreeSWITCH 风格双控：
#   编译时  ./configure [--with-modules=a,b,c] [--without-modules=x,y,z]
#           读 conf/modules.conf → 分 STATIC/DYNAMIC 两组
#           生成 .config/config.mk + src/core/builtin_modules.gen.inc + build/mod/mod.load
#   运行时  nsbc.yaml modules.enable / modules.disable 段
# =====================================================================
#
#   ./configure                    # 读本文件默认 modules.conf + make
#   make                           # 编译 nsbcd + 选中的 .so
#   make check                     # 构建并运行单元测试
#   make install                   # 安装 nsbcd + .so + conf
#   make clean                     # 清 build/（含 .so）
#
# 可选开关（./configure 或环境变量传）：
#   NSBC_WITH_OPENSSL=1   SIP TLS + DTLS-SRTP（-lssl -lcrypto）
#   NSBC_WITH_SRTP=1      libsrtp2 媒体加密（-lsrtp2）
#   NSBC_WITH_SQLITE=1    SQLite WAL（-lsqlite3）
#   NSBC_WITH_BCG729=1    G.729 Annex B（bcg729 vendored 源码）
#   NSBC_WITH_DLOPEN=1    动态模块加载（默认开，关闭后不编 mod_*.so）
#   NSBC_WITH_CURL=1      cURL —— ai_dialog 真 ASR/TTS（Whisper + ElevenLabs），pkg-config 找
#                         不装 libcurl 时可设 0 走 mock 模式注入静音帧

CC      ?= gcc
BUILD   ?= build
CONFIG_DIR ?= .
NPROC   ?= $(shell nproc 2>/dev/null || echo 2)

# 默认启用（./configure 也会覆盖）
NSBC_WITH_OPENSSL ?= 1
NSBC_WITH_SRTP    ?= 1
NSBC_WITH_BCG729  ?= 1
NSBC_WITH_DLOPEN  ?= 1
NSBC_WITH_CURL    ?= 0   # 默认关：生产 ASR/TTS 需要 libcurl（远程机要 pkg-config libcurl）
NSBC_WITH_SQLITE  ?= 0   # 架构硬约束：C 核心 nsbcd 不访问数据库；SQLite 在 Rust Web 边车侧
                         # 设 0 时 dbal/cdr_wal 里的 sqlite3 代码被 #ifdef NSBC_WITH_SQLITE 跳过
                         # 设 1 时必须同时确保系统有 libsqlite3-dev 且 LDLIBS 有 -lsqlite3

# =====================================================================
# configure 产物优先覆盖（-include 允许文件不存在 → fallback 下面手工默认值）
# =====================================================================
# config.mk 放在 .config/（项目根），make clean 只删 build/ 不动它。
# 过期 gen.inc 防御：如果 builtin_modules.gen.inc 存在但 config.mk 不存在
# （说明 configure 产物不同步），自动删除 gen.inc 让 builtin.c 走手写 fallback。
# 否则 __has_include 会 pick 到陈旧 gen.inc，导致 STATIC 模块（如 oam）不注册。
CONFIG_MK := $(CONFIG_DIR)/config.mk
ifeq ($(wildcard src/core/builtin_modules.gen.inc),)
    # gen.inc 不存在 → builtin.c 走手写 fallback，OK
else ifeq ($(wildcard $(CONFIG_MK)),)
    $(warning [Makefile] src/core/builtin_modules.gen.inc exists but $(CONFIG_MK) missing — stale configure output, deleting gen.inc)
    $(shell rm -f src/core/builtin_modules.gen.inc)
endif

-include $(CONFIG_MK)

# =====================================================================
# 架构硬约束（最高优先级 —— 用 override 关键字，config.mk 无法覆盖）
# =====================================================================
override NSBC_WITH_SQLITE   := 0   # C 核心不碰 DB（Rust Web 边车侧）
override NSBC_WITH_OPENSSL  := 1   # WebRTC / DTLS-SRTP 硬依赖，必须开
override NSBC_WITH_SRTP     := 1   # DTLS-SRTP 媒体加密，配套 OpenSSL 必开

# CPPFLAGS：过滤掉 config.mk 里的 sqlite3 宏，然后强制追加：
#   - NSBC 根头文件路径（config.mk 不能覆盖）
#   - -DNSBC_WITH_OPENSSL -DNSBC_WITH_SRTP —— 直接在 override 层打进去，
#     不依赖后面的 ifeq ($(NSBC_WITH_OPENSSL),1) 条件块是否触发
override CPPFLAGS := $(strip $(filter-out -DNSBC_WITH_SQLITE,$(CPPFLAGS)))
override CPPFLAGS += -I src/include -I3rd/include
override CPPFLAGS += -I3rd/bcg729/include -I3rd/bcg729/src
override CPPFLAGS += -DHAVE_BCG729
override CPPFLAGS += -DNSBC_WITH_OPENSSL -DNSBC_WITH_SRTP
override CPPFLAGS += -DNSBC_NO_DB_MODULE   # 架构硬约束：C 核心不碰 DB

# 链接库：override 层确保基础库齐全，不重算吃掉 L160 原有的库
override LDLIBS += -lm -lpcre2-8 -lpthread -lrt
# 过滤 sqlite3 相关（架构硬约束），但不影响其他库
override LDLIBS := $(strip $(filter-out -lsqlite3 $(SQLITE_LDLIBS),$(LDLIBS)))
override LDLIBS += -lssl -lcrypto
override LDLIBS += $(shell pkg-config --libs libsrtp2 2>/dev/null || echo -lsrtp2)

# ---- 诊断输出（让用户看到 config.mk 是否加载、关键变量最终值）----
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


# config.mk 由 ./configure 生成，包含 POST_BUILD_COPY 等变量
# 没跑 configure 时 ?= 兜底：默认复制 build/mod → ./mod
POST_BUILD_COPY ?= rm -rf ./mod && if [ -d build/mod ]; then cp -a build/mod ./mod; fi

SRC_DIRS := $(shell find src -type d)

# ---- Makefile 默认 SRCS（没跑 configure 时用这组）----
# configure 会直接设 STATIC_MOD_SRCS 覆盖下面 ?= 行
# 排除原则：
#   src/db/*           — 架构硬约束：C 核心不碰 DB（即使 NSBC_WITH_SQLITE=0 也排除，保险）
#   src/media/rec/*     — 媒体录制编码（可选）
#   src/mod/*           — 动态模块候选（STATIC 模块清单在 SRCS_MOD_STATIC 里手工指定）
#   src/media/codec/g*  — codec 实现文件（g711/g729）：g711 硬链给 recorder 用（在 SRCS_BASE 里单独加），
#                         g729 归 mod_codecs.so 动态模块；media_codec.c（注册中心 + transcode 引擎）
#                         必须保留（core/moat_bridge/obs_control 硬依赖 codec_registry_*/transcode_*）
SRCS_CORE ?= $(shell find src -name '*.c' ! -name 'nsbc_cli.c' \
              ! -path 'src/mod/*' ! -path 'src/media/rec/*' \
              ! -path 'src/db/*' ! -path 'src/media/codec/g*.c')
# 默认手工选的 STATIC 模块（没 configure 时的 fallback）
# 注意：故意不含 src/mod/codecs/*——codecs 走 DYNAMIC 模块链，g711.c 单独硬链进主二进制
SRCS_MOD_STATIC ?= $(shell (find src/mod/endpoints src/mod/outbound_reg -name '*.c'; \
                           find src/mod/dialplans -name '*.c'; \
                           find src/mod/billing -name '*.c'; \
                           find src/mod/recording -name '*.c'; \
                           find src/mod/callcenter -name '*.c'; \
                           find src/media/rec -name '*.c') | sort | uniq)

# STATIC_MOD_SRCS 来自 configure 的 config.mk；没 configure 时用 SRCS_MOD_STATIC
STATIC_MOD_SRCS ?= $(SRCS_MOD_STATIC)

# ---- SRCS_BASE 最终集合并防御性清洗 ----
# config.mk 可能硬编码源文件列表（绕过 ?= 默认值），所以必须在最终合成后用 filter-out 兜底：
#   1) src/db/dbal.c            —— C 核心不碰 DB（架构硬约束），sqlite3 全路径且无调用方
#   2) src/db/dbal_module.c     —— 同上（config.mk 可能把 dbal 当作模块加进来）
#   3) src/mod/codecs/*.c      —— codec 实现文件走 DYNAMIC 模块链，硬进会 multiple definition
#   4) src/media/codec/g*.c    —— 同上（g711 单独硬链，g729 走 DYNAMIC）
# ⚠️ 注意：src/mod/billing/cdr_wal.c 必须**保留**——billing.c 无条件调用 cdr_wal_*
# 函数（cdr_wal_create/append/destroy/replay），billing 是 STATIC 模块链接依赖。
# cdr_wal.c 里的 sqlite3 代码全在 #ifdef NSBC_WITH_SQLITE 内，只要 override 设 NSBC_WITH_SQLITE=0
# 预处理器就会剪掉 sqlite3 分支，剩纯文本 WAL 后端，完全不碰 DB。
STATIC_MOD_SRCS := $(filter-out src/db/%.c src/mod/codecs/%.c src/media/codec/g%.c,$(STATIC_MOD_SRCS))
SRCS_CORE := $(filter-out src/db/%.c src/media/codec/g%.c,$(SRCS_CORE))

# recorder.c（主二进制）硬依赖 g711_ulaw_decode / g711_alaw_decode，
# 但 configure 把 codecs 归 DYNAMIC，g711.c 只进 mod_codecs.so —— 主二进制链接会丢符号。
# 这里强制把 g711.c 编进 nsbcd，与 configure 分层解耦。
# 取实际存在的那份（可能在 src/media/codec/g711.c 或 src/mod/codecs/g711.c，
# 也可能两份都在——$(firstword $(wildcard ...)) 取第一份）。
G711_SRC := $(firstword $(wildcard src/media/codec/g711.c) $(wildcard src/mod/codecs/g711.c))
SRCS_BASE := $(sort $(SRCS_CORE) \
                    $(STATIC_MOD_SRCS) \
                    $(if $(G711_SRC),$(G711_SRC)) \
                    3rd/cjson/cJSON.c)

# ---- 最终兜底：SRCS_BASE 再硬排除一次 ----
# 即使上面 filter-out 没拦住（config.mk 直接设 SRCS_BASE），这里也必排 db/
# cdr_wal.c **故意保留**——billing STATIC 模块依赖它
SRCS_BASE := $(filter-out src/db/%.c,$(SRCS_BASE))

# vendored bcg729 —— 编进 mod_codecs.so，不再编进 nsbcd
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

# 动态模块加载：主二进制需要 -export-dynamic 让 .so 引用 nsbc 全局符号
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
# config.mk 里 SQLITE_LDLIBS 来自 pkg-config（更精确），fallback 到硬编码 -lsqlite3
LDLIBS   += $(if $(SQLITE_LDLIBS),$(SQLITE_LDLIBS),-lsqlite3)
endif
ifeq ($(NSBC_WITH_BCG729),1)
CPPFLAGS += -DHAVE_BCG729 -I3rd/bcg729/include -I3rd/bcg729/src
$(BUILD)/3rd/bcg729/src/%.o: CFLAGS += -Wno-error -Wno-unused-parameter -Wno-maybe-uninitialized
endif

# sanitizer 变体
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

# 动态模块 .so 目标（DYNAMIC_MODS 来自 configure 的 config.mk）
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

.PHONY: all check clean asan tsan ubsan install cli modules

all: $(TARGET) $(CLI_TARGET) $(DYNAMIC_SOS)

# ================================================================
# 动态模块构建规则：每个 mod_xxx.so 独立 -shared -fPIC 编译
# configure 生成的 MOD_SRC_xxx 变量指定源码路径（默认 fallback 用 find）
# $* stem 是 mod_billing → 去掉前缀 mod_ 变成 billing 拼 MOD_SRC_ 变量名
#
# 只有 ai_dialog 模块需要 cURL —— 按 MOD_STEM 判断：
#   是 ai_dialog 且 NSBC_WITH_CURL=1 → pkg-config libcurl + -DHAVE_REAL_ASR_TTS
#   其他模块不加 curl 头 / -lcurl（避免所有 .so 都依赖 libcurl.so）
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

# 只给 ai_dialog 模块加 curl flag
# 只给 codecs 模块加 bcg729 兼容 flag（第三方代码有 warning）
$(BUILD)/mod/%.so:
	@mkdir -p $(dir $@)
	@echo "  MOD $@"
ifeq ($(MOD_STEM),ai_dialog)
	$(CC) -shared -fPIC -fvisibility=hidden \
	      $(CFLAGS) $(CPPFLAGS) $(AI_DIALOG_CPPFLAGS) -o $@ \
	      $(MOD_SRC_$(MOD_STEM)) \
	      $(LDLIBS) $(AI_DIALOG_LDLIBS)
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

$(TARGET): $(OBJS) $(MAIN_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)
	cp -f build/nsbcd .
	$(POST_BUILD_COPY)

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
	@if [ -f build/mod.load ]; then install -m 0644 build/mod.load $(DESTDIR)$(PREFIX)/etc/nsbc/mod/mod.load; fi
endif

clean:
	rm -rf build
