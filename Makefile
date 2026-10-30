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

UPSTREAM_URL := https://codeberg.org/dnkl/foot.git
# The port's own files, i.e. not changes to upstream's
PORT_FILES := ':!macos' ':!.github' ':!Makefile' ':!AGENTS.md' ':!CLAUDE.md'

VERSION = $(shell git describe --tags --always 2>/dev/null)

##@ Start

.PHONY: run
run: build ## Build and run foot (debug build: assertions, unit tests at startup)
	./$(DEBUG_DIR)/macos/foot $(ARGS)

.PHONY: run-release
run-release: release ## Build and run the optimized build
	./$(RELEASE_DIR)/macos/foot $(ARGS)

.PHONY: open
open: release ## Build foot.app and open it, like the Finder would
	open $(APP)

.PHONY: install
install: release ## Build foot.app and install it to /Applications
	rm -rf $(INSTALLED_APP)
	ditto $(APP) $(INSTALLED_APP)
	@$(LSREGISTER) -f $(INSTALLED_APP)
	@if pgrep -xq foot; then echo 'Installed. Running instances keep the old version until restarted.'; fi

.PHONY: uninstall
uninstall: ## Remove foot.app from /Applications
	rm -rf $(INSTALLED_APP)

##@ Build

.PHONY: build
build: $(DEBUG_DIR)/build.ninja ## Build the debug version into build/
	$(NINJA) -C $(DEBUG_DIR)

.PHONY: release
release: $(RELEASE_DIR)/build.ninja ## Build the optimized version and foot.app into build-release/
	$(NINJA) -C $(RELEASE_DIR)

.PHONY: pgo
pgo: ## Like release, with profile guided optimization, as in releases (a window opens)
	macos/pgo.sh $(RELEASE_DIR) $(RELEASE_OPTIONS)

.PHONY: setup
setup: ## Configure both builds again (after changing meson options)
	$(MESON) setup --reconfigure $(DEBUG_DIR) $(MESON_OPTIONS) --buildtype=debug
	$(MESON) setup --reconfigure $(RELEASE_DIR) $(RELEASE_OPTIONS) -Db_lto=true

.PHONY: clean
clean: ## Remove the compiled files, keep the configuration
	@for dir in $(DEBUG_DIR) $(RELEASE_DIR); do \
		if [[ -f $$dir/build.ninja ]]; then echo "$(NINJA) -C $$dir clean"; $(NINJA) -C $$dir clean >/dev/null; fi; \
	done

.PHONY: distclean
distclean: ## Remove both build directories
	rm -rf $(DEBUG_DIR) $(RELEASE_DIR) $(RELEASE_DIR)-pgo $(SANITIZE_DIR)

##@ Test

.PHONY: test
test: build ## Run foot's tests: the configuration, and the unit tests
	$(MESON) test -C $(DEBUG_DIR) config --print-errorlogs
	@# Debug builds run foot's unit tests at startup
	./$(DEBUG_DIR)/macos/foot --version

.PHONY: smoke
smoke: build ## Run foot for real: output, footclient, the quake terminal (windows open)
	macos/smoke-test.py $(DEBUG_DIR)/macos

.PHONY: sanitize
sanitize: $(SANITIZE_DIR)/build.ninja ## All of the above, with AddressSanitizer and UBSan, in build-sanitize/
	$(NINJA) -C $(SANITIZE_DIR)
	UBSAN_OPTIONS=$(SANITIZE_UBSAN) $(MESON) test -C $(SANITIZE_DIR) config --print-errorlogs
	macos/smoke-test.py $(SANITIZE_DIR)/macos

.PHONY: check-config
check-config: build ## Check your foot.ini (or CONFIG=path)
	./$(DEBUG_DIR)/macos/foot --check-config $(if $(CONFIG),--config='$(CONFIG)')
	@echo 'The configuration is valid.'

.PHONY: bench
bench: pgo ## Benchmark foot against kitty and Ghostty (macos/bench; ~10 minutes, windows open)
	macos/bench/bench.py $(ARGS)

.PHONY: crash
crash: ## Open foot's newest crash report in Console
	@report=$$(ls -t ~/Library/Logs/DiagnosticReports/foot*.ips 2>/dev/null | head -1); \
	if [[ -z "$$report" ]]; then echo 'No crash reports, good.'; exit 0; fi; \
	echo "$$report"; \
	open -a Console "$$report"

