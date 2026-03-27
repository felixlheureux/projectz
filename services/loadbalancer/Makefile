.PHONY: all configure build run test test-asan test-tsan clean \
       release run-release tune load-test monitor profile

# Default just builds the project
all: build

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

# 4. Standard Unit Testing (No sanitizers)
test: configure
	cmake --build build --target proxy_tests
	./build/proxy_tests

# 5. Testing under AddressSanitizer
test-asan:
	cmake -B build_asan -G Ninja -DENABLE_ASAN=ON
	cmake --build build_asan --target proxy_tests
	./build_asan/proxy_tests

# 6. Testing under ThreadSanitizer
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

# 9. Apply kernel tuning (requires sudo)
tune:
	sudo bash scripts/tune_kernel.sh

# 10. Run the load test ramp (proxy must be running)
load-test:
	bash scripts/load_test.sh

# 11. Monitor the running proxy in real-time (requires sudo)
monitor:
	sudo bash scripts/monitor.sh

# 12. CPU profile the running proxy (requires sudo)
profile:
	sudo bash scripts/profile.sh

# ──────────────── Cleanup ────────────────

clean:
	rm -rf build build_asan build_tsan build_release load_test_results
