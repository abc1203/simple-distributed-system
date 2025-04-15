//
// Starter code for CS 454/654
// You SHOULD change this file
//

#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>       // if using vector
#include <sys/stat.h>   // if using stat()
#include <fcntl.h>      // if using open()
#include <unistd.h>     // if using close(), read(), write()
#include <cstdio>   
#include "watdfs_client.h"
#include "debug.h"
INIT_LOG

#include "rpc.h"
#include "rw_lock.h"

// Define logging macro
#ifdef PRINT_ERR
#define LOG_ERROR(msg) std::cerr << "ERROR: " << msg << std::endl
#else
#define LOG_ERROR(msg)
#endif

struct CacheEntry {
    int fh;
    int flags;

    time_t Tc;        // last time entry validated by client
};

struct ClientData {
    const char* path_to_cache;
    time_t cache_interval;
    std::unordered_map<std::string, CacheEntry> cacheMap; // current open files
    // pthread_mutex_t cacheMutex;
};


// SETUP AND TEARDOWN
void *watdfs_cli_init(struct fuse_conn_info *conn, const char *path_to_cache,
                      time_t cache_interval, int *ret_code) {
    // TODO: set up the RPC library by calling `rpcClientInit`.
    *ret_code = rpcClientInit();

    // TODO: check the return code of the `rpcClientInit` call
    // `rpcClientInit` may fail, for example, if an incorrect port was exported.
    if(*ret_code != OK) {
        LOG_ERROR("RPC client init failed (code: " << *ret_code << ")");
    }

    // It may be useful to print to stderr or stdout during debugging.
    // Important: Make sure you turn off logging prior to submission!
    // One useful technique is to use pre-processor flags like:
   
    // Tip: Try using a macro for the above to minimize the debugging code.

    // TODO Initialize any global state that you require for the assignment and return it.
    // The value that you return here will be passed as userdata in other functions.
    // In A1, you might not need it, so you can return `nullptr`.
    ClientData *userdata = new ClientData();
    // pthread_mutex_init(&userdata->cacheMutex, NULL);

    // TODO: save `path_to_cache` and `cache_interval` (for A3).
    userdata->path_to_cache = path_to_cache;
    userdata->cache_interval = cache_interval;

    // TODO: set `ret_code` to 0 if everything above succeeded else some appropriate
    // non-zero value.
    *ret_code = 0;

    // Return pointer to global state data.
    return (void *)userdata;
}

void watdfs_cli_destroy(void *userdata) {
    // TODO: clean up your userdata state.
    // ClientData *ud = (ClientData *)userdata;
    // pthread_mutex_destroy(&ud->cacheMutex);

    free(userdata);
    // TODO: tear down the RPC library by calling `rpcClientDestroy`.
    rpcClientDestroy();
}

// CREATE
int watdfs_cli_mknod(void *userdata, const char *path, mode_t mode, dev_t dev) {
    DLOG("watdfs_cli_mknod called for '%s'", path);
    int fxn_ret = 0;

    char* local_path = get_local_path(userdata, path);

    int fd = open(local_path, O_CREAT | O_WRONLY, mode);
    if (fd < 0) {
        fxn_ret = -errno;
        unlink(local_path);
        free(local_path);
        return fxn_ret;
    }
    close(fd);
    DLOG("watdfs_cli_mknod called mknod locally");

    fxn_ret = mknod_RPC(userdata, path, mode, dev);
    if (fxn_ret < 0) {
        // Delete the local file if server creation failed.
        unlink(local_path);
        free(local_path);
        return fxn_ret;
    }
    free(local_path);

    return fxn_ret;
}


