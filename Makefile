.PHONY: all help node-build rocksdb-build synced wait-synced full-node pruned-node rocksdb-full-node rocksdb-pruned-node portable-windows portable-linux portable-all static-windows static-linux static-all

all: help

ifneq ($(OS),Windows_NT)
UNAME := $(shell uname -s)
endif
ifeq ($(OS),Windows_NT)
WSL_DISTRO ?= Ubuntu-22.04
WINDOWS_PS := powershell -ExecutionPolicy Bypass -File
NODE_PRESET ?= windows-clang-static-release-headless
NODE_BUILD_DIR ?= out/build/windows-clang-static-release
NODE_BUILD_CONFIG ?= --config Release
NODE_BIN ?= $(NODE_BUILD_DIR)/geocoin-node.exe

portable-windows:
	$(WINDOWS_PS) scripts/package_portable_windows.ps1

portable-linux:
	wsl -d $(WSL_DISTRO) -- bash -lc 'set -euo pipefail; cd "$$(wslpath -a "$(CURDIR)")"; tmp="$$(mktemp scripts/package_portable_linux.XXXXXX.sh)"; tr -d "\r" < scripts/package_portable_linux.sh > "$$tmp"; chmod +x "$$tmp"; "$$tmp"; rm -f "$$tmp"'
else
ifeq ($(UNAME),Darwin)
NODE_PRESET ?= macos-release-headless
NODE_BUILD_DIR ?= out/build/macos-release
NODE_BUILD_CONFIG ?=
NODE_BIN ?= $(NODE_BUILD_DIR)/geocoin-node
GUI_PRESET ?= macos-arm64-release
BUNDLE_PROFILE ?= user
else
NODE_PRESET ?= linux-release-headless
NODE_BUILD_DIR ?= out/build/linux-release
NODE_BUILD_CONFIG ?=
NODE_BIN ?= $(NODE_BUILD_DIR)/geocoin-node
endif

portable-windows:
	@echo "Run portable-windows from a Windows host with PowerShell."
	@exit 1

portable-linux:
	@tmp="$$(mktemp scripts/package_portable_linux.XXXXXX.sh)"; \
	tr -d '\r' < scripts/package_portable_linux.sh > "$$tmp"; \
	chmod +x "$$tmp"; \
	"$$tmp"; \
	rm -f "$$tmp"
ifeq ($(UNAME),Darwin)

.PHONY: build-macos portable-macos portable-macos-profile
build-macos:
	cmake --preset $(GUI_PRESET) -S .
	cmake --build --preset $(GUI_PRESET) --parallel

.PHONY: build-macos-sdk
build-macos-sdk:
	cmake --preset macos-arm64-release -S libgeo
	cmake --build libgeo/out/build/macos-arm64-release --parallel

portable-macos: build-macos
	GEO_BUILD_DIR="$(CURDIR)/out/build/$(GUI_PRESET)" bash scripts/package_portable_macos.sh --profile "$(BUNDLE_PROFILE)"

ifeq ($(BUNDLE_PROFILE),sdk)
portable-macos: build-macos-sdk
endif

# make portable-macos-profile=user — profile-specific bundle (user|admin|sdk).
# GNU Make 3.81 parses the '=' form as a variable assignment; the plain
# portable-macos-profile goal is the goal users get when typing that name.
portable-macos-profile:
	@$(MAKE) portable-macos BUNDLE_PROFILE="$${portable_macos_profile:-user}"
endif
endif

ifeq ($(UNAME),Darwin)
portable-all: build-macos build-macos-sdk
	@set -e; for profile in user admin sdk; do \
		GEO_BUILD_DIR="$(CURDIR)/out/build/$(GUI_PRESET)" bash scripts/package_portable_macos.sh --profile "$$profile"; \
	done
else
portable-all: portable-windows portable-linux
endif

static-windows: portable-windows
static-linux: portable-linux
static-all: portable-all

