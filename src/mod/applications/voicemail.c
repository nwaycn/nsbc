/* 语音信箱——粗骨架（REQ-PBX-VM-001~004，P1）
 *
 * 装配：组合根（applications init）注入 vm_ops：
 *  play/record 走 media；notify_mwi 走 sip_event 发 NOTIFY（包
 *  message-summary，body "Messages-Waiting: yes\r\nVoice-Message:
 *  N/M ..."）。留言落盘由 recorder 管生命周期，DBAL 只存元数据行。
 * 存储：粗骨架定槽数组；接线批换分片哈希+DBAL 持久化+保留期清理。
 */
#include "voicemail.h"

struct vm_store {
    vm_ops_t ops;
    vm_box_t boxes[VM_CAP];
    int      n;
};

int vm_create(const vm_ops_t *ops, vm_store_t **out)
{
    vm_store_t *s;
    if (out == NULL || ops == NULL || ops->play == NULL ||
        ops->record_start == NULL || ops->record_stop == NULL)
        return NSBC_INVAL;
    s = (vm_store_t *)calloc(1, sizeof(*s));
    if (s == NULL)
        return NSBC_NOMEM;
    s->ops = *ops;
    *out = s;
    return NSBC_OK;
}

void vm_destroy(vm_store_t *s)
{
    free(s);
}

static vm_box_t *box_find(vm_store_t *s, const char *tenant, const char *ext)
{
    int i;
    for (i = 0; i < s->n; ++i) {
        if (strcmp(s->boxes[i].tenant, tenant) == 0 &&
            strcmp(s->boxes[i].ext, ext) == 0)
            return &s->boxes[i];
    }
    return NULL;
}

int vm_box_enable(vm_store_t *s, const char *tenant, const char *ext,
                  const char *greeting, int pin_required)
{
    vm_box_t *b = box_find(s, tenant, ext);
    if (b == NULL) {
        if (s->n >= VM_CAP)
            return NSBC_NOSPC;
        b = &s->boxes[s->n++];
        snprintf(b->tenant, sizeof(b->tenant), "%s", tenant);
        snprintf(b->ext, sizeof(b->ext), "%s", ext);
    }
    if (greeting != NULL)
        snprintf(b->greeting, sizeof(b->greeting), "%s", greeting);
    b->pin_required = pin_required;
    b->enabled = 1;
    return NSBC_OK;
}

int vm_box_lookup(vm_store_t *s, const char *tenant, const char *ext,
                  vm_box_t *out)
{
    const vm_box_t *b;
    if (s == NULL || tenant == NULL || ext == NULL || out == NULL)
        return NSBC_INVAL;
    b = box_find(s, tenant, ext);
    if (b == NULL || !b->enabled)
        return NSBC_NOENT;
    *out = *b;
    return NSBC_OK;
}

int vm_leave_begin(vm_store_t *s, const char *tenant, const char *ext,
                   void *call_key)
{
    vm_box_t *b;
    if (s == NULL || call_key == NULL)
        return NSBC_INVAL;
    b = box_find(s, tenant, ext);
    if (b == NULL || !b->enabled)
        return NSBC_NOENT;
    /* 播问候（默认问候可由 Web 上传；空路径走系统缺省） */
    return s->ops.play(s->ops.ud, call_key,
                       b->greeting[0] ? b->greeting : "vm/default", 0);
    /* TODO：问候播毕调 record_start；挂机/静音超时调 record_stop
     * 并回填 vm_leave_end（组合根在媒体事件回调里驱动）。 */
}

int vm_leave_end(vm_store_t *s, const char *tenant, const char *ext,
                 const char *from, const char *path, uint32_t dur_ms)
{
    vm_box_t *b;
    vm_message_t *m;
    if (s == NULL || tenant == NULL || ext == NULL || path == NULL)
        return NSBC_INVAL;
    b = box_find(s, tenant, ext);
    if (b == NULL || b->n_total >= VM_MSG_PER_BOX)
        return b == NULL ? NSBC_NOENT : NSBC_NOSPC;
    m = &b->msgs[b->n_total];
    memset(m, 0, sizeof(*m));
    snprintf(m->path, sizeof(m->path), "%s", path);
    if (from != NULL)
        snprintf(m->from, sizeof(m->from), "%s", from);
    m->duration_ms = dur_ms;
    m->state = VM_MSG_NEW;
    ++b->n_new;
    ++b->n_total;
    if (s->ops.notify_mwi != NULL)
        (void)s->ops.notify_mwi(s->ops.ud, tenant, ext,
                                b->n_new, b->n_total);
    return NSBC_OK;
}

int vm_list(vm_store_t *s, const char *tenant, const char *ext,
            vm_message_t *out, int cap, int *total)
{
    vm_box_t *b;
    int n;
    if (s == NULL || out == NULL || cap <= 0)
        return NSBC_INVAL;
    b = box_find(s, tenant, ext);
    if (b == NULL)
        return NSBC_NOENT;
    n = b->n_total < cap ? b->n_total : cap;
    memcpy(out, b->msgs, (size_t)n * sizeof(*out));
    if (total != NULL)
        *total = b->n_total;
    return n;
}

static vm_message_t *msg_at(vm_box_t *b, int idx)
{
    if (idx < 0 || idx >= b->n_total)
        return NULL;
    /* n_total 即数组槽位（删除时压缩），直接索引 */
    return &b->msgs[idx];
}

int vm_mark_read(vm_store_t *s, const char *tenant, const char *ext, int idx)
{
    vm_box_t *b = box_find(s, tenant, ext);
    vm_message_t *m;
    if (b == NULL)
        return NSBC_NOENT;
    m = msg_at(b, idx);
    if (m == NULL)
        return NSBC_INVAL;
    if (m->state == VM_MSG_NEW) {
        m->state = VM_MSG_READ;
        --b->n_new;
        if (s->ops.notify_mwi != NULL)
            (void)s->ops.notify_mwi(s->ops.ud, tenant, ext,
                                    b->n_new, b->n_total);
    }
    return NSBC_OK;
}

int vm_delete(vm_store_t *s, const char *tenant, const char *ext, int idx)
{
    vm_box_t *b = box_find(s, tenant, ext);
    vm_message_t *m;
    if (b == NULL)
        return NSBC_NOENT;
    m = msg_at(b, idx);
    if (m == NULL)
        return NSBC_INVAL;
    if (m->state == VM_MSG_NEW)
        --b->n_new;
    /* TODO：组合根异步 unlink 录音文件（失败只告警，不阻塞删元数据） */
    *m = b->msgs[b->n_total - 1];
    --b->n_total;
    if (s->ops.notify_mwi != NULL)
        (void)s->ops.notify_mwi(s->ops.ud, tenant, ext,
                                b->n_new, b->n_total);
    return NSBC_OK;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