int watdfs_cli_open(void *userdata, const char *path,
                    struct fuse_file_info *fi) {
    // Called during open.
    // You should fill in fi->fh.
    DLOG("watdfs_cli_open called for '%s'", path);
    int fxn_ret = 0;
    ClientData *ud = (ClientData *)userdata;
    
    // check if file already opened
    if (ud->cacheMap.count(path) != 0) return -EMFILE;

    // file not open => download first
    fxn_ret = download_file(userdata, path);
    if(fxn_ret < 0) return fxn_ret;
    
    char* local_path = get_local_path(userdata, path);

    // open local copy
    int local_fd = open(local_path, fi->flags, 0666);
    free(local_path);
    if (local_fd < 0) return local_fd;

    // open server file
    fxn_ret = open_RPC(userdata, path, fi);
    if(fxn_ret < 0) return fxn_ret;

    // put entry in cache
    CacheEntry &entry = ud->cacheMap[path];
    entry.fh = fi->fh;
    entry.flags = fi->flags;
    entry.Tc = time(NULL);
    // set fi->fh to local file
    fi->fh = local_fd;

    return fxn_ret;
}


int watdfs_cli_release(void *userdata, const char *path,
                       struct fuse_file_info *fi) {
    // Called during close, but possibly asynchronously.
    DLOG("watdfs_cli_release called for '%s'", path);
    int fxn_ret = 0;
    ClientData *ud = (ClientData *)userdata;

    if(ud->cacheMap.count(path) != 0) { // file opened
        if (!read_only(fi->flags)) { // open in write mode => upload
            fxn_ret = upload_file(userdata, path);
            if(fxn_ret < 0) return fxn_ret;
        }

        int local_fd = (int)fi->fh;

        // close server file
        fi->fh = ud->cacheMap[path].fh;
        fxn_ret = release_RPC(userdata, path, fi);
        if(fxn_ret < 0) return fxn_ret;

        // close local copy
        fxn_ret = close(local_fd);
        if(fxn_ret < 0) return fxn_ret;
        
        // delete from map
        ud->cacheMap.erase(path);
    } else { // file not open => cannot release
        return -EMFILE;
    }

    return fxn_ret;
}


// READ AND WRITE DATA
int watdfs_cli_read(void *userdata, const char *path, char *buf, size_t size,
                    off_t offset, struct fuse_file_info *fi) {
    // Read size amount of data at offset of file into buf.

    // Remember that size may be greater than the maximum array size of the RPC
    // library.
    DLOG("watdfs_cli_read called for '%s'", path);
    int fxn_ret = 0;
    ClientData *ud = (ClientData *)userdata;

    if(ud->cacheMap.count(path) != 0) { // currently opened
        bool is_fresh = check_freshness(userdata, path);

        if(is_fresh) { // read from local copy
            int local_fd = (int)fi->fh;
            fxn_ret = pread(local_fd, buf, size, offset);

            return fxn_ret;
        }
    }

    // not opened or not fresh => fetch from server
    fxn_ret = download_file(userdata, path);
    if(fxn_ret < 0) return fxn_ret;

    // read from local
    int local_fd = (int)fi->fh;
    fxn_ret = pread(local_fd, buf, size, offset);
    if(fxn_ret < 0) return fxn_ret;

    return fxn_ret;
}

// GET FILE ATTRIBUTES
int watdfs_cli_getattr(void *userdata, const char *path, struct stat *statbuf) {
    // SET UP THE RPC CALL
    DLOG("watdfs_cli_getattr called for '%s'", path);
    int fxn_ret = 0;
    ClientData *ud = (ClientData *)userdata;

    if(strcmp("/", path) == 0) return getattr_RPC(userdata, path, statbuf);

    char* local_path = get_local_path(userdata, path);
    
    if(ud->cacheMap.count(path) == 0) { // file not opened => download from server
        fxn_ret = download_file(userdata, path);
        if(fxn_ret < 0) return fxn_ret;
    } else if(read_only(ud->cacheMap[path].flags)) { // open with read-only => freshness check
        bool is_fresh = check_freshness(userdata, path);
        if(!is_fresh) { // not fresh => download from server
            fxn_ret = download_file(userdata, path);
            if(fxn_ret < 0) return fxn_ret;
        }
    }

    // getattr from local file
    fxn_ret = stat(local_path, statbuf);
    free(local_path);
    if(fxn_ret < 0) return fxn_ret;

    return fxn_ret;
}


