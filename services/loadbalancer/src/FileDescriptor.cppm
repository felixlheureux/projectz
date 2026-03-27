// The Global Module Fragment: used to safely include legacy C/C++ headers before the module begins
module;

#include <unistd.h>
#include <stdexcept>

// Formally declare this file as the FileDescriptor Module
export module FileDescriptor;

// Export the class so it is visible to anything that imports this module
export class FileDescriptor {
private:
    int fd_;

public:
    explicit FileDescriptor(int descriptor) : fd_(descriptor) {
        if (fd_ < 0) {
            throw std::runtime_error("Invalid file descriptor initialized.");
        }
    }

    ~FileDescriptor() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) {
                ::close(fd_);
            }
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept {
        return fd_;
    }
};
