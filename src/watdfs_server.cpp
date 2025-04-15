//
// Starter code for CS 454/654
// You SHOULD change this file
//

#include "rpc.h"
#include "debug.h"
INIT_LOG

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>
#include <cstdlib>
#include <fuse.h>
#include <mutex>
#include <unordered_map>
#include "rw_lock.h"

// Global state server_persist_dir.
char *server_persist_dir = nullptr;

// mutex 
std::mutex file_lock_mutex;
std::unordered_map<std::string, rw_lock_t> file_locks; // file : locks
std::unordered_map<std::string, std::pair<bool, int>> file_status; // file : <inWrite?, numReader>

// Important: the server needs to handle multiple concurrent client requests.
// You have to be careful in handling global variables, especially for updating them.
// Hint: use locks before you update any global variable.

// We need to operate on the path relative to the server_persist_dir.
// This function returns a path that appends the given short path to the
// server_persist_dir. The character array is allocated on the heap, therefore
// it should be freed after use.
// Tip: update this function to return a unique_ptr for automatic memory management.
char *get_full_path(char *short_path) {
    int short_path_len = strlen(short_path);
    int dir_len = strlen(server_persist_dir);
    int full_len = dir_len + short_path_len + 1;

    char *full_path = (char *)malloc(full_len);

    // First fill in the directory.
    strcpy(full_path, server_persist_dir);
    // Then append the path.
    strcat(full_path, short_path);
    DLOG("Full path: %s\n", full_path);

    return full_path;
}

// The server implementation of getattr.
int watdfs_getattr(int *argTypes, void **args) {
    // Get the arguments.
    // The first argument is the path relative to the mountpoint.
    char *short_path = (char *)args[0];
    // The second argument is the stat structure, which should be filled in
    // by this function.
    struct stat *statbuf = (struct stat *)args[1];
    // The third argument is the return code, which should be set be 0 or -errno.
    int *ret = (int *)args[2];

    // Get the local file name, so we call our helper function which appends
    // the server_persist_dir to the given path.
    char *full_path = get_full_path(short_path);

    // Initially we set the return code to be 0.
    *ret = 0;

    // TODO: Make the stat system call, which is the corresponding system call needed
    // to support getattr. You should use the statbuf as an argument to the stat system call.
    // Let sys_ret be the return code from the stat system call.
    int sys_ret = stat(full_path, statbuf);

    if (sys_ret < 0) {
        // If there is an error on the system call, then the return code should
        // be -errno.
        *ret = -errno;
    }

    // Clean up the full path, it was allocated on the heap.
    free(full_path);

    //DLOG("Returning code: %d", *ret);
    // The RPC call succeeded, so return 0.
    return 0;
}

int watdfs_mknod(int *argTypes, void **args) {
    char *short_path = (char *)args[0];
    mode_t *mode_ptr = (mode_t *)args[1];
    dev_t *dev_ptr   = (dev_t *)args[2];
    int *ret         = (int *)args[3];

    char *full_path = get_full_path(short_path);

    int rc = mknod(full_path, *mode_ptr, *dev_ptr);
    if (rc < 0) {
        *ret = -errno;
    } else {
        *ret = 0;
    }

    free(full_path);
    return 0;
}

int watdfs_open(int *argTypes, void **args) {
    char *short_path         = (char *)args[0];
    struct fuse_file_info *fi = (struct fuse_file_info *)args[1];
    int *ret                 = (int *)args[2];
    *ret = 0;
    char *full_path = get_full_path(short_path);
    int fxn_ret;

    std::lock_guard<std::mutex> lock(file_lock_mutex);

    if((fi->flags & (O_WRONLY | O_RDWR)) && file_status.count(full_path) && file_status[full_path].first) {
        *ret = -EACCES;
        free(full_path);
        return 0;
    }

    // Open using fi->flags
    int fd = open(full_path, fi->flags, 0644);
    if (fd < 0) {
        fi->fh = -1;
        *ret = -errno;
    } else {
        fi->fh = fd;
        *ret = 0;
    }

    if(file_status.count(full_path) == 0) { // DNE in file_status
        file_status[full_path].first = false;
        file_status[full_path].second = 0;
        file_locks[full_path] = rw_lock_t();

        fxn_ret = rw_lock_init(&file_locks[full_path]);
        if(fxn_ret < 0) {
            *ret = -errno;
            return 0;
        }
    }

    if(fi->flags & (O_WRONLY | O_RDWR)) file_status[full_path].first = true;
    else file_status[full_path].second++;

    free(full_path);
    return 0;
}