int watdfs_cli_write(void *userdata, const char *path, const char *buf,
                     size_t size, off_t offset, struct fuse_file_info *fi) {
    // Write size amount of data at offset of file from buf.

    // Remember that size may be greater than the maximum array size of the RPC
    // library.
    DLOG("watdfs_cli_write called for '%s'", path);
    int fxn_ret = 0;
    ClientData *ud = (ClientData *)userdata;
    
    if(ud->cacheMap.count(path) == 0) { // not opened
        return -ENOENT;
    }
    if(read_only(ud->cacheMap[path].flags)) { // opened in read-only
        return -EMFILE;
    }

    // write to local copy
    int local_fd = (int)fi->fh;
    size_t bytes_written = pwrite(local_fd, buf, size, offset);
    if(bytes_written < 0) {
        return -errno;
    }

    // check freshness; write back if not fresh
    bool is_fresh = check_freshness(userdata, path);
    if(!is_fresh) { // write back to server
        fxn_ret = upload_file(userdata, path);
        if(fxn_ret < 0) return fxn_ret;
    }

    return bytes_written;
}


int watdfs_cli_truncate(void *userdata, const char *path, off_t newsize) {
    // same logic for write
    DLOG("watdfs_cli_truncate called for '%s'", path);
    int fxn_ret = 0;
    ClientData *ud = (ClientData *)userdata;
    
    if(ud->cacheMap.count(path) == 0) { // not open => download 
        fxn_ret = download_file(userdata, path);
        if(fxn_ret < 0) return fxn_ret;
    }

    if(read_only(ud->cacheMap[path].flags)) { // open with read-only
        return -EMFILE;
    }

    // truncate local copy
    char* local_path = get_local_path(userdata, path);
    fxn_ret = truncate(local_path, newsize);
    free(local_path);
    if(fxn_ret < 0) return fxn_ret;

    // check freshness; write back if not fresh
    bool is_fresh = check_freshness(userdata, path);
    if(!is_fresh) { // write back to server
        fxn_ret = upload_file(userdata, path);
        if(fxn_ret < 0) return fxn_ret;
    }
    
    return fxn_ret;
}


int watdfs_cli_fsync(void *userdata, const char *path,
                     struct fuse_file_info *fi) {
    // Force a flush of file data.
    DLOG("watdfs_cli_fsync called for '%s'", path);
    int fxn_ret = 0;
    ClientData *ud = (ClientData *)userdata;
    
    if(ud->cacheMap.count(path) == 0) return -ENOENT; // not open
    if(read_only(ud->cacheMap[path].flags)) return -EMFILE; // read-only

    // fsync local copy
    int local_fd = (int)fi->fh;
    fxn_ret = fsync(local_fd);
    if (fxn_ret < 0) return -errno;

    // write back to server
    fxn_ret = upload_file(userdata, path);
    if(fxn_ret < 0) return fxn_ret;

    return fxn_ret;
}


// CHANGE METADATA
int watdfs_cli_utimensat(void *userdata, const char *path,
                       const struct timespec ts[2]) {
    // Change file access and modification times.
    DLOG("watdfs_cli_utimensat called for '%s'", path);
    int fxn_ret = 0;
    ClientData *ud = (ClientData *)userdata;
    
    if(ud->cacheMap.count(path) != 0 && read_only(ud->cacheMap[path].flags)) { // open in read-only
        return -EMFILE;
    }
    if(ud->cacheMap.count(path) == 0) { // not open => download file
        fxn_ret = download_file(userdata, path);
        if(fxn_ret < 0) return fxn_ret;
    }

    // utimeset local copy
    char* local_path = get_local_path(userdata, path);
    fxn_ret = utimensat(AT_FDCWD, local_path, ts, 0);
    free(local_path);
    if(fxn_ret < 0) return fxn_ret;

    // upload to server if not fresh or not opened
    bool is_fresh = check_freshness(userdata, path);
    if(!is_fresh || ud->cacheMap.count(path) == 0) {
        fxn_ret = upload_file(userdata, path);
        if(fxn_ret < 0) return fxn_ret;
    }

    return fxn_ret;
}