NETWORK ?= degree
DATA_DIR ?= data
STATUS_INTERVAL ?= 30
ACTIVATION_MANIFEST ?=
NODE_ARGS ?=
NODE_CMAKE_ARGS ?=
DB_BACKEND ?=
DB_PROFILE ?=
DB_WRITE_BUFFER_MB ?=
DB_CACHE_MB ?=
DB_MAX_BACKGROUND_JOBS ?=
DB_UNSAFE_DISABLE_WAL_DURING_IBD ?=
NODE_TYPE ?=
PRUNED_BLOCKS ?= 1000
PRUNED_KEEP_OLD_BLOCKS ?=
PRUNED_KEEP_OLD_INDEXES ?=
NODE_DB_ARGS = $(if $(strip $(DB_BACKEND)),--db-backend "$(DB_BACKEND)",)
NODE_DB_ARGS += $(if $(strip $(DB_PROFILE)),--db-profile "$(DB_PROFILE)",)
NODE_DB_ARGS += $(if $(strip $(DB_WRITE_BUFFER_MB)),--db-write-buffer-mb "$(DB_WRITE_BUFFER_MB)",)
NODE_DB_ARGS += $(if $(strip $(DB_CACHE_MB)),--db-cache-mb "$(DB_CACHE_MB)",)
NODE_DB_ARGS += $(if $(strip $(DB_MAX_BACKGROUND_JOBS)),--db-max-background-jobs "$(DB_MAX_BACKGROUND_JOBS)",)
NODE_DB_ARGS += $(if $(strip $(DB_UNSAFE_DISABLE_WAL_DURING_IBD)),--db-unsafe-disable-wal-during-ibd,)
NODE_TYPE_ARGS = $(if $(strip $(NODE_TYPE)),--node-type "$(NODE_TYPE)",)
NODE_TYPE_ARGS += $(if $(and $(filter pruned,$(NODE_TYPE)),$(strip $(PRUNED_BLOCKS))),--pruned-blocks "$(PRUNED_BLOCKS)",)
NODE_TYPE_ARGS += $(if $(and $(filter pruned,$(NODE_TYPE)),$(strip $(PRUNED_KEEP_OLD_BLOCKS))),--pruned-keep-old-blocks,)
NODE_TYPE_ARGS += $(if $(and $(filter pruned,$(NODE_TYPE)),$(strip $(PRUNED_KEEP_OLD_INDEXES))),--pruned-keep-old-indexes,)
NODE_COMMON_ARGS = --data-dir "$(DATA_DIR)" --network "$(NETWORK)" --status-interval "$(STATUS_INTERVAL)"
ifneq ($(strip $(ACTIVATION_MANIFEST)),)
NODE_COMMON_ARGS += --activation-manifest "$(ACTIVATION_MANIFEST)"
endif
NODE_COMMON_ARGS += $(NODE_DB_ARGS)
NODE_COMMON_ARGS += $(NODE_TYPE_ARGS)
RPC_URL ?= http://127.0.0.1:3033/
RPC_USER ?=
RPC_PASS ?=
RPC_PASS_FILE ?=
WAIT_TIMEOUT ?= 300
WAIT_INTERVAL ?= 5
WAIT_SYNC_ARGS = --url "$(RPC_URL)" --timeout "$(WAIT_TIMEOUT)" --interval "$(WAIT_INTERVAL)"
ifneq ($(strip $(RPC_USER)),)
WAIT_SYNC_ARGS += --user "$(RPC_USER)"
endif
ifneq ($(strip $(RPC_PASS_FILE)),)
WAIT_SYNC_ARGS += --password-file "$(RPC_PASS_FILE)"
endif
ifneq ($(strip $(RPC_PASS)),)
WAIT_SYNC_ARGS += --password "$(RPC_PASS)"
endif

node-build:
	cmake --preset $(NODE_PRESET) -S . $(NODE_CMAKE_ARGS)
	cmake --build "$(NODE_BUILD_DIR)" $(NODE_BUILD_CONFIG) --parallel

