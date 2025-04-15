# simple-distributed-system

## Overview
![WatDFS Architecture](https://github.com/abc1203/simple-distributed-system/blob/main/img/DFS_architecture.png)

WatDFS is a distributed file system that provides transparent access to remote files using FUSE. It is implemented with the upload-download model: files are copied from the server to the client and the client performs all operations locally.

- **Download/Upload Mechanism**:  
  - Files are transferred atomically using `lock`/`unlock` RPC calls to ensure consistency
  - `download_file` fetches files from the server to the client cache; `upload_file` syncs local changes back

- **Mutual Exclusion**:  
  - **Server**: Uses reader-writer locks to enable concurrent reads but exclusive writes
  - **Client**: Tracks open files to prevent multiple writes

- **Timeout-Based Caching**:  
  - Files are validated using freshness checks
  - Stale files trigger re-downloads

- **RPC Operations**:  
  - Implements RPC calls including `getattr`, `mknod`, `open`, `read`, `write`, and custom `lock`/`unlock` for atomic transfers


## Usage

### 1. Compile the Project
```bash
make clean all
```

### 2. Start Server on Linux Terminal
```bash
mkdir -p /tmp/$USER/server
./watdfs_server /tmp/$USER/server
```
the terminal should output SERVER_ADDRESS and SERVER_PORT

### 3. Start Client on New Terminal
```bash
export SERVER_ADDRESS=${SERVER_ADDRESS}
export SERVER_PORT=${SERVER_PORT}
export CACHE_INTERVAL_SEC=${CACHE_INTERVAL}
mkdir -p /tmp/$USER/cache /tmp/$USER/mount
./watdfs_client -s -f -o direct_io /tmp/$USER/cache /tmp/$USER/mount
```

### 4. Run Bash Cmds on Client
```bash
touch /tmp/$USER/mount/myfile.txt # Create empty file
echo "Hello World" > /tmp/$USER/mount/myfile.txt # Write to a file
cat /tmp/$USER/mount/myfile.txt # Read file
stat /tmp/$USER/mount/myfile.txt # Get file attributes
```