int watdfs_release(int *argTypes, void **args) {
    char *short_path          = (char *)args[0];
    struct fuse_file_info *fi = (struct fuse_file_info *)args[1];
    int *ret                  = (int *)args[2];

    (void)short_path;
    *ret = 0;
    char *full_path = get_full_path(short_path);

    int fd = (int)(fi->fh);

    int fxn_ret = close(fd);
    if(fxn_ret < 0) {
        *ret = -errno;
        return 0;
    }

    std::lock_guard<std::mutex> lock(file_lock_mutex);
    
    if(fi->flags & (O_WRONLY | O_RDWR)) file_status[full_path].first = false;
    else file_status[full_path].second--;

    if(file_status[full_path].first == false && file_status[full_path].second == 0) { // remove
        file_status.erase(full_path);
        fxn_ret = rw_lock_destroy(&file_locks[full_path]);
        if(fxn_ret < 0) {
            *ret = -errno;
            return 0;
        }
        file_locks.erase(full_path);
    }

    return 0;
}

int watdfs_read(int *argTypes, void **args) {
    char *short_path         = (char *)args[0];
    char *buffer             = (char *)args[1];
    long *size_param         = (long *)args[2];
    long *offset_param       = (long *)args[3];
    struct fuse_file_info *fi = (struct fuse_file_info *)args[4];
    int *ret                 = (int *)args[5];

    (void)short_path;

    int fd = (int)fi->fh;
    ssize_t nread = pread(fd, buffer, (size_t)*size_param, (off_t)*offset_param);
    if (nread < 0) {
        *ret = -errno;
    } else {
        *ret = (int)nread;
    }

    return 0;
}

int watdfs_write(int *argTypes, void **args) {
    char *short_path          = (char *)args[0];
    const char *buffer        = (const char *)args[1];
    long *size_param          = (long *)args[2];
    long *offset_param        = (long *)args[3];
    struct fuse_file_info *fi = (struct fuse_file_info *)args[4];
    int *ret                  = (int *)args[5];

    (void)short_path;

    int fd = (int)fi->fh;
    ssize_t nwritten = pwrite(fd, buffer, (size_t)*size_param, (off_t)*offset_param);
    if (nwritten < 0) {
        *ret = -errno;
    } else {
        *ret = (int)nwritten;
    }

    return 0;
}

int watdfs_truncate(int *argTypes, void **args) {
    char *short_path = (char *)args[0];
    long *newsize    = (long *)args[1];
    int *ret         = (int *)args[2];

    char *full_path = get_full_path(short_path);

    int rc = truncate(full_path, (off_t)*newsize);
    if (rc < 0) *ret = -errno;
    else *ret = 0;

    free(full_path);
    return 0;
}

int watdfs_fsync(int *argTypes, void **args) {
    char *short_path          = (char *)args[0];
    struct fuse_file_info *fi = (struct fuse_file_info *)args[1];
    int *ret                  = (int *)args[2];

    (void)short_path;

    int fd = (int)fi->fh;
    if (fsync(fd) < 0) {
        *ret = -errno;
    } else {
        *ret = 0;
    }
    return 0;
}

int watdfs_utimensat(int *argTypes, void **args) {
    char *short_path      = (char *)args[0];
    struct timespec *ts   = (struct timespec *)args[1];
    int *ret              = (int *)args[2];

    char *full_path = get_full_path(short_path);

    int rc = utimensat(AT_FDCWD, full_path, ts, 0);
    DLOG("utimensat return with %d", rc);
    if (rc < 0) {
        *ret = -errno;
    } else {
        *ret = 0;
    }

    free(full_path);
    return 0;
}

int watdfs_lock(int *argTypes, void **args) {
    char *short_path = (char *)args[0];
    rw_lock_mode_t *mode = (rw_lock_mode_t *)args[1];
    int *ret = (int *)args[2];
    *ret = 0;
    char* full_path = get_full_path(short_path);

    if(file_locks.count(full_path) == 0) { // lock doesn't exist
        *ret = -EINVAL;
        return 0;
    }

    int i = 0;
    while (rw_lock_lock(&file_locks[full_path], *mode) < 0) {
        DLOG("Acquire lock at watdfs_lock fails at: %d, sleep", i);
        usleep(1000 * (1 << i));
    }
    DLOG("Acquired lock successfully");
   
    return 0;
}

int watdfs_unlock(int *argTypes, void **args) {
    char *short_path = (char *)args[0];
    rw_lock_mode_t *mode = (rw_lock_mode_t *)args[1];
    int *ret = (int *)args[2];
    *ret = 0;
    char* full_path = get_full_path(short_path);

    if(file_locks.count(full_path) == 0) { // lock doesn't exist
        *ret = -EINVAL;
        return 0;
    }

    int fxn_ret = rw_lock_unlock(&file_locks[full_path], *mode);
    if(fxn_ret < 0) *ret = -errno;
    else *ret = fxn_ret;

    return 0;
}