int download_file(void *userdata, const char *path) {
    DLOG("download_file called for '%s'", path);
    int retcode;

    char* local_path = get_local_path(userdata, path);
    struct fuse_file_info fi_read = { .flags = O_RDONLY };

    // open server
    retcode = open_RPC(userdata, path, &fi_read);
    if (retcode < 0) {
        free(local_path);
        return retcode;
    }

    // lock
    retcode = lock_RPC(userdata, path, RW_READ_LOCK);
    if(retcode < 0) return retcode;

    // getattr from server
    struct stat st_server;
    retcode = getattr_RPC(userdata, path, &st_server);
    if (retcode < 0) {
        unlock_RPC(userdata, path, RW_READ_LOCK);
        unlink(local_path);
        free(local_path);
        return retcode;
    }
    // read from server
    char* buf = (char *)malloc(st_server.st_size);
    retcode = read_RPC(userdata, path, buf, st_server.st_size, 0, &fi_read);

    // unlock
    retcode = unlock_RPC(userdata, path, RW_READ_LOCK);
    if(retcode < 0) return retcode;


    // open local copy
    int fd_local = open(local_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd_local < 0) {
        unlink(local_path);
        free(local_path);
        return fd_local;
    }

    // truncate local copy
    retcode = ftruncate(fd_local, 0);
    if (retcode < 0) {
        close(fd_local);
        unlink(local_path);
        free(local_path);
        return retcode;
    }

    // write to local copy
    retcode = write(fd_local, buf, st_server.st_size);
    if (retcode < 0) {
        close(fd_local);
        unlink(local_path);
        free(local_path);
        return retcode;
    }

    close(fd_local);
    release_RPC(userdata, path, &fi_read);

    // 5. update the file metadata at the client
    struct timespec ts[2];
    ts[0] = st_server.st_atim;
    ts[1] = st_server.st_mtim;

    retcode = utimensat(AT_FDCWD, local_path, ts, 0);
    if(retcode < 0) return retcode;

    free(local_path);
    free(buf);
    DLOG("download_file done successfully");
    return retcode;
}

