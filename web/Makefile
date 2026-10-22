# ============================================================
# nsbc-web (Rust 边车) 构建 Makefile
# ============================================================
#
# 用法：
#   make fetch         联网拉依赖（禁用 vendor → 重建 lockfile → fetch）
#   make vendor        联网 vendor（fetch + cargo vendor）
#   make build         默认内网模式编译（零外部 crate，离线）
#   make build-acme    公网 ACME 模式编译（需先 make vendor）
#   make run           默认内网模式启动
#   make run-acme      公网 ACME 模式启动
#   make clean         cargo clean
#
# 注意：
#   - cargo fetch / generate-lockfile 不支持 --features
#     lockfile 永远包含 Cargo.toml 里写的所有依赖（含 optional certon）
#     只有 build/check/run 才根据 --features 决定是否 include
#   - --features 放在 cargo 子命令名之后：
#     ✅ cargo build --features acme
#     ❌ cargo --features acme build
#
# ============================================================

CARGO       := cargo
RELEASE_DIR := target/release
BINARY      := $(RELEASE_DIR)/nsbc-web
FEATURES    :=
OFFLINE     := --offline --locked

# 自动检测 .cargo/config.toml 是否存在 vendor 配置
VENDOR_CFG  := .cargo/config.toml
VENDOR_BAK  := .cargo/config.toml.bak

.PHONY: all fetch vendor build build-acme run run-acme clean check check-acme

# ===== 1. 联网准备（首次或 Cargo.toml 改过才跑）=====
# cargo generate-lockfile 自动解析 Cargo.toml 里所有 deps（包括 optional certon）
fetch:
	@echo "=== 临时禁用 vendor 配置（联网模式）==="
	@if [ -f $(VENDOR_CFG) ]; then \
		cp $(VENDOR_CFG) $(VENDOR_BAK); \
		rm -f $(VENDOR_CFG); \
		echo "  saved → $(VENDOR_BAK)"; \
	else \
		echo "  no $(VENDOR_CFG) — skip"; \
	fi
	@echo "=== 重建 Cargo.lock（联网下载所有 deps）==="
	rm -f Cargo.lock
	$(CARGO) generate-lockfile
	$(CARGO) fetch
	@echo "=== 恢复 vendor 配置 ==="
	@if [ -f $(VENDOR_BAK) ]; then \
		mv $(VENDOR_BAK) $(VENDOR_CFG); \
		echo "  restored $(VENDOR_CFG)"; \
	fi
	@echo "✅ fetch 完成（Cargo.lock 已更新）"

vendor: fetch
	@echo "=== 清空旧 vendor ==="
	rm -rf vendor
	@echo "=== cargo vendor ==="
	$(CARGO) vendor vendor --versioned-dirs
	@echo "✅ vendor 完成（vendor/ 目录已生成）"

# ===== 2. 编译 =====
all: build

build:
	@echo "=== cargo build --release（默认内网模式）==="
	$(CARGO) build --release $(OFFLINE)
	@echo "✅ build 完成 → $(BINARY)"
	@echo "=== 复制到 ../ ==="
	cp -f target/release/nsbc-web ../.
	cp -rf static ../.
	@echo "✅ install 完成（二进制 + static 已复制到上一级目录）"

build-acme:
	@echo "=== cargo build --release --features acme（公网 ACME）==="
	$(CARGO) build --release $(OFFLINE) --features acme
	@echo "✅ build-acme 完成 → $(BINARY)  (certon TLS-ALPN-01 已编译)"
	@echo "=== 复制到 ../ ==="
	cp -f target/release/nsbc-web ../.
	cp -rf static ../.
	@echo "✅ install 完成（二进制 + static 已复制到上一级目录）"

# ===== 3. 快速 check（不产出二进制）=====
check:
	$(CARGO) check --release $(OFFLINE)

check-acme:
	$(CARGO) check --release $(OFFLINE) --features acme

# ===== 4. 运行 =====
run: build
	@echo "=== 启动 nsbc-web（默认内网模式）==="
	./$(BINARY)

run-acme: build-acme
	@echo "=== 启动 nsbc-web（公网 ACME TLS-ALPN-01）==="
	./$(BINARY)

# ===== 5. 清理 =====
clean:
	$(CARGO) clean
	@echo "✅ cargo clean 完成"