rocksdb-build: node-build

synced: wait-synced

wait-synced:
	@scripts/wait_synced.py $(WAIT_SYNC_ARGS)

full-node: NODE_TYPE = full
full-node: node-build
	"$(NODE_BIN)" $(NODE_COMMON_ARGS) --archive $(NODE_ARGS)

pruned-node: NODE_TYPE = pruned
pruned-node: node-build
	"$(NODE_BIN)" $(NODE_COMMON_ARGS) $(NODE_ARGS)

rocksdb-full-node: DB_BACKEND = rocksdb
rocksdb-full-node: DB_PROFILE = fast-sync
rocksdb-full-node: full-node

rocksdb-pruned-node: DB_BACKEND = rocksdb
rocksdb-pruned-node: DB_PROFILE = fast-sync
rocksdb-pruned-node: pruned-node

help:
	@echo "GeoCoin build entrypoints:"
	@echo "  make node-build        - Build the headless GeoCoin node"
	@echo "  make rocksdb-build     - Build node with RocksDB storage backend"
	@echo "  make synced            - Wait until a running node reports synced"
	@echo "  make wait-synced       - Alias target used by synced"
	@echo "  make full-node         - Build/start an archive full-history node"
	@echo "  make pruned-node       - Build/start a Pruned node retaining recent blocks"
	@echo "  make rocksdb-full-node - Build/start archive node with RocksDB chainstate"
	@echo "  make rocksdb-pruned-node - Build/start Pruned node with RocksDB chainstate"
	@echo "  make portable-windows   - Build/package true static Windows bundle"
	@echo "  make portable-linux     - Build/package self-contained Linux bundle"
ifeq ($(UNAME),Darwin)
	@echo "  make portable-macos     - Build/package self-contained macOS arm64 bundle"
	@echo "  make portable-all       - Build user, admin and SDK bundles on macOS"
else
	@echo "  make portable-all       - Build/package both bundles"
endif
	@echo "  make static-windows     - Alias for portable-windows"
	@echo "  make static-linux       - Alias for portable-linux"
	@echo "  make static-all         - Alias for portable-all"
	@echo ""
	@echo "Node targets can be customized with NETWORK=..., DATA_DIR=..., STATUS_INTERVAL=...,"
	@echo "ACTIVATION_MANIFEST=..., DB_BACKEND=rocksdb, DB_PROFILE=safe|fast-sync|unsafe-benchmark,"
	@echo "DB_WRITE_BUFFER_MB=..., DB_CACHE_MB=..., DB_MAX_BACKGROUND_JOBS=..., DB_UNSAFE_DISABLE_WAL_DURING_IBD=1,"
	@echo "NODE_TYPE=full|pruned, PRUNED_BLOCKS=1000, PRUNED_KEEP_OLD_BLOCKS=1, PRUNED_KEEP_OLD_INDEXES=1, NODE_ARGS='...'. Example:"
	@echo "  make rocksdb-full-node DATA_DIR=/var/lib/geocoin-rocksdb NODE_ARGS='--rpc --rpc-user geocoin --rpc-pass-file /etc/geocoin/rpc.pass'"
	@echo "Sync wait can be customized with RPC_URL=..., RPC_USER=..., RPC_PASS_FILE=...,"
	@echo "WAIT_TIMEOUT=..., WAIT_INTERVAL=.... Example:"
	@echo "  make synced RPC_USER=geocoin RPC_PASS_FILE=/etc/geocoin/rpc.pass"
	@echo ""
	@echo "Windows release uses ClangCL + vcpkg x64-windows-static-release."
	@echo "The Windows package must not contain project/Qt/OpenSSL/Curl DLLs."
	@echo "Linux release remains a portable Ubuntu 22.04 bundle with runtime deps included."
ifeq ($(UNAME),Darwin)
	@echo "macOS release targets Apple Silicon on macOS 14+. Run bash scripts/bootstrap-macos.sh --profile release first."
endif