int upload_file(void *userdata, const char *path) {
    DLOG("upload_file called for '%s'", path);
    int retcode;
    struct fuse_file_info fi_write = { .flags = O_WRONLY };
    ClientData *ud = (ClientData *)userdata;

    // fetch from local file
    char* local_path = get_local_path(userdata, path);

    // get local file attributes
    struct stat *statbuf = new struct stat();
    retcode = stat(local_path, statbuf);
    if(retcode < 0) return retcode;

    // open local file
    int fd_local = open(local_path, O_RDONLY);
    if (fd_local < 0) {
        free(local_path);
        return fd_local;
    }

    // read from local file
    size_t size = statbuf->st_size;
    char * buf = (char *)malloc(size);
    retcode = read(fd_local, buf, size);
    if(retcode < 0) return retcode;

    // close local file
    retcode = close(fd_local);
    if(retcode < 0) return retcode;

    // mknod server file
    retcode = mknod_RPC(userdata, path, S_IFREG | 0644, 0);
    if(retcode < 0 && retcode != -EEXIST) return retcode;

    // check if server file is already opened with write mode; if not, do that
    bool need_release_call = false;
    if(ud->cacheMap.count(path) == 0 || read_only(ud->cacheMap[path].flags)) {
        retcode = open_RPC(userdata, path, &fi_write);
        if(retcode < 0) return retcode;
        need_release_call = true;
    }

    retcode = lock_RPC(userdata, path, RW_WRITE_LOCK);
    if(retcode < 0) return retcode;
    // truncate server file
    retcode = truncate_RPC(userdata, path, 0);
    DLOG("truncate_RPC done");
    if(retcode < 0) {
        unlock_RPC(userdata, path, RW_WRITE_LOCK);
        return retcode;
    }

    // write into server file
    if(!need_release_call) {
        fi_write.fh = ud->cacheMap[path].fh;
    }
    retcode = write_RPC(userdata, path, buf, size, 0, &fi_write);
    if(retcode < 0) {
        unlock_RPC(userdata, path, RW_WRITE_LOCK);
        return retcode;
    }

    // set time in server file
    struct timespec ts[2];
    ts[0] = statbuf->st_atim;
    ts[1] = statbuf->st_mtim;
    retcode = utimensat_RPC(userdata, path, ts);
    if(retcode < 0) {
        unlock_RPC(userdata, path, RW_WRITE_LOCK);
        return retcode;
    }

    retcode = unlock_RPC(userdata, path, RW_WRITE_LOCK);
    if(retcode < 0) return retcode;

    if(need_release_call) {
        retcode = release_RPC(userdata, path, &fi_write);
        if(retcode < 0) return retcode;
    }
    DLOG("upload_file done successfully");

    return retcode;
}

char* get_local_path(void *userdata, const char *path) {
    ClientData *ud = (ClientData *)userdata;
    char* local_path = (char *)malloc(strlen(ud->path_to_cache) + strlen(path) + 2);
    sprintf(local_path, "%s/%s", ud->path_to_cache, path);
    
    return local_path;
}

bool check_freshness(void* userdata, const char* path) {
    ClientData *ud = (ClientData *)userdata;
    if(ud->cacheMap.count(path) == 0) return false;

    CacheEntry &entry = ud->cacheMap[path];

    // check 1
    time_t T = time(NULL);
    if((T - entry.Tc) < ud->cache_interval) {
        DLOG("It is Fresh!");
        return true;
    }
    // check 2
    struct stat st_server;
    if(getattr_RPC(userdata, path, &st_server) < 0) return false;

    struct stat st_client;
    char* local_path = get_local_path(userdata, path);
    if(stat(local_path, &st_client) < 0) {
        free(local_path);
        return false;
    }
    free(local_path);
    entry.Tc = time(NULL); // update Tc
    if(st_client.st_mtime == st_server.st_mtime) {
        DLOG("It is Fresh!");
        return true;
    }

    DLOG("It is Not Fresh!");
    return false;

}




