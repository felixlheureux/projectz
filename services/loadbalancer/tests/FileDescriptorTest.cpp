#include <gtest/gtest.h>
#include <unistd.h>
#include <fcntl.h>

import FileDescriptor;

// Test 1: Move Semantics Validation
TEST(FileDescriptorTest, MoveConstructorSetsSourceToNegativeOne) {
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0) << "Failed to create dummy pipe";
    
    // Create RAII wrapper holding the read end
    FileDescriptor source(pipefd[0]);
    EXPECT_EQ(source.get(), pipefd[0]);
    
    // Trigger the Move Constructor
    FileDescriptor destination(std::move(source));
    
    // Validate Move Semantics successfully hollowed out the source object
    EXPECT_EQ(source.get(), -1);
    EXPECT_EQ(destination.get(), pipefd[0]);
    
    // Clean up the write end; destination destructor will close the read end
    close(pipefd[1]);
}

// Test 2: Destructor Execution Validation
TEST(FileDescriptorTest, DestructorClosesFileDescriptorSuccessfully) {
    int dummy_fd;
    {
        int pipefd[2];
        ASSERT_EQ(pipe(pipefd), 0);
        
        FileDescriptor wrapper(pipefd[0]);
        dummy_fd = pipefd[0];
        close(pipefd[1]); // Close write end
        
        // Assert the FD is currently valid
        EXPECT_NE(fcntl(dummy_fd, F_GETFD), -1);
        
    } // wrapper falls out of scope here
    
    // Assert the OS explicitly considers the FD closed (RAII executed)
    EXPECT_EQ(fcntl(dummy_fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}