##@ Release

.PHONY: dist
dist: ## Build foot.app for distribution: universal, notarized, zipped (no release)
	macos/dist.sh $(VERSION)

.PHONY: publish
publish: ## Publish a release: GitHub, Sparkle, Homebrew (asks for everything)
	@macos/publish.sh

.PHONY: sparkle-key
sparkle-key: ## Create the key that updates are signed with, in the keychain (once)
	macos/dist.sh --fetch-sparkle
	build-release/sparkle-$$(sed -n 's/^SPARKLE_VERSION=//p' macos/dist.sh)/bin/generate_keys --account foot-macos

##@ Upstream

.PHONY: upstream
upstream: ## List the upstream commits that aren't merged yet
	@git remote get-url upstream >/dev/null 2>&1 || git remote add upstream $(UPSTREAM_URL)
	@git fetch --quiet --tags upstream master
	@count=$$(git rev-list --count HEAD..upstream/master); \
	if [[ $$count -eq 0 ]]; then echo "Up to date with upstream ($$(git describe --tags --always upstream/master))."; exit 0; fi; \
	echo "$$count upstream commit(s) to merge, up to $$(git describe --tags --always upstream/master) (make sync):"; \
	git log --oneline --no-decorate HEAD..upstream/master | sed 's/^/  /'

.PHONY: sync
sync: ## Merge upstream foot, then build and test (macos/sync-upstream.sh)
	macos/sync-upstream.sh

.PHONY: port-diff
port-diff: ## Show which upstream files the port changes, and how much
	@git remote get-url upstream >/dev/null 2>&1 || git remote add upstream $(UPSTREAM_URL)
	@git fetch --quiet --tags upstream master
	@git diff --stat upstream/master...HEAD -- . $(PORT_FILES)

##@ GitHub

.PHONY: ci
ci: ## Show the latest GitHub Actions runs
	gh run list --limit 8

.PHONY: ci-watch
ci-watch: ## Follow the newest GitHub Actions run until it's done
	@id=$$(gh run list --limit 1 --json databaseId --jq '.[0].databaseId'); \
	gh run watch "$$id" --exit-status

.PHONY: ci-sync
ci-sync: ## Run the upstream sync workflow on GitHub now (it runs every Monday)
	gh workflow run upstream-sync.yml
	@echo 'Started. make ci-watch follows it.'

.PHONY: repo
repo: ## Open the repository on GitHub
	gh repo view --web

##@ Setup

.PHONY: deps
deps: ## Install the build tools with Homebrew (meson, ninja, bison)
	brew install meson ninja bison

.PHONY: doctor
doctor: ## Check the build tools and print their versions
	@status=0; \
	for tool in $(MESON) $(NINJA) /opt/homebrew/opt/bison/bin/bison xcrun; do \
		if command -v $$tool >/dev/null; then \
			printf '  %-8s %s\n' "$${tool##*/}" "$$($$tool --version 2>&1 | head -1)"; \
		else \
			printf '  %-8s missing (make deps)\n' "$${tool##*/}"; status=1; \
		fi; \
	done; \
	printf '  %-8s %s\n' clang "$$(clang --version | head -1)"; \
	printf '  %-8s %s\n' macOS "$$(sw_vers -productVersion)"; \
	command -v rsvg-convert >/dev/null || echo '  (optional: brew install librsvg, for make icon)'; \
	exit $$status

.PHONY: icon
icon: ## Generate the app icon from upstream's logo (needs librsvg)
	macos/icon/make-icon.py

##@ Help

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
	printf "\n  $${d}Variables: ARGS='-e htop' for run and run-release, ARGS='--save' (or --help) for bench, CONFIG=<path> for check-config$${r}\n\n"

# Configures a build directory the first time it's used.
$(DEBUG_DIR)/build.ninja:
	$(MESON) setup $(DEBUG_DIR) $(MESON_OPTIONS) --buildtype=debug

$(SANITIZE_DIR)/build.ninja:
	$(MESON) setup $(SANITIZE_DIR) $(SANITIZE_OPTIONS)

$(RELEASE_DIR)/build.ninja:
	$(MESON) setup $(RELEASE_DIR) $(RELEASE_OPTIONS) -Db_lto=true
