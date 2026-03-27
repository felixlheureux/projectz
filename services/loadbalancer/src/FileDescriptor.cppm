// The Global Module Fragment: used to safely include legacy C/C++ headers before the module begins
module;

#include <unistd.h>
#include <stdexcept>

// Formally declare this file as the FileDescriptor Module
export module FileDescriptor;

// Export the class so it is visible to anything that imports this module
export class FileDescriptor {
private:
    int fd;

public:
    explicit FileDescriptor(int descriptor) : fd(descriptor) {
        if (fd < 0) {
            throw std::runtime_error("Invalid file descriptor initialized.");
        }
    }

    ~FileDescriptor() {
        if (fd >= 0) {
            close(fd);
        }
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept : fd(other.fd) {
        other.fd = -1;
    }

    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            if (fd >= 0) {
                close(fd);
            }
            fd = other.fd;
            other.fd = -1;
        }
        return *this;
    }

    [[nodiscard]] int get() const {
        return fd;
    }
};
