#ifndef NSBC_VOICEMAIL_H
#define NSBC_VOICEMAIL_H

/* 语音信箱（REQ-PBX-VM-001~004，P1；接线轮廓本批落地）
 *
 * 能力：主人问候语播放、留言录制（接 media/recorder）、听取/删除留言、
 * MWI（RFC3842 message-summary 事件包，经 sip_event SUBSCRIBE/NOTIFY）、
 * 留言存储路径/保留期策略。
 * 留言文件由 recorder 产出；本模块只管信箱元数据与状态，不直接碰
 * 媒体（放/录经组合根回调，call_key 不透明）。
 */
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VM_TENANT_MAX   32
#define VM_EXT_MAX      32
#define VM_PATH_MAX     160
#define VM_GREET_MAX    VM_PATH_MAX
#define VM_CAP          200000
#define VM_MSG_PER_BOX  200

typedef enum {
    VM_MSG_NEW = 0,
    VM_MSG_READ = 1
} vm_msg_state_t;

typedef struct {
    char    path[VM_PATH_MAX]; /* 录音文件相对路径（recorder 产物） */
    char    from[VM_EXT_MAX];  /* 主叫号 */
    uint64_t ts_ms;
    uint32_t duration_ms;
    vm_msg_state_t state;
} vm_message_t;

typedef struct {
    char tenant[VM_TENANT_MAX];
    char ext[VM_EXT_MAX];
    char greeting[VM_GREET_MAX];
    int  enabled;
    int  pin_required;         /* 听取留言是否校验 PIN（走 authz） */
    vm_message_t msgs[VM_MSG_PER_BOX];
    int  n_new;
    int  n_total;
} vm_box_t;

/* 媒体回调（play/record 经 media 层；notify_mwi 经 sip_event 发
 * NOTIFY message-summary，组合根装配） */
typedef struct {
    int  (*play)(void *ud, void *call_key, const char *path, int loop);
    int  (*record_start)(void *ud, void *call_key, const char *path);
    int  (*record_stop)(void *ud, void *call_key, uint32_t *dur_ms);
    int  (*notify_mwi)(void *ud, const char *tenant, const char *ext,
                       int n_new, int n_total);
    void *ud;
} vm_ops_t;

typedef struct vm_store vm_store_t;

int  vm_create(const vm_ops_t *ops, vm_store_t **out);
void vm_destroy(vm_store_t *s);

int  vm_box_enable(vm_store_t *s, const char *tenant, const char *ext,
                   const char *greeting, int pin_required);
int  vm_box_lookup(vm_store_t *s, const char *tenant, const char *ext,
                   vm_box_t *out);

/* 来电流转：播问候→录音；record_path 由调用方（组合根）分配 */
int  vm_leave_begin(vm_store_t *s, const char *tenant, const char *ext,
                    void *call_key);
int  vm_leave_end(vm_store_t *s, const char *tenant, const char *ext,
                  const char *from, const char *path, uint32_t dur_ms);

/* 主人听取：列出/标记已读/删除（粗骨架：序号访问） */
int  vm_list(vm_store_t *s, const char *tenant, const char *ext,
             vm_message_t *out, int cap, int *total);
int  vm_mark_read(vm_store_t *s, const char *tenant, const char *ext,
                  int idx);
int  vm_delete(vm_store_t *s, const char *tenant, const char *ext, int idx);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_VOICEMAIL_H */
