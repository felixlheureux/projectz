.PHONY: all configure build run clean

# Default just builds the project
all: build

# 1. Generate the Ninja build files (Only needed if you delete the build folder)
configure:
	cmake -B build -G Ninja

# 2. Compile!
build:
	cmake --build build

# 3. Run the binary (Will automatically compile if changes were made)
run: build
	./build/loadbalancer

# Wipe everything
clean:
	rm -rf build
