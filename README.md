# simple-distributed-system
A simple distributed file system built using FUSE and RPC.

![WatDFS Architecture](image.png)

## Overview
WatDFS is a distributed file system that provides transparent access to remote files using FUSE (Filesystem in Userspace). It operates in two modes:  
1. **Remote Access (P1)**: Directly forwards file operations to a remote server via RPC.  
2. **Upload/Download (P2)**: Caches files locally, enforces mutual exclusion, and syncs changes with the server based on timeout-based freshness checks.  

The system comprises:  
- **Client**: Mounts a FUSE filesystem, handles local caching, and communicates with the server via RPC.  
- **Server**: Manages remote file operations and enforces write mutual exclusion.  

## Implementation Details

### Key Features
- **Download/Upload Mechanism**:  
  - Files are transferred atomically using `lock`/`unlock` RPC calls to ensure consistency.  
  - `download_file` fetches files from the server to the client cache; `upload_file` syncs local changes back.  

- **Mutual Exclusion**:  
  - **Server**: Uses reader-writer locks (`rw_lock.h`) to allow concurrent reads but exclusive writes.  
  - **Client**: Tracks open files to prevent multiple writes (returns `-EMFILE` if conflicting opens occur).  

- **RPC Operations**:  
  - Implements RPC calls like `getattr`, `mknod`, `open`, `read`, `write`, and custom `lock`/`unlock` for atomic transfers.  
  - Follows strict protocol definitions (Section 4.1 of specs) for argument serialization.  

- **Timeout-Based Caching**:  
  - Files are validated using freshness checks (`Tc` timestamp) against a `cache_interval`. Stale files trigger re-downloads.  

## Demo: Running WatDFS

### 1. Compile the Project
```bash
make clean all  # Compiles libwatdfs.a, watdfs_server, and watdfs_client