int open_RPC(void *userdata, const char *path, struct fuse_file_info *fi) {
    DLOG("open_RPC called for %s", path);
    int fxn_ret;
    int ARG_COUNT = 3;

    void **args = new void*[ARG_COUNT];
    int arg_types[ARG_COUNT + 1];
    int pathlen = strlen(path) + 1;

    arg_types[0] =
        (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | (uint) pathlen;
    args[0] = (void *)path;

    arg_types[1] = (1u << ARG_INPUT) | (1u << ARG_OUTPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | 
                (uint) sizeof(struct fuse_file_info);
    args[1] = (void *)fi;

    arg_types[2] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
    int ret_code;
    args[2] = (int *)&ret_code;

    arg_types[3] = 0;

    // MAKE THE RPC CALL
    int rpc_ret = rpcCall((char *)"open", arg_types, args);
    if (rpc_ret < 0) {
        DLOG("open rpc failed with error '%d'", rpc_ret);
        fxn_ret = rpc_ret;
    } else {
        fxn_ret = ret_code;
    }
    delete []args;

    return fxn_ret;
}

int release_RPC(void *userdata, const char *path, struct fuse_file_info *fi) {
    DLOG("release_RPC called for %s", path);
    int ARG_COUNT = 3;

    void **args = new void*[ARG_COUNT];
    int arg_types[ARG_COUNT + 1];
    int pathlen = strlen(path) + 1;

    arg_types[0] =
        (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | (uint) pathlen;
    args[0] = (void *)path;

    arg_types[1] = (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | 
                   (uint) sizeof(struct fuse_file_info);
    args[1] = (void *)fi;

    arg_types[2] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
    int ret_code;
    args[2] = (int *)&ret_code;

    arg_types[3] = 0;

    // MAKE THE RPC CALL
    int rpc_ret = rpcCall((char *)"release", arg_types, args);

    int fxn_ret = 0;
    if (rpc_ret < 0) {
        DLOG("open rpc failed with error '%d'", rpc_ret);
        fxn_ret = rpc_ret;
    } else {
        fxn_ret = ret_code;
    }

    delete []args;
    return fxn_ret;
}

int getattr_RPC(void *userdata, const char *path, struct stat *statbuf) {
    DLOG("getattr_RPC called for %s", path);
    // getattr has 3 arguments.
    int ARG_COUNT = 3;

    // Allocate space for the output arguments.
    void **args = new void*[ARG_COUNT];

    int arg_types[ARG_COUNT + 1];

    int pathlen = strlen(path) + 1;

    arg_types[0] =
        (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | (uint) pathlen;
    args[0] = (void *)path;

    arg_types[1] = (1u << ARG_OUTPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) |
                   (uint) sizeof(struct stat); // statbuf
    args[1] = (void *)statbuf;

    arg_types[2] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);

    int ret_code;
    args[2] = (int *)&ret_code;

    arg_types[3] = 0;

    int rpc_ret = rpcCall((char *)"getattr", arg_types, args);

    int fxn_ret = 0;
    if (rpc_ret < 0) {
        DLOG("getattr rpc failed with error '%d'", rpc_ret);
        fxn_ret = rpc_ret;
    } else {
        fxn_ret = ret_code;
    }

    if (fxn_ret < 0) {
        memset(statbuf, 0, sizeof(struct stat));
    }

    delete []args;
    return fxn_ret;
}

int read_RPC(void *userdata, const char *path, char *buf, size_t size,
                off_t offset, struct fuse_file_info *fi) {
    DLOG("read_RPC called for %s", path);
    size_t total_read = 0;

    // Keep reading in chunks up to MAX_ARRAY_LEN 
    while (total_read < size) {
        size_t chunk_size = size - total_read;
        if (chunk_size > MAX_ARRAY_LEN) {
            chunk_size = MAX_ARRAY_LEN;
        }

        int ARG_COUNT = 6;
        void *args[ARG_COUNT];
        int arg_types[ARG_COUNT + 1];

        int pathlen = strlen(path) + 1;
        arg_types[0] =
            (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | pathlen;
        args[0] = (void *)path;

        arg_types[1] =
            (1u << ARG_OUTPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | (uint)chunk_size;
        // allocate tmp buffer
        char chunk_buf[65536];
        args[1] = (void *)chunk_buf;

        arg_types[2] = (1u << ARG_INPUT) | (ARG_LONG << 16u);
        long rpc_size = (long)chunk_size;
        args[2] = (void *)&rpc_size;

        arg_types[3] = (1u << ARG_INPUT) | (ARG_LONG << 16u);
        long rpc_offset = (long)(offset + total_read);
        args[3] = (void *)&rpc_offset;

        arg_types[4] =
            (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) |
            (uint) sizeof(struct fuse_file_info);
        args[4] = (void *)fi;

        arg_types[5] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
        int retcode;
        args[5] = (void *)&retcode;

        arg_types[6] = 0;

        // Make the RPC call
        int rpc_ret = rpcCall((char *)"read", arg_types, args);
        if (rpc_ret < 0) {
            DLOG("read rpcCall failed with %d", rpc_ret);
            return rpc_ret;
        }
        if (retcode < 0) {
            return retcode;
        }

        if (retcode == 0) {
            // EOF or nothing more to read
            break;
        }

        memcpy(buf + total_read, chunk_buf, retcode);
        total_read += retcode;

        if (retcode < (int)chunk_size) {
            break;
        }
    }

    return (int)total_read;
}

int write_RPC(void *userdata, const char *path, const char *buf,
                size_t size, off_t offset, struct fuse_file_info *fi) {
    DLOG("write_RPC called for %s", path);
    size_t total_written = 0;

    while (total_written < size) {
        size_t chunk_size = size - total_written;
        if (chunk_size > MAX_ARRAY_LEN) {
            chunk_size = MAX_ARRAY_LEN;
        }

        int ARG_COUNT = 6;
        void *args[ARG_COUNT];
        int arg_types[ARG_COUNT + 1];

        int pathlen = strlen(path) + 1;
        arg_types[0] =
            (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | pathlen;
        args[0] = (void *)path;

        arg_types[1] =
            (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | (uint)chunk_size;
        args[1] = (void *)(buf + total_written);

        arg_types[2] = (1u << ARG_INPUT) | (ARG_LONG << 16u);
        long rpc_size = (long)chunk_size;
        args[2] = (void *)&rpc_size;

        arg_types[3] = (1u << ARG_INPUT) | (ARG_LONG << 16u);
        long rpc_offset = (long)(offset + total_written);
        args[3] = (void *)&rpc_offset;

        arg_types[4] =
            (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | 
            (uint) sizeof(struct fuse_file_info);
        args[4] = (void *)fi;

        arg_types[5] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
        int retcode;
        args[5] = (void *)&retcode;

        arg_types[6] = 0;

        int rpc_ret = rpcCall((char *)"write", arg_types, args);
        if (rpc_ret < 0) {
            DLOG("write rpcCall failed with %d", rpc_ret);
            return rpc_ret;
        }
        if (retcode < 0) {
            return retcode;
        }

        if (retcode == 0) {
            break;
        }
        total_written += retcode;

        if (retcode < (int)chunk_size) {
            break;
        }
    }

    return (int)total_written;
}

int truncate_RPC(void *userdata, const char *path, off_t newsize) {
    DLOG("truncate_RPC called for %s", path);
    int ARG_COUNT = 3;
    void *args[ARG_COUNT];
    int arg_types[ARG_COUNT + 1];

    int pathlen = strlen(path) + 1;
    arg_types[0] =
        (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | pathlen;
    args[0] = (void *)path;

    arg_types[1] = (1u << ARG_INPUT) | (ARG_LONG << 16u);
    long size_param = (long)newsize;
    args[1] = &size_param;

    arg_types[2] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
    int retcode;
    args[2] = &retcode;

    arg_types[3] = 0;

    int rpc_ret = rpcCall((char *)"truncate", arg_types, args);
    if (rpc_ret < 0) {
        DLOG("truncate rpcCall failed with %d", rpc_ret);
        return rpc_ret;
    }
    return retcode;
}

int fsync_RPC(void *userdata, const char *path, struct fuse_file_info *fi) {
    DLOG("fsync_RPC called for %s", path);
    int ARG_COUNT = 3;
    void *args[ARG_COUNT];
    int arg_types[ARG_COUNT + 1];

    int pathlen = strlen(path) + 1;
    arg_types[0] =
        (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | pathlen;
    args[0] = (void *)path;

    arg_types[1] =
        (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | 
        (uint) sizeof(struct fuse_file_info);
    args[1] = (void *)fi;

    arg_types[2] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
    int retcode;
    args[2] = &retcode;

    arg_types[3] = 0;

    int rpc_ret = rpcCall((char *)"fsync", arg_types, args);
    if (rpc_ret < 0) {
        DLOG("fsync rpcCall failed with %d", rpc_ret);
        return rpc_ret;
    }
    return retcode;
}

int utimensat_RPC(void *userdata, const char *path, const struct timespec ts[2]) {
    DLOG("utimensat_RPC called for %s", path);
    int ARG_COUNT = 3;
    void *args[ARG_COUNT];
    int arg_types[ARG_COUNT + 1];

    int pathlen = strlen(path) + 1;
    arg_types[0] = (1u << ARG_INPUT) | (1u << ARG_ARRAY)
                   | (ARG_CHAR << 16u) | pathlen;
    args[0] = (void *)path;

    arg_types[1] = (1u << ARG_INPUT) | (1u << ARG_ARRAY)
                   | (ARG_CHAR << 16u) | (uint)(2 * sizeof(struct timespec));
    args[1] = (void *)ts;

    arg_types[2] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
    int retcode;
    args[2] = &retcode;

    arg_types[3] = 0;

    int rpc_ret = rpcCall((char *)"utimensat", arg_types, args);
    if (rpc_ret < 0) {
        DLOG("utimensat rpcCall failed with %d", rpc_ret);
        return -EIO;
    }
    return retcode;
}

int mknod_RPC(void *userdata, const char *path, mode_t mode, dev_t dev) {
    int ARG_COUNT = 4;

    void **args = new void*[ARG_COUNT];
    int arg_types[ARG_COUNT + 1];
    int pathlen = strlen(path) + 1;

    arg_types[0] =
        (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | (uint) pathlen;
    args[0] = (void *)path;

    arg_types[1] = (1u << ARG_INPUT) | (ARG_INT << 16u);
    args[1] = (void *)&mode;

    arg_types[2] = (1u << ARG_INPUT) | (ARG_LONG << 16u);
    args[2] = (void *)&dev;

    arg_types[3] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
    int ret_code;
    args[3] = (int *)&ret_code;

    arg_types[4] = 0;

    // MAKE THE RPC CALL
    int rpc_ret = rpcCall((char *)"mknod", arg_types, args);

    int fxn_ret = 0;
    if (rpc_ret < 0) {
        DLOG("mknod rpc failed with error '%d'", rpc_ret);
        fxn_ret = rpc_ret;
    } else {
        fxn_ret = ret_code;
    }

    delete []args;
    return fxn_ret;
}


int lock_RPC(void *userdata, const char *path, rw_lock_mode_t mode) {
    int ARG_COUNT = 3;
    void *args[ARG_COUNT];
    int arg_types[ARG_COUNT + 1];

    // Path
    arg_types[0] = (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | (strlen(path) + 1);
    args[0] = (void *)path;

    // Mode 
    arg_types[1] = (1u << ARG_INPUT) | (ARG_INT << 16u);
    args[1] = (void *)&mode;

    // Retcode
    arg_types[2] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
    int ret_code;
    args[2] = &ret_code;

    arg_types[3] = 0;

    int rpc_ret = rpcCall((char *)"lock", arg_types, args);
    return (rpc_ret < 0) ? rpc_ret : ret_code;
}

int unlock_RPC(void *userdata, const char *path, rw_lock_mode_t mode) {
    int ARG_COUNT = 3;
    void *args[ARG_COUNT];
    int arg_types[ARG_COUNT + 1];

    // Path
    arg_types[0] = (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | (strlen(path) + 1);
    args[0] = (void *)path;

    // Mode 
    arg_types[1] = (1u << ARG_INPUT) | (ARG_INT << 16u);
    args[1] = (void *)&mode;

    // Retcode
    arg_types[2] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
    int ret_code;
    args[2] = &ret_code;

    arg_types[3] = 0;

    int rpc_ret = rpcCall((char *)"unlock", arg_types, args);
    return (rpc_ret < 0) ? rpc_ret : ret_code;
}

