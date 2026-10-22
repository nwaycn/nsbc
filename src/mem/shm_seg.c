/* 共享内存段——粗骨架（REQ-ARCH-005/REQ-ENV-001，P0/P1）
 *
 * 实现状态：__linux__ 下给真实回退链系统调用轮廓（memfd_create→
 * shm_open→shmget），开发机无 Linux 不编译；非 Linux 返回 NOTSUP。
 * 注意：当前未接任何调用方（组合根在 HA 接线批装配），不要在汇报中
 * 声称已双机验证。
 */
#include "shm_seg.h"

#ifdef __linux__
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000 /* Linux 4.17+；旧内核 mmap 返回 EINVAL，走下方任意选址回退 */
#endif
#endif

struct shm_seg {
    char          name[SHM_NAME_MAX];
    void         *va;
    size_t        total;
    shm_backend_t backend;
    int           creator;
#ifdef __linux__
    int           fd;          /* memfd/posix；SysV 存 -1 */
    int           sysv_id;
#endif
};

#ifdef __linux__
static int create_memfd(const char *name, size_t size, shm_seg_t *seg,
                        int do_create)
{
    int fd;
    /* memfd_create 无名字持久性，只适合 create；attach 回退到后两级。
     * x86_64 __NR_memfd_create=319；正式接线批改 <sys/syscall.h> 的
     * SYS_memfd_create 并为 aarch64 提供分支。 */
    if (!do_create)
        return NSBC_NOENT;
    fd = (int)syscall(319, name, (unsigned)MFD_CLOEXEC);
    if (fd < 0)
        return NSBC_NOENT;
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return NSBC_ERR;
    }
    seg->fd = fd;
    seg->backend = SHM_BACKEND_MEMFD;
    return NSBC_OK;
}

static int create_posix(const char *name, size_t size, shm_seg_t *seg,
                        int do_create)
{
    int flags = do_create ? (O_CREAT | O_EXCL | O_RDWR) : O_RDWR;
    int fd = shm_open(name, flags, 0640);
    if (fd < 0 && do_create && errno == EEXIST) {
        /* 残留段：attach 并校验长度（不截断，避免踩活数据） */
        fd = shm_open(name, O_RDWR, 0640);
    }
    if (fd < 0)
        return NSBC_NOENT;
    if (do_create && ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return NSBC_ERR;
    }
    seg->fd = fd;
    seg->backend = SHM_BACKEND_POSIX;
    return NSBC_OK;
}

static int map_segment(shm_seg_t *seg, size_t size, const shm_cfg_t *cfg,
                       int do_create)
{
    int flags = MAP_SHARED;
    void *va;
    (void)cfg;
    if (cfg->prefer_va != NULL)
        flags |= MAP_FIXED_NOREPLACE; /* Linux 4.17+；旧内核退回任意选址 */
    va = mmap(cfg->prefer_va, size, PROT_READ | PROT_WRITE, flags,
              seg->fd, 0);
    if (va == MAP_FAILED && cfg->prefer_va != NULL)
        va = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  seg->fd, 0);
    if (va == MAP_FAILED)
        return NSBC_ERR;
    seg->va = va;
    seg->total = size;
    seg->creator = do_create;
    if (do_create) {
        shm_hdr_t *h = (shm_hdr_t *)va;
        memset(va, 0, size);
        h->magic = SHM_MAGIC;
        h->version = SHM_VERSION;
        h->size = size;
    }
    return NSBC_OK;
}
#endif /* __linux__ */

