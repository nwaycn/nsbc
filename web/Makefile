# ============================================================
# nsbc-web (Rust sidecar) build Makefile
# ============================================================
#
# Usage:
#   make fetch         online dep fetch (disable vendor -> rebuild lockfile -> fetch)
#   make vendor        online vendor (fetch + cargo vendor)
#   make build         default intranet mode build (zero external crates, offline)
#   make build-acme    public ACME mode build (must run make vendor first)
#   make run           default intranet mode start
#   make run-acme      public ACME mode start
#   make clean         cargo clean
#
# Note:
#   - cargo fetch / generate-lockfile does not support --features
#     lockfile always contains all deps in Cargo.toml (including optional certon)
#     only build/check/run decides whether to include based on --features
#   - --features goes AFTER the cargo subcommand name:
#     ✅ cargo build --features acme
#     ❌ cargo --features acme build
#
# ============================================================

CARGO       := cargo
RELEASE_DIR := target/release
BINARY      := $(RELEASE_DIR)/nsbc-web
FEATURES    :=
OFFLINE     := --offline --locked

# auto-detect whether .cargo/config.toml has vendor config
VENDOR_CFG  := .cargo/config.toml
VENDOR_BAK  := .cargo/config.toml.bak

.PHONY: all fetch vendor build build-acme run run-acme clean check check-acme

# ===== 1. online preparation (first time or after Cargo.toml changes) =====
# cargo generate-lockfile auto-parses all deps in Cargo.toml (including optional certon)
#
# fetch (default): preserve existing Cargo.lock if present, only regenerate when absent.
#   → Keeps reproducible builds; does NOT pull latest versions on every run.
#   → Use make fetch-refresh after editing Cargo.toml / bumping deps to force a fresh resolve.
fetch:
	@echo "=== temporarily disable vendor config (online mode) ==="
	@if [ -f $(VENDOR_CFG) ]; then \
		cp $(VENDOR_CFG) $(VENDOR_BAK); \
		rm -f $(VENDOR_CFG); \
		echo "  saved → $(VENDOR_BAK)"; \
	else \
		echo "  no $(VENDOR_CFG) — skip"; \
	fi
	@if [ -f Cargo.lock ]; then \
		echo "=== Cargo.lock present — skip generate-lockfile, cargo fetch only ==="; \
	else \
		echo "=== no Cargo.lock — running cargo generate-lockfile ==="; \
		$(CARGO) generate-lockfile; \
	fi
	$(CARGO) fetch
	@echo "=== restore vendor config ==="
	@if [ -f $(VENDOR_BAK) ]; then \
		mv $(VENDOR_BAK) $(VENDOR_CFG); \
		echo "  restored $(VENDOR_CFG)"; \
	fi
	@echo "✅ fetch done (Cargo.lock preserved)"

# Force-fetch: delete Cargo.lock + fully regenerate (use after Cargo.toml edits)
fetch-refresh:
	@echo "=== FORCE rebuild Cargo.lock ==="
	rm -f Cargo.lock
	@$(MAKE) fetch

vendor: fetch
	@echo "=== clear old vendor ==="
	rm -rf vendor
	@echo "=== cargo vendor ==="
	$(CARGO) vendor vendor --versioned-dirs
	@echo "✅ vendor done (vendor/ directory generated)"

# ===== 2. compile =====
all: build

build:
	@echo "=== cargo build --release (default intranet mode) ==="
	$(CARGO) build --release $(OFFLINE)
	@echo "✅ build done → $(BINARY)"
	@echo "=== copy to ../ ==="
	cp -f target/release/nsbc-web ../.
	cp -rf static ../.
	@echo "✅ install done (binary + static copied to parent directory)"

build-acme:
	@echo "=== cargo build --release --features acme (public ACME) ==="
	$(CARGO) build --release $(OFFLINE) --features acme
	@echo "✅ build-acme done → $(BINARY)  (certon TLS-ALPN-01 compiled)"
	@echo "=== copy to ../ ==="
	cp -f target/release/nsbc-web ../.
	cp -rf static ../.
	@echo "✅ install done (binary + static copied to parent directory)"

# ===== 3. quick check (no binary output) =====
check:
	$(CARGO) check --release $(OFFLINE)

check-acme:
	$(CARGO) check --release $(OFFLINE) --features acme

# ===== 4. run =====
run: build
	@echo "=== start nsbc-web (default intranet mode) ==="
	./$(BINARY)

run-acme: build-acme
	@echo "=== start nsbc-web (public ACME TLS-ALPN-01) ==="
	./$(BINARY)

# ===== 5. clean =====
clean:
	$(CARGO) clean
	@echo "✅ cargo clean done"