// The main function of the server.
int main(int argc, char *argv[]) {
    // argv[1] should contain the directory where you should store data on the
    // server. If it is not present it is an error, that we cannot recover from.
    if (argc != 2) {
        // In general, you shouldn't print to stderr or stdout, but it may be
        // helpful here for debugging. Important: Make sure you turn off logging
        // prior to submission!
        // See watdfs_client.cpp for more details
        // # ifdef PRINT_ERR
        // std::cerr << "Usage:" << argv[0] << " server_persist_dir";
        // #endif
        return -1;
    }
    // Store the directory in a global variable.
    server_persist_dir = argv[1];

    // TODO: Initialize the rpc library by calling `rpcServerInit`.
    // Important: `rpcServerInit` prints the 'export SERVER_ADDRESS' and
    // 'export SERVER_PORT' lines. Make sure you *do not* print anything
    // to *stdout* before calling `rpcServerInit`.
    //DLOG("Initializing server...");
    int init_ret = rpcServerInit();
    if (init_ret < 0) {
        DLOG("rpcServerInit failed with %d\n", init_ret);
        return init_ret;
    }

    int ret = 0;
    // TODO: If there is an error with `rpcServerInit`, it maybe useful to have
    // debug-printing here, and then you should return.

    // TODO: Register your functions with the RPC library.
    // Note: The braces are used to limit the scope of `argTypes`, so that you can
    // reuse the variable for multiple registrations. Another way could be to
    // remove the braces and use `argTypes0`, `argTypes1`, etc.
    {
        // There are 3 args for the function (see watdfs_client.cpp for more
        // detail).
        int argTypes[4];
        // First is the path.
        argTypes[0] =
            (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | 1u;
        // The second argument is the statbuf.
        argTypes[1] =
            (1u << ARG_OUTPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | 1u;
        // The third argument is the retcode.
        argTypes[2] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
        // Finally we fill in the null terminator.
        argTypes[3] = 0;

        // We need to register the function with the types and the name.
        ret = rpcRegister((char *)"getattr", argTypes, watdfs_getattr);
        if (ret < 0) {
            // It may be useful to have debug-printing here.
            DLOG("rpcRegister(getattr) failed with %d\n", ret);
            return ret;
        }
    }

    // TODO: Hand over control to the RPC library by calling `rpcExecute`.
    // mknod
    {
        int argTypes[5];
        argTypes[0] = (1 << ARG_INPUT) | (1 << ARG_ARRAY) | (ARG_CHAR << 16) | 1;
        argTypes[1] = (1 << ARG_INPUT) | (ARG_INT << 16);
        argTypes[2] = (1 << ARG_INPUT) | (ARG_LONG << 16);
        argTypes[3] = (1 << ARG_OUTPUT) | (ARG_INT << 16);
        argTypes[4] = 0;

        ret = rpcRegister((char *)"mknod", argTypes, watdfs_mknod);
        if (ret < 0) {
            DLOG("rpcRegister(mknod) failed with %d\n", ret);
            return ret;
        }
    }

    // open
    {
        int argTypes[4];
        argTypes[0] = (1 << ARG_INPUT) | (1 << ARG_ARRAY) | (ARG_CHAR << 16) | 1;
        argTypes[1] = (1 << ARG_INPUT) | (1 << ARG_OUTPUT) | (1 << ARG_ARRAY) | (ARG_CHAR << 16) | 1;
        argTypes[2] = (1 << ARG_OUTPUT) | (ARG_INT << 16);
        argTypes[3] = 0;

        ret = rpcRegister((char *)"open", argTypes, watdfs_open);
        if (ret < 0) {
            DLOG("rpcRegister(open) failed with %d\n", ret);
            return ret;
        }
    }

    // release
    {
        int argTypes[3 + 1];
        argTypes[0] = (1 << ARG_INPUT) | (1 << ARG_ARRAY) | (ARG_CHAR << 16) | 1;
        argTypes[1] = (1 << ARG_INPUT) | (1 << ARG_ARRAY) | (ARG_CHAR << 16) | 1;
        argTypes[2] = (1 << ARG_OUTPUT) | (ARG_INT << 16);
        argTypes[3] = 0;

        ret = rpcRegister((char *)"release", argTypes, watdfs_release);
        if (ret < 0) {
            DLOG("rpcRegister(release) failed with %d\n", ret);
            return ret;
        }
    }

    // read
    {
        int argTypes[7];
        argTypes[0] = (1 << ARG_INPUT) | (1 << ARG_ARRAY) | (ARG_CHAR << 16) | 1;
        argTypes[1] = (1 << ARG_OUTPUT) | (1 << ARG_ARRAY) | (ARG_CHAR << 16) | 1;
        argTypes[2] = (1 << ARG_INPUT) | (ARG_LONG << 16);
        argTypes[3] = (1 << ARG_INPUT) | (ARG_LONG << 16);
        argTypes[4] = (1 << ARG_INPUT) | (1 << ARG_ARRAY) | (ARG_CHAR << 16) | 1;
        argTypes[5] = (1 << ARG_OUTPUT) | (ARG_INT << 16);
        argTypes[6] = 0;

        ret = rpcRegister((char *)"read", argTypes, watdfs_read);
        if (ret < 0) {
            DLOG("rpcRegister(read) failed with %d\n", ret);
            return ret;
        }
    }

    // write
    {
        int argTypes[7];
        argTypes[0] = (1 << ARG_INPUT) | (1 << ARG_ARRAY) | (ARG_CHAR << 16) | 1;
        argTypes[1] = (1 << ARG_INPUT) | (1 << ARG_ARRAY) | (ARG_CHAR << 16) | 1;
        argTypes[2] = (1 << ARG_INPUT) | (ARG_LONG << 16);
        argTypes[3] = (1 << ARG_INPUT) | (ARG_LONG << 16);
        argTypes[4] = (1 << ARG_INPUT) | (1 << ARG_ARRAY) | (ARG_CHAR << 16) | 1;
        argTypes[5] = (1 << ARG_OUTPUT) | (ARG_INT << 16);
        argTypes[6] = 0;

        ret = rpcRegister((char *)"write", argTypes, watdfs_write);
        if (ret < 0) {
            DLOG("rpcRegister(write) failed with %d\n", ret);
            return ret;
        }
    }

    // truncate
    {
        int argTypes[4];
        argTypes[0] = (1 << ARG_INPUT) | (1 << ARG_ARRAY) | (ARG_CHAR << 16) | 1;
        argTypes[1] = (1 << ARG_INPUT) | (ARG_LONG << 16);
        argTypes[2] = (1 << ARG_OUTPUT) | (ARG_INT << 16);
        argTypes[3] = 0;

        ret = rpcRegister((char *)"truncate", argTypes, watdfs_truncate);
        if (ret < 0) {
            DLOG("rpcRegister(truncate) failed with %d\n", ret);
            return ret;
        }
    }

    // fsync
    {
        int argTypes[4];
        argTypes[0] = (1 << ARG_INPUT) | (1 << ARG_ARRAY) | (ARG_CHAR << 16) | 1;
        argTypes[1] = (1 << ARG_INPUT) | (1 << ARG_ARRAY) | (ARG_CHAR << 16) | 1;
        argTypes[2] = (1 << ARG_OUTPUT) | (ARG_INT << 16);
        argTypes[3] = 0;

        ret = rpcRegister((char *)"fsync", argTypes, watdfs_fsync);
        if (ret < 0) {
            DLOG("rpcRegister(fsync) failed with %d\n", ret);
            return ret;
        }
    }

    // utimensat
    {
        int argTypes[4];
        argTypes[0] = (1 << ARG_INPUT) | (1 << ARG_ARRAY) | (ARG_CHAR << 16) | 1;
        argTypes[1] = (1 << ARG_INPUT) | (1 << ARG_ARRAY) | (ARG_CHAR << 16) | 1;
        argTypes[2] = (1 << ARG_OUTPUT) | (ARG_INT << 16);
        argTypes[3] = 0;

        ret = rpcRegister((char *)"utimensat", argTypes, watdfs_utimensat);
        if (ret < 0) {
            DLOG("rpcRegister(utimensat) failed with %d\n", ret);
            return ret;
        }
    }

    // lock
    {
        int argTypes[4];
        argTypes[0] = (1 << ARG_INPUT) | (1 << ARG_ARRAY) | (ARG_CHAR << 16) | 1;
        argTypes[1] = (1 << ARG_INPUT) | (ARG_INT << 16);
        argTypes[2] = (1 << ARG_OUTPUT) | (ARG_INT << 16);
        argTypes[3] = 0;

        rpcRegister((char *)"lock", argTypes, watdfs_lock);
        if(ret < 0) {
            DLOG("prcRegister(lock) failed with %d\n", ret);
            return ret;
        }
    }

    // unlock
    {
        int argTypes[4];
        argTypes[0] = (1 << ARG_INPUT) | (1 << ARG_ARRAY) | (ARG_CHAR << 16) | 1;
        argTypes[1] = (1 << ARG_INPUT) | (ARG_INT << 16);
        argTypes[2] = (1 << ARG_OUTPUT) | (ARG_INT << 16);
        argTypes[3] = 0;

        rpcRegister((char *)"unlock", argTypes, watdfs_unlock);
        if(ret < 0) {
            DLOG("prcRegister(unlock) failed with %d\n", ret);
            return ret;
        }
    }

    // rpcExecute could fail, so you may want to have debug-printing here, and
    // then you should return.
    ret = rpcExecute();
    if (ret < 0) {
        DLOG("rpcExecute failed with %d\n", ret);
    }
    return ret;
}
