/* 会议室——粗骨架（REQ-MEET-001~004，P1）
 *
 * 装配：conf_ops 由 applications 模块 init 时注入（mix_* 桥接
 * media/mixer.c；record_* 桥接 recorder.c；play_room 走 player）。
 * 房间生命周期与呼叫解耦：房间长期存在，成员随呼叫进出；房间销毁
 * 时（管理面）先 detach 全部腿。
 */
#include "conference.h"

typedef struct {
    conf_room_t   room;
    conf_member_t members[CONF_MEMBER_MAX];
    int           n_members;
} conf_room_slot_t;

struct conf_mgr {
    conf_ops_t      ops;
    conf_room_slot_t rooms[CONF_ROOM_CAP];
    int             n;
};

int conf_create(const conf_ops_t *ops, conf_mgr_t **out)
{
    conf_mgr_t *m;
    if (ops == NULL || out == NULL || ops->mix_attach == NULL ||
        ops->mix_detach == NULL)
        return NSBC_INVAL;
    m = (conf_mgr_t *)calloc(1, sizeof(*m));
    if (m == NULL)
        return NSBC_NOMEM;
    m->ops = *ops;
    *out = m;
    return NSBC_OK;
}

void conf_destroy(conf_mgr_t *m)
{
    free(m);
}

static conf_room_slot_t *room_find(conf_mgr_t *m, const char *tenant,
                                   const char *name)
{
    int i;
    for (i = 0; i < m->n; ++i) {
        if (strcmp(m->rooms[i].room.tenant, tenant) == 0 &&
            strcmp(m->rooms[i].room.name, name) == 0)
            return &m->rooms[i];
    }
    return NULL;
}

int conf_room_add(conf_mgr_t *m, const conf_room_t *r)
{
    conf_room_slot_t *s;
    if (m == NULL || r == NULL || r->name[0] == '\0')
        return NSBC_INVAL;
    s = room_find(m, r->tenant, r->name);
    if (s != NULL) {
        s->room = *r;
        return NSBC_OK;
    }
    if (m->n >= CONF_ROOM_CAP)
        return NSBC_NOSPC;
    s = &m->rooms[m->n++];
    s->room = *r;
    return NSBC_OK;
}

int conf_room_remove(conf_mgr_t *m, const char *tenant, const char *name)
{
    int i;
    for (i = 0; i < m->n; ++i) {
        if (strcmp(m->rooms[i].room.tenant, tenant) == 0 &&
            strcmp(m->rooms[i].room.name, name) == 0) {
            if (m->rooms[i].n_members > 0)
                return NSBC_AGAIN; /* 有人不允许直接删（先清场） */
            m->rooms[i] = m->rooms[--m->n];
            return NSBC_OK;
        }
    }
    return NSBC_NOENT;
}

const conf_room_t *conf_room_get(conf_mgr_t *m, const char *tenant,
                                 const char *name)
{
    conf_room_slot_t *s = room_find(m, tenant, name);
    return s == NULL ? NULL : &s->room;
}

static conf_member_t *member_by_key(conf_room_slot_t *s, const void *call_key)
{
    int i;
    for (i = 0; i < s->n_members; ++i) {
        if (s->members[i].call_key == call_key)
            return &s->members[i];
    }
    return NULL;
}

int conf_join(conf_mgr_t *m, const char *tenant, const char *name,
              const char *ext, const char *pin, void *call_key)
{
    conf_room_slot_t *s;
    conf_member_t *mem;
    conf_role_t role;
    int rc;
    if (m == NULL || ext == NULL || call_key == NULL)
        return NSBC_INVAL;
    s = room_find(m, tenant, name);
    if (s == NULL)
        return NSBC_NOENT;
    if (s->room.locked)
        return NSBC_PERM_DENIED;
    if (s->n_members >= s->room.max_members)
        return NSBC_NOSPC;
    if (member_by_key(s, call_key) != NULL)
        return NSBC_EXISTS;
    /* PIN 判定：主持 PIN > 成员 PIN；公开房首位为主持人 */
    if (s->room.mod_pin[0] != '\0' && pin != NULL &&
        strcmp(pin, s->room.mod_pin) == 0)
        role = CONF_ROLE_MODERATOR;
    else if (s->room.pin[0] != '\0' && pin != NULL &&
             strcmp(pin, s->room.pin) == 0)
        role = CONF_ROLE_GUEST;
    else if (s->room.pin[0] == '\0' && s->n_members == 0)
        role = CONF_ROLE_MODERATOR;
    else if (s->room.pin[0] == '\0')
        role = CONF_ROLE_GUEST;
    else
        return NSBC_AUTH;
    rc = m->ops.mix_attach(m->ops.ud, name, call_key);
    if (rc != NSBC_OK)
        return rc;
    mem = &s->members[s->n_members++];
    memset(mem, 0, sizeof(*mem));
    snprintf(mem->ext, sizeof(mem->ext), "%s", ext);
    mem->call_key = call_key;
    mem->role = role;
    if (s->room.announce_join_leave && m->ops.play_room != NULL)
        (void)m->ops.play_room(m->ops.ud, name, "conf/join");
    return NSBC_OK;
}

