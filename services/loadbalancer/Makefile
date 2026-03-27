.PHONY: all configure build run test test-asan test-tsan clean

# Default just builds the project
all: build

# 1. Generate the standard Ninja build files
configure:
	cmake -B build -G Ninja

# 2. Compile!
build: configure
	cmake --build build

# 3. Run the binary (Will automatically compile if changes were made)
run: build
	./build/loadbalancer

# 4. Standard Unit Testing (No sanitizers)
test: configure
	cmake --build build --target proxy_tests
	./build/proxy_tests

# 5. Testing under AddressSanitizer (Memory Bounds / Overflows)
test-asan:
	cmake -B build_asan -G Ninja -DENABLE_ASAN=ON
	cmake --build build_asan --target proxy_tests
	./build_asan/proxy_tests

# 6. Testing under ThreadSanitizer (Data Races in RCU / Lock-Free paths)
test-tsan:
	cmake -B build_tsan -G Ninja -DENABLE_TSAN=ON
	cmake --build build_tsan --target proxy_tests
	./build_tsan/proxy_tests

# Wipe everything explicitly including the sanitizer builds
clean:
	rm -rf build build_asan build_tsan
