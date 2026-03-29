BUILD_DIR=build

all: build

build:
	mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && cmake .. && make

clean:
	rm -rf $(BUILD_DIR)

run: build
	cd $(BUILD_DIR) && ./loadbalancer 5001 192.168.5.2 6000