int conf_leave(conf_mgr_t *m, const char *tenant, const char *name,
               void *call_key)
{
    conf_room_slot_t *s;
    int i;
    if (m == NULL || call_key == NULL)
        return NSBC_INVAL;
    s = room_find(m, tenant, name);
    if (s == NULL)
        return NSBC_NOENT;
    for (i = 0; i < s->n_members; ++i) {
        if (s->members[i].call_key == call_key) {
            (void)m->ops.mix_detach(m->ops.ud, name, call_key);
            s->members[i] = s->members[--s->n_members];
            if (s->room.announce_join_leave && m->ops.play_room != NULL)
                (void)m->ops.play_room(m->ops.ud, name, "conf/leave");
            return NSBC_OK;
        }
    }
    return NSBC_NOENT;
}

int conf_dtmf_cmd(conf_mgr_t *m, const char *tenant, const char *name,
                  const void *call_key, const char *cmd)
{
    conf_room_slot_t *s;
    conf_member_t *mem;
    if (m == NULL || call_key == NULL || cmd == NULL)
        return NSBC_INVAL;
    s = room_find(m, tenant, name);
    if (s == NULL)
        return NSBC_NOENT;
    mem = member_by_key(s, call_key);
    if (mem == NULL)
        return NSBC_NOENT;
    if (strcmp(cmd, "*1") == 0) {
        mem->muted = !mem->muted; /* TODO：媒体面按 member 静音入混音 */
        return NSBC_OK;
    }
    if (strcmp(cmd, "*2") == 0) {
        mem->talking = 1; /* TODO：举手→主持人提示（P1） */
        return NSBC_OK;
    }
    if (strcmp(cmd, "*3") == 0 || strcmp(cmd, "*4") == 0) {
        if (mem->role != CONF_ROLE_MODERATOR)
            return NSBC_PERM_DENIED;
        if (strcmp(cmd, "*3") == 0) {
            s->room.locked = !s->room.locked;
        } else if (m->ops.record_start != NULL) {
            s->room.record_enabled = !s->room.record_enabled;
            /* TODO：path 由组合根按 tenant/room/时间戳生成 */
            if (s->room.record_enabled)
                (void)m->ops.record_start(m->ops.ud, name, "conf/rec");
            else
                (void)m->ops.record_stop(m->ops.ud, name);
        }
        return NSBC_OK;
    }
    return NSBC_NOENT;
}

int conf_member_count(conf_mgr_t *m, const char *tenant, const char *name,
                      int *total, int *moderators)
{
    const conf_room_slot_t *s;
    int i, mods = 0;
    if (m == NULL)
        return NSBC_INVAL;
    s = room_find(m, tenant, name);
    if (s == NULL)
        return NSBC_NOENT;
    for (i = 0; i < s->n_members; ++i) {
        if (s->members[i].role == CONF_ROLE_MODERATOR)
            ++mods;
    }
    if (total != NULL)
        *total = s->n_members;
    if (moderators != NULL)
        *moderators = mods;
    return NSBC_OK;
}

int conf_set_talking(conf_mgr_t *m, const void *call_key, int talking)
{
    int ri;
    if (m == NULL || call_key == NULL)
        return NSBC_INVAL;
    /* 成员跨房间线性定位（粗骨架；接线批用 call_key→member 反查表） */
    for (ri = 0; ri < m->n; ++ri) {
        conf_member_t *mem = member_by_key(&m->rooms[ri], call_key);
        if (mem != NULL) {
            mem->talking = talking;
            return NSBC_OK;
        }
    }
    return NSBC_NOENT;
}
