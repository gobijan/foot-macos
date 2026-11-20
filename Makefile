# foot for macOS. `make` or `make help` lists every command.
#
# This Makefile is a frontend only: foot is built with meson
# (meson.build, macos/meson.build). Upstream foot has no Makefile, so this
# file never conflicts with upstream merges.
SHELL := /bin/bash
.DEFAULT_GOAL := help

MESON ?= meson
NINJA ?= ninja
MESON_OPTIONS := --native-file macos/native.ini -Dgrapheme-clustering=enabled

DEBUG_DIR := build
RELEASE_DIR := build-release
RELEASE_OPTIONS := $(MESON_OPTIONS) --buildtype=release
SANITIZE_DIR := build-sanitize
# werror: sprintf() is only deprecated without _FORTIFY_SOURCE, which
# sanitizers turn off
SANITIZE_OPTIONS := $(MESON_OPTIONS) --buildtype=debug -Db_sanitize=address,undefined \
	-Db_lundef=false -Dwerror=false
# As in macos/smoke-test.py; meson test has its own default otherwise
SANITIZE_UBSAN := halt_on_error=1:print_stacktrace=1:suppressions=$(CURDIR)/macos/ubsan.supp
APP := $(RELEASE_DIR)/macos/foot.app
INSTALLED_APP := /Applications/foot.app
LSREGISTER := /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister

VERSION = $(shell git describe --tags --always 2>/dev/null)

##@ Start

.PHONY: run
run: build ## Build and run foot (debug build: assertions, unit tests at startup)
	./$(DEBUG_DIR)/macos/foot $(ARGS)

.PHONY: install
install: release ## Build foot.app and install it to /Applications
	rm -rf $(INSTALLED_APP)
	ditto $(APP) $(INSTALLED_APP)
	@$(LSREGISTER) -f $(INSTALLED_APP)
	@if pgrep -xq foot; then echo 'Installed. Running instances keep the old version until restarted.'; fi

##@ Build

.PHONY: build
build: $(DEBUG_DIR)/build.ninja ## Build the debug version into build/
	$(NINJA) -C $(DEBUG_DIR)

.PHONY: release
release: $(RELEASE_DIR)/build.ninja ## Build the optimized version and foot.app into build-release/
	macos/footd-linux.sh $(RELEASE_DIR)/footd-linux
	$(NINJA) -C $(RELEASE_DIR)

.PHONY: pgo
pgo: ## Like release, with profile guided optimization, as in releases (a window opens)
	macos/pgo.sh $(RELEASE_DIR) $(RELEASE_OPTIONS)

.PHONY: clean
clean: ## Remove all build directories (the next build configures them again)
	rm -rf $(DEBUG_DIR) $(RELEASE_DIR) $(RELEASE_DIR)-pgo $(SANITIZE_DIR) build-x86_64 build-x86_64-pgo

##@ Test

.PHONY: test
test: build ## Run foot's tests: the configuration, sessions, and the unit tests
	$(MESON) test -C $(DEBUG_DIR) config sessions footd --print-errorlogs
	@# Debug builds run foot's unit tests at startup
	./$(DEBUG_DIR)/macos/foot --version

.PHONY: smoke
smoke: build ## Run foot for real, in windows: all smoke tests, or ARGS=splits for one
	macos/smoke-test.py $(DEBUG_DIR)/macos $(ARGS)

.PHONY: sanitize
sanitize: $(SANITIZE_DIR)/build.ninja ## All of the above, with AddressSanitizer and UBSan, in build-sanitize/
	$(NINJA) -C $(SANITIZE_DIR)
	UBSAN_OPTIONS=$(SANITIZE_UBSAN) $(MESON) test -C $(SANITIZE_DIR) config sessions footd --print-errorlogs
	macos/smoke-test.py $(SANITIZE_DIR)/macos

.PHONY: linux
linux: ## Build footd for Linux and run its tests, in a container (Docker; PLATFORM=linux/amd64)
	macos/linux.sh $(PLATFORM)

.PHONY: bench
bench: pgo ## Benchmark foot against kitty and Ghostty (macos/bench; ~10 minutes, windows open)
	macos/bench/bench.py $(ARGS)

##@ Release

.PHONY: dist
dist: ## Build foot.app for distribution: universal, notarized, zipped (no release)
	macos/dist.sh $(VERSION)

.PHONY: publish
publish: ## Publish a release: GitHub, Sparkle, Homebrew (asks for everything)
	@macos/publish.sh

##@ More

.PHONY: sync
sync: ## Merge upstream foot, then build and test (macos/sync-upstream.sh)
	macos/sync-upstream.sh

.PHONY: deps
deps: ## Install the build tools with Homebrew (meson, ninja, bison)
	brew install meson ninja bison

.PHONY: site
site: ## Serve the website (macos/site) at http://localhost:8000
	python3 -m http.server 8000 --bind 127.0.0.1 --directory macos/site

.PHONY: help
help: ## List the commands
	@if [ -t 1 ] && [ -z "$$NO_COLOR" ]; then b='\033[1m'; d='\033[2m'; r='\033[0m'; g='\033[32m'; y='\033[33m'; fi; \
	state() { case $$1 in ok) printf "$${g}●$${r} %s" "$$2" ;; old) printf "$${y}●$${r} %s $${d}(outdated)$${r}" "$$2" ;; *) printf "$${d}○ %s$${r}" "$$2" ;; esac; }; \
	debug=missing; [ -x $(DEBUG_DIR)/macos/foot ] && debug=ok; \
	release=missing; [ -d $(APP) ] && release=ok; \
	installed=missing; if [ -d $(INSTALLED_APP) ]; then installed=old; \
		cmp -s $(APP)/Contents/MacOS/foot $(INSTALLED_APP)/Contents/MacOS/foot && installed=ok; fi; \
	printf '\n'; \
	sh macos/banner.sh "$${b}foot$${r}  $${d}$(VERSION)$${r}" "$${d}The fast, lightweight terminal emulator, native on macOS$${r}" "$${d}make <command> [VARIABLE=value]$${r}" "" \
		"$$(state $$debug 'debug build')   $$(state $$release 'release build')   $$(state $$installed 'installed')"; \
	awk -v b="$$b" -v r="$$r" 'BEGIN { FS = ":.*## "; split("36;172;212 71;180;19 227;196;1 242;175;253 19;194;153 246;43;90 230;230;230", palette, " ") } \
		/^##@/ { c = b == "" ? "" : "\033[38;2;" palette[n++ % 7 + 1] "m"; printf "\n  %s%s%s\n", b, substr($$0, 5), r } \
		/^[a-z0-9-]+:.*## / { printf "    %s%-14s%s %s\n", c, $$1, r, $$2 }' $(MAKEFILE_LIST); \
	printf "\n  $${d}Variables: ARGS='-e htop' for run, ARGS=splits for smoke, ARGS='--save' (or --help) for bench$${r}\n\n"

# Configures a build directory the first time it's used.
$(DEBUG_DIR)/build.ninja:
	$(MESON) setup $(DEBUG_DIR) $(MESON_OPTIONS) --buildtype=debug

$(SANITIZE_DIR)/build.ninja:
	$(MESON) setup $(SANITIZE_DIR) $(SANITIZE_OPTIONS)

$(RELEASE_DIR)/build.ninja:
	$(MESON) setup $(RELEASE_DIR) $(RELEASE_OPTIONS) -Db_lto=true
