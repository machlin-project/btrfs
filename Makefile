PYTHON ?= python3
MESON ?= meson
BUILD_DIR ?= .build
MESON_OPTIONS ?=
BUILD_JOBS ?= 4
TEST_JOBS ?= 2

ifeq ($(shell uname -s),Darwin)
ifeq ($(origin CC),default)
CC := xcrun --sdk macosx clang
endif
export CC
endif

.PHONY: all configure build test format check-style fskit kext
all: build
configure:
	$(MESON) setup --reconfigure "$(BUILD_DIR)" $(MESON_OPTIONS)
build: configure
	$(MESON) compile -C "$(BUILD_DIR)" -j $(BUILD_JOBS)
test: build
	env -i PATH="$(PATH)" $(MESON) test -C "$(BUILD_DIR)" --no-rebuild -j $(TEST_JOBS) --print-errorlogs
format:
	$(PYTHON) scripts/format.py
check-style:
	$(PYTHON) scripts/format.py --check
fskit:
	$(PYTHON) scripts/build_fskit.py
kext:
	$(PYTHON) scripts/build_kext.py
