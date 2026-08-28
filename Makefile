SHELL := /bin/sh

BUILD_DIR ?= build
BUILD_TYPE ?= Release
PREFIX ?= $(HOME)/.local
SUDO ?= sudo
PACMAN := /usr/bin/pacman
ARCH_RELEASE ?= /etc/arch-release
OS_RELEASE ?= /etc/os-release

DEPENDENCIES := base-devel cmake ninja go qt6-base qt6-svg libarchive curl squashfs-tools podman polkit gnupg openssl

.DEFAULT_GOAL := all
.NOTPARALLEL:
.PHONY: help all check-arch check-user deps configure build test install uninstall clean

help:
	@echo "PacSmith build targets:"
	@echo "  make deps     Install build/runtime dependencies with pacman"
	@echo "  make build    Configure and build PacSmith"
	@echo "  make test     Build and run the automated tests"
	@echo "  make install  Install dependencies, test, and install for this user"
	@echo "  make uninstall Remove files recorded by the last current-user install"
	@echo "  make clean    Clean compiled files in the configured build directory"
	@echo
	@echo "Optional variables: BUILD_DIR=build BUILD_TYPE=Release PREFIX=~/.local SUDO=sudo"

all: build

check-arch:
	@set -eu; \
	is_arch=false; \
	if [ -e "$(ARCH_RELEASE)" ]; then is_arch=true; fi; \
	if [ -r "$(OS_RELEASE)" ]; then \
		. "$(OS_RELEASE)"; \
		case " $${ID:-} $${ID_LIKE:-} " in \
			*" arch "*|*" manjaro "*|*" endeavouros "*|*" garuda "*|*" cachyos "*) is_arch=true ;; \
		esac; \
	fi; \
	if [ "$$is_arch" != true ]; then \
		echo "error: PacSmith currently supports Arch Linux and Arch-based distributions only." >&2; \
		exit 1; \
	fi; \
	if [ ! -x "$(PACMAN)" ]; then \
		echo "error: $(PACMAN) was not found; this does not appear to be an Arch-based system." >&2; \
		exit 1; \
	fi

check-user:
	@if [ "$$(id -u)" -eq 0 ]; then \
		echo "error: run this Makefile as your normal user, not as root." >&2; \
		exit 1; \
	fi

deps: check-arch check-user
	@if ! command -v "$(word 1,$(SUDO))" >/dev/null 2>&1; then \
		echo "error: $(word 1,$(SUDO)) is required for dependency installation (override with SUDO=doas if needed)." >&2; \
		exit 1; \
	fi
	$(SUDO) $(PACMAN) -S --needed $(DEPENDENCIES)

configure: check-arch check-user
	cmake -S . -B "$(BUILD_DIR)" -G Ninja \
		-DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" \
		-DCMAKE_INSTALL_PREFIX="$(PREFIX)" \
		-DBUILD_TESTING=ON

build: configure
	cmake --build "$(BUILD_DIR)" --parallel

test: build
	ctest --test-dir "$(BUILD_DIR)" --output-on-failure

install: check-user deps test
	cmake --install "$(BUILD_DIR)"
	"$(PREFIX)/bin/pacsmith" skill install
	@if command -v update-desktop-database >/dev/null 2>&1; then \
		update-desktop-database "$(PREFIX)/share/applications" || \
			echo "warning: desktop application cache could not be refreshed." >&2; \
	fi
	@if command -v systemctl >/dev/null 2>&1; then \
		systemctl --user daemon-reload || \
			echo "warning: systemd user manager was not available; run 'systemctl --user daemon-reload' later." >&2; \
	fi
	@echo "PacSmith installed for the current user under $(PREFIX)."
	@echo "Run '$(PREFIX)/bin/pacsmith-gui' or add '$(PREFIX)/bin' to PATH."
	@echo "Portable Agent Plugin: $$($(PREFIX)/bin/pacsmith plugin path)"
	@echo "The library daemon is $(PREFIX)/bin/pacsmithd (systemd user unit pacsmithd.service)."
	@if command -v systemctl >/dev/null 2>&1; then \
		systemctl --user try-restart pacsmithd.service || \
			echo "warning: could not restart the running pacsmithd.service." >&2; \
	fi

uninstall: check-user
	@set -eu; \
	manifest="$(BUILD_DIR)/install_manifest.txt"; \
	if [ ! -f "$$manifest" ]; then \
		echo "error: no install manifest at $$manifest; run 'make install' first (or set BUILD_DIR to the build that was installed)." >&2; \
		exit 1; \
	fi; \
	if [ -x "$(PREFIX)/bin/pacsmith" ]; then \
		"$(PREFIX)/bin/pacsmith" skill uninstall || \
			echo "warning: the user Agent Skill was not removed; see the error above." >&2; \
	fi; \
	if command -v systemctl >/dev/null 2>&1; then \
		systemctl --user disable --now pacsmithd.service pacsmith-update.timer pacsmith-tray.service >/dev/null 2>&1 || true; \
		systemctl --user stop pacsmith-update.service >/dev/null 2>&1 || true; \
	fi; \
	while IFS= read -r file; do \
		[ -n "$$file" ] || continue; \
		if [ -e "$$file" ] || [ -L "$$file" ]; then \
			rm -f "$$file"; \
		fi; \
	done < "$$manifest"; \
	rmdir "$(PREFIX)/share/doc/pacsmith" >/dev/null 2>&1 || true; \
	if command -v update-desktop-database >/dev/null 2>&1; then \
		update-desktop-database "$(PREFIX)/share/applications" || \
			echo "warning: desktop application cache could not be refreshed." >&2; \
	fi; \
	if command -v systemctl >/dev/null 2>&1; then \
		systemctl --user daemon-reload || \
			echo "warning: systemd user manager was not available; run 'systemctl --user daemon-reload' later." >&2; \
	fi; \
	echo "PacSmith uninstalled using $$manifest."; \
	echo "Uninstall left both the legacy library under ~/.local/share/pacsmith/projects and any new server data under ~/.local/share/pacsmith/server."

clean: check-arch
	@if [ -f "$(BUILD_DIR)/build.ninja" ]; then \
		cmake --build "$(BUILD_DIR)" --target clean; \
	else \
		echo "Nothing to clean in $(BUILD_DIR)."; \
	fi
