// test_cli_open.cpp
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fuse.h>
#include "watdfs_client.h"

using namespace std;

int main(int argc, char *argv[]) {
    if(argc < 2) {
        cerr << "Usage: " << argv[0] << " <mount_point>" << endl;
        return EXIT_FAILURE;
    }
    
    // Use the provided mount point as the cache directory.
    const char* mountPoint = argv[1];

    // Create the mount directory if it doesn't exist.
    std::string mkdirCmd = "mkdir -p ";
    mkdirCmd += mountPoint;
    system(mkdirCmd.c_str());

    // Set a cache interval (in seconds)
    time_t cacheInterval = 60;

    // Initialize the WatDFS client.
    int retCode = 0;
    void* userdata = watdfs_cli_init(nullptr, mountPoint, cacheInterval, &retCode);
    if (!userdata || retCode != 0) {
        cerr << "watdfs_cli_init failed with code: " << retCode << endl;
        return EXIT_FAILURE;
    }

    // Specify the test file path (relative to the mount point).
    const char* testPath = "/test.txt";

    // Prepare a fuse_file_info structure.
    struct fuse_file_info fi;
    memset(&fi, 0, sizeof(fi));
    // For testing, open with read/write and create if needed.
    fi.flags = O_RDWR | O_CREAT;

    // Call watdfs_cli_open.
    retCode = watdfs_cli_open(userdata, testPath, &fi);
    if(retCode != 0) {
        cerr << "watdfs_cli_open failed with code: " << retCode << endl;
        watdfs_cli_destroy(userdata);
        return EXIT_FAILURE;
    }
    cout << "watdfs_cli_open succeeded, file descriptor: " << fi.fh << endl;

    // Call watdfs_cli_release.
    retCode = watdfs_cli_release(userdata, testPath, &fi);
    if(retCode != 0) {
        cerr << "watdfs_cli_release failed with code: " << retCode << endl;
        watdfs_cli_destroy(userdata);
        return EXIT_FAILURE;
    }
    cout << "watdfs_cli_release succeeded." << endl;

    // Clean up WatDFS client userdata.
    watdfs_cli_destroy(userdata);

    return EXIT_SUCCESS;
}
