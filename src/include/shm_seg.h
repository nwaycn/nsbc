#ifndef NSBC_SHM_SEG_H
#define NSBC_SHM_SEG_H

/* 共享内存段（REQ-ARCH-005/REQ-ENV-001，P0 主备零切换 / P1 进程复用）
 *
 * 用途：主备/多进程（媒体面与控制面分进程的部署形态）间共享只读为主
 * 的大状态（在线绑定/位置库快照/端口表），MAP_SHARED 映射；
 * 热升级时新进程以同名 attach 继承状态（Hitless Upgrade REQ-HA-004）。
 *
 * 创建回退链：memfd_create(2)（Linux ≥3.17，无 /dev/shm 依赖）→
 * POSIX shm_open（/dev/shm，容器需可写）→ SystemV shmget（兜底）。
 * 映射优先提示固定 VA（ASLR 下失败重试任意 VA），首块放
 * shm_hdr_t（magic/version/size/世代号），attach 时校验拒绝错版本。
 * 分层：mem（1），仅依赖 base；锁原语用 nsbc_atomic，不引 pthread。
 */
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SHM_NAME_MAX     64
#define SHM_MAGIC        0x4e534243u  /* "NSBC" */
#define SHM_VERSION      1u

typedef enum {
    SHM_BACKEND_MEMFD = 0,
    SHM_BACKEND_POSIX,
    SHM_BACKEND_SYSV,
    SHM_BACKEND_NONE
} shm_backend_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t size;         /* 含本头 */
    uint64_t epoch;        /* 主世代（attach 方据此判陈旧） */
    uint32_t crc;          /* 负载 CRC32C（写者维护，0=尚未初始化） */
    uint32_t reserved0;
    uint64_t reserved1[4];
} shm_hdr_t;

typedef struct {
    char          name[SHM_NAME_MAX];
    size_t        size;           /* 调用方期望（不含 hdr，自动加头） */
    int           create;         /* 1=创建/截断；0=attach 既有 */
    void         *prefer_va;      /* NULL=内核选址 */
    shm_backend_t backend_force;  /* SHM_BACKEND_NONE=自动回退链 */
} shm_cfg_t;

typedef struct shm_seg shm_seg_t;

int  shm_seg_open(const shm_cfg_t *cfg, shm_seg_t **out);
void shm_seg_close(shm_seg_t *seg);
/* 卸载名字（最后一个 close 后回收；SysV 为 IPC_RMID 延迟到无映射） */
int  shm_seg_unlink(const char *name);

void       *shm_addr(shm_seg_t *seg);  /* 含 hdr 的映射首址 */
shm_hdr_t  *shm_hdr(shm_seg_t *seg);
size_t      shm_total_size(shm_seg_t *seg);
shm_backend_t shm_backend(shm_seg_t *seg);
int         shm_is_creator(const shm_seg_t *seg);
const char *shm_seg_name(const shm_seg_t *seg);  /* 日志/诊断用段名 */

/* msync/写者发布：更新 epoch 后调 publish（粗实现 msync ASYNC） */
int  shm_publish(shm_seg_t *seg, uint64_t epoch, uint32_t crc);
/* 读者校验：magic/version 不匹配返回 PROTO；epoch 出参可空 */
int  shm_validate(shm_seg_t *seg, uint64_t *epoch, uint32_t *crc);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_SHM_SEG_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
