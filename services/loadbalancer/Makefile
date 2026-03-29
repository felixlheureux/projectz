.PHONY: all configure build run test test-asan test-tsan clean \
       release run-release setup tune load-test monitor profile

# Default just builds the project
all: build

# Scripts live two levels up at the repo root.
SCRIPTS_DIR := ../../scripts

# ──────────────── Development ────────────────

# 1. Generate the standard Ninja build files
configure:
	cmake -B build -G Ninja

# 2. Compile
build: configure
	cmake --build build

# 3. Run the binary
run: build
	./build/loadbalancer

# ──────────────── Testing ────────────────

# 4. Standard Unit + Integration Tests (no sanitizers)
test: configure
	cmake --build build --target proxy_tests
	./build/proxy_tests

# 5. Tests under AddressSanitizer
test-asan:
	cmake -B build_asan -G Ninja -DENABLE_ASAN=ON
	cmake --build build_asan --target proxy_tests
	./build_asan/proxy_tests

# 6. Tests under ThreadSanitizer
test-tsan:
	cmake -B build_tsan -G Ninja -DENABLE_TSAN=ON
	cmake --build build_tsan --target proxy_tests
	./build_tsan/proxy_tests

# ──────────────── Production / Load Testing ────────────────

# 7. Build optimized release binary (-O3 -flto)
release:
	cmake -B build_release -G Ninja -DCMAKE_BUILD_TYPE=Release
	cmake --build build_release

# 8. Run the release binary
run-release: release
	./build_release/loadbalancer

# 9. Install build dependencies (run once on a fresh machine)
#    The Ubuntu 24.04 apt package (liburing-dev 2.5) ships a STUB
#    io_uring_prep_cmd_sock that discards level/optname/optval/optlen.
#    We MUST compile from upstream to get full SOCKET_URING_OP_SETSOCKOPT
#    support required for the async SO_LINGER bypass.
setup:
	@echo "[setup] Removing stale apt liburing packages..."
	sudo apt-get remove -y liburing-dev liburing2 2>/dev/null || true
	@echo "[setup] Cloning upstream liburing..."
	rm -rf /tmp/liburing-upstream
	git clone --depth 1 https://github.com/axboe/liburing.git /tmp/liburing-upstream
	@echo "[setup] Building and installing upstream liburing..."
	cd /tmp/liburing-upstream && ./configure && make -C src -j$$(nproc)
	cd /tmp/liburing-upstream && sudo make install
	sudo ldconfig
	@echo "[setup] Done.  Verify: pkg-config --modversion liburing"

# 10. Apply kernel tuning (requires sudo)
tune:
	sudo bash $(SCRIPTS_DIR)/tune_kernel.sh

# 11. Self-contained CPS load test (builds proxy + spammer, starts backend, cleans up)
#     Usage: make load-test [DURATION=30]
load-test:
	bash $(SCRIPTS_DIR)/load_test.sh $(DURATION)

# 12. Monitor the running proxy in real-time (requires sudo)
monitor:
	sudo bash $(SCRIPTS_DIR)/monitor.sh

# 13. CPU profile the running proxy (requires sudo)
profile:
	sudo bash $(SCRIPTS_DIR)/profile.sh

# ──────────────── Cleanup ────────────────

clean:
	rm -rf build build_asan build_tsan build_release load_test_results
