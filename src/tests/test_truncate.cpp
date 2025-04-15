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

    // Step 1: Create or overwrite the file and write known content.
    const char* content = "The quick brown fox jumps over the lazy dog";
    size_t contentLength = std::strlen(content);

    int fd = open(filePath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        std::cerr << "Error opening file for writing: " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }

    ssize_t bytesWritten = write(fd, content, contentLength);
    if (bytesWritten < 0) {
        std::cerr << "Error writing to file: " << strerror(errno) << std::endl;
        close(fd);
        return EXIT_FAILURE;
    }
    std::cout << "Wrote " << bytesWritten << " bytes to '" << filePath << "'." << std::endl;
    close(fd);

    // Step 2: Verify file size before truncation.
    struct stat st;
    if (stat(filePath.c_str(), &st) != 0) {
        std::cerr << "Error statting file: " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "File size before truncate: " << st.st_size << " bytes." << std::endl;
    if (static_cast<size_t>(st.st_size) != contentLength) {
        std::cerr << "Error: expected file size " << contentLength << " but got " << st.st_size << "." << std::endl;
        return EXIT_FAILURE;
    }

    // Step 3: Truncate the file.
    off_t newSize = 10;  // For example, truncate to 10 bytes.
    if (truncate(filePath.c_str(), newSize) != 0) {
        std::cerr << "Error truncating file: " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "Truncate called: file size set to " << newSize << " bytes." << std::endl;

    // Step 4: Verify file size after truncation.
    if (stat(filePath.c_str(), &st) != 0) {
        std::cerr << "Error statting file after truncate: " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "File size after truncate: " << st.st_size << " bytes." << std::endl;
    if (static_cast<off_t>(st.st_size) != newSize) {
        std::cerr << "Error: expected file size " << newSize << " but got " << st.st_size << "." << std::endl;
        return EXIT_FAILURE;
    }

    // Step 5: Read the file and verify its contents.
    fd = open(filePath.c_str(), O_RDONLY);
    if (fd < 0) {
        std::cerr << "Error opening file for reading: " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }
    char buffer[1024] = {0};
    ssize_t bytesRead = read(fd, buffer, sizeof(buffer) - 1);
    if (bytesRead < 0) {
        std::cerr << "Error reading file: " << strerror(errno) << std::endl;
        close(fd);
        return EXIT_FAILURE;
    }
    close(fd);

    std::string readContent(buffer, bytesRead);
    std::cout << "Read " << bytesRead << " bytes from file. Content: '" << readContent << "'" << std::endl;
    if (static_cast<off_t>(readContent.size()) != newSize) {
        std::cerr << "Error: truncated content length (" << readContent.size() 
                  << ") does not match expected size (" << newSize << ")." << std::endl;
        return EXIT_FAILURE;
    }

    std::cout << "Truncate test passed successfully." << std::endl;
    return EXIT_SUCCESS;
}
