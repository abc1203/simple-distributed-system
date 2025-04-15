#include <iostream>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdlib>
#include <cerrno>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <mount_point>" << std::endl;
        return EXIT_FAILURE;
    }

    std::string mountPoint = argv[1];
    std::string filePath = mountPoint + "/testfile.txt";
    
    // Open file with O_WRONLY | O_CREAT | O_TRUNC, mode 0644
    int fd = open(filePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open failed");
        exit(EXIT_FAILURE);
    }
    
    // Retrieve file descriptor flags using fcntl
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        perror("fcntl F_GETFL failed");
        close(fd);
        exit(EXIT_FAILURE);
    }
    printf("Opened file with flags: 0x%x\n", flags);
    
    // Write a message to the file
    const char *msg = "Hello, World!\n";
    ssize_t written = write(fd, msg, strlen(msg));
    if (written < 0) {
        perror("write failed");
        close(fd);
        exit(EXIT_FAILURE);
    } else {
        printf("Wrote %zd bytes.\n", written);
    }
    
    // Close the file
    close(fd);
    return 0;
}