int shm_seg_open(const shm_cfg_t *cfg, shm_seg_t **out)
{
#ifdef __linux__
    shm_seg_t *seg;
    size_t total;
    int rc;
    if (cfg == NULL || out == NULL || cfg->name[0] == '\0' ||
        cfg->size == 0)
        return NSBC_INVAL;
    total = cfg->size + sizeof(shm_hdr_t);
    seg = (shm_seg_t *)calloc(1, sizeof(*seg));
    if (seg == NULL)
        return NSBC_NOMEM;
    seg->fd = -1;
    seg->sysv_id = -1;
    snprintf(seg->name, sizeof(seg->name), "%s", cfg->name);
    if (cfg->backend_force == SHM_BACKEND_MEMFD ||
        cfg->backend_force == SHM_BACKEND_NONE) {
        rc = create_memfd(seg->name, total, seg, cfg->create);
        if (rc == NSBC_OK)
            rc = map_segment(seg, total, cfg, cfg->create);
        if (rc != NSBC_OK && cfg->backend_force == SHM_BACKEND_MEMFD)
            goto fail;
    }
    if (seg->va == NULL &&
        (cfg->backend_force == SHM_BACKEND_POSIX ||
         cfg->backend_force == SHM_BACKEND_NONE)) {
        rc = create_posix(seg->name, total, seg, cfg->create);
        if (rc == NSBC_OK)
            rc = map_segment(seg, total, cfg, cfg->create);
        if (rc != NSBC_OK && cfg->backend_force == SHM_BACKEND_POSIX)
            goto fail;
    }
    if (seg->va == NULL &&
        (cfg->backend_force == SHM_BACKEND_SYSV ||
         cfg->backend_force == SHM_BACKEND_NONE)) {
        /* TODO(接线批)：shmget(IPC_CREAT|0640, size) + shmat 兜底；
         * 键值由 name ftok 派生。粗骨架留待 HA 装配时补。 */
        goto fail;
    }
    *out = seg;
    return NSBC_OK;
fail:
    if (seg->fd >= 0)
        close(seg->fd);
    free(seg);
    return NSBC_ERR;
#else
    (void)cfg;
    (void)out;
    return NSBC_NOTSUP;
#endif
}

void shm_seg_close(shm_seg_t *seg)
{
    if (seg == NULL)
        return;
#ifdef __linux__
    if (seg->va != NULL)
        munmap(seg->va, seg->total);
    if (seg->fd >= 0)
        close(seg->fd);
#endif
    free(seg);
}

int shm_seg_unlink(const char *name)
{
    if (name == NULL)
        return NSBC_INVAL;
#ifdef __linux__
    return shm_unlink(name) == 0 ? NSBC_OK : NSBC_ERR;
#else
    (void)name;
    return NSBC_NOTSUP;
#endif
}

void *shm_addr(shm_seg_t *seg)
{
    return seg == NULL ? NULL : seg->va;
}

shm_hdr_t *shm_hdr(shm_seg_t *seg)
{
    return seg == NULL || seg->va == NULL ? NULL
                                          : (shm_hdr_t *)seg->va;
}

size_t shm_total_size(shm_seg_t *seg)
{
    return seg == NULL ? 0 : seg->total;
}

shm_backend_t shm_backend(shm_seg_t *seg)
{
    return seg == NULL ? SHM_BACKEND_NONE : seg->backend;
}

int shm_is_creator(const shm_seg_t *seg)
{
    return seg != NULL && seg->creator;
}

const char *shm_seg_name(const shm_seg_t *seg)
{
    return seg == NULL ? "" : seg->name;
}

int shm_publish(shm_seg_t *seg, uint64_t epoch, uint32_t crc)
{
    shm_hdr_t *h;
#ifdef __linux__
    int rc;
#endif
    if (seg == NULL || !seg->creator)
        return NSBC_INVAL;
    h = (shm_hdr_t *)seg->va;
    h->epoch = epoch;
    h->crc = crc;
#ifdef __linux__
    rc = msync(seg->va, seg->total, MS_ASYNC);
    return rc == 0 ? NSBC_OK : NSBC_ERR;
#else
    return NSBC_NOTSUP;
#endif
}

int shm_validate(shm_seg_t *seg, uint64_t *epoch, uint32_t *crc)
{
    const shm_hdr_t *h;
    if (seg == NULL || seg->va == NULL)
        return NSBC_INVAL;
    h = (shm_hdr_t *)seg->va;
    if (h->magic != SHM_MAGIC || h->version != SHM_VERSION ||
        h->size != seg->total)
        return NSBC_PROTO;
    if (epoch != NULL)
        *epoch = h->epoch;
    if (crc != NULL)
        *crc = h->crc;
    return NSBC_OK;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
