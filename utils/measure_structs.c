#include <stdio.h>
#include <stdint.h>

#define MAX_PATH_LEN 256
#define WAPP_MAX_NAME_LEN 15
#define WAPP_MAX_VERSION_LEN 15
#define MAX_OPTIONS_SIZE 1024
#define MAX_DRIVERS_CNT 10
#define WAPP_MAX_IMAGE_REF_LEN (WAPP_MAX_NAME_LEN + 1 + WAPP_MAX_VERSION_LEN)
#define WAPP_MAX_ARGS 8
#define WAPP_MAX_ARG_LEN 64
#define WAPP_MAX_ENVS 8
#define WAPP_MAX_ENV_LEN 64
#define TARFS_MAX_LAYERS 8

typedef struct wapp_driver_t {
    char name[WAPP_MAX_NAME_LEN];
    char path[MAX_PATH_LEN];
    char options[MAX_OPTIONS_SIZE];
} wapp_driver_t;

typedef struct wapp_config_t {
    uint8_t valid;
    char image[WAPP_MAX_IMAGE_REF_LEN];
    wapp_driver_t console[3];
    size_t driversCnt;
    wapp_driver_t drivers[MAX_DRIVERS_CNT];
    size_t mountsCnt;
    wapp_driver_t mounts[MAX_DRIVERS_CNT];
    size_t socketsCnt;
    wapp_driver_t sockets[MAX_DRIVERS_CNT];
    char args[WAPP_MAX_ARGS][WAPP_MAX_ARG_LEN];
    size_t argsCnt;
    char envs[WAPP_MAX_ENVS][WAPP_MAX_ENV_LEN];
    size_t envsCnt;
} wapp_config_t;

typedef struct wapp_t {
    char name[WAPP_MAX_NAME_LEN];
    char image[WAPP_MAX_NAME_LEN];
    char version[WAPP_MAX_VERSION_LEN];
    wapp_config_t cfg;
    uint8_t *layers[TARFS_MAX_LAYERS];
    size_t layer_lens[TARFS_MAX_LAYERS];
    uint8_t layer_cnt;
} wapp_t;

typedef struct wapp_data_t {
    uint8_t id;
    wapp_t *wapp;
    void* vfs;
    void* wamr;
    int lastStatus;
    int32_t exit_code;
} wapp_data_t;

#define VFS_MAX_FDS 32
#define VFS_MAX_MOUNTS 8
#define VFS_DEVFS_MAX_ENTRIES 10
#define VFS_PROCFS_MAX_ENTRIES 16
#define MAX_ENTRY_NAME_LEN 32

typedef struct vfs_fd_t {
    int type;
    void *internal_ctx;
    int flags;
    int rights;
    char path[64];
    const void *driver;
    int drv_fd;
} vfs_fd_t;

typedef struct vfs_named_drv_t {
    char name[MAX_ENTRY_NAME_LEN];
    const void *drv;
} vfs_named_drv_t;

typedef struct vfs_mount_t {
    char prefix[64];
    int type;
    const void *drv;
} vfs_mount_t;

typedef struct vfs_proc_entry_t {
    char name[MAX_ENTRY_NAME_LEN];
    void* read_fn;
    uint8_t privileged;
} vfs_proc_entry_t;

struct vfs_ctx_t {
    vfs_fd_t fds[VFS_MAX_FDS];
    void *tarfs;
    vfs_named_drv_t devfs[VFS_DEVFS_MAX_ENTRIES];
    uint8_t devfs_cnt;
    vfs_named_drv_t netfs[VFS_DEVFS_MAX_ENTRIES];
    uint8_t netfs_cnt;
    vfs_mount_t mounts[VFS_MAX_MOUNTS];
    uint8_t mounts_cnt;
    vfs_proc_entry_t procfs[VFS_PROCFS_MAX_ENTRIES];
    uint8_t procfs_cnt;
    uint8_t privileged;
};

#define WASI_MAX_PREOPENS 8
typedef struct wasi_preopen_t {
    char path[64];
    int  fd;
} wasi_preopen_t;

typedef struct wasi_ctx_t {
    int32_t     exit_code;
    uint32_t    argc;
    const char **argv;
    uint32_t    envc;
    const char **envp;
    void*   vfsCtx;
    wasi_preopen_t preopens[WASI_MAX_PREOPENS];
    uint8_t        preopens_cnt;
} wasi_ctx_t;

struct wamrData_t {
    void* module;
    void* instance;
    void* exec_env;
    uint8_t *wasm_bytes;
};

int main() {
    printf("wapp_config_t: %zu\n", sizeof(wapp_config_t));
    printf("wapp_t: %zu\n", sizeof(wapp_t));
    printf("wapp_data_t: %zu\n", sizeof(wapp_data_t));
    printf("struct vfs_ctx_t: %zu\n", sizeof(struct vfs_ctx_t));
    printf("wasi_ctx_t: %zu\n", sizeof(wasi_ctx_t));
    printf("struct wamrData_t: %zu\n", sizeof(struct wamrData_t));
    
    size_t total = sizeof(wapp_t) + sizeof(wapp_data_t) + sizeof(struct vfs_ctx_t) + sizeof(wasi_ctx_t) + sizeof(struct wamrData_t);
    printf("Total dynamic per-slot engine overhead (approx): %zu B\n", total);
    return 0;
}
