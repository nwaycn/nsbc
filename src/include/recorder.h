#ifndef NSBC_RECORDER_H
#define NSBC_RECORDER_H

/* 通话录音（REQ-REC-001~004，P1；recording mod 为空占位）
 *
 * 三种模式（对齐 SRS REQ-REC-002）：
 *   SINGLE 单腿、MIXED 双腿混音一文件（复用 mixer 归一化）、
 *   SEPARATE 双腿两文件；
 * 存储：本地/NFS/对象存储（异步 worker group commit，DB 只存索引）；
 * 格式容器（formats 模块，REQ-MEDIA-FORMAT）：WAV(PCM) 基线、
 * OGG/Opus P1；加密静态（等保/GDPR 录音隐私，密钥走 cfg_crypto）。
 *
 * 分层：media（消费 PCM 帧）；业务启停策略（全程/按需/强制）在 mod，
 * sip 层不引本头——启停经 core/signaling 回调槽装配。
 */
#include "nsbc_common.h"
#include "media_codec.h" /* media_frame_t */

#ifdef __cplusplus
extern "C" {
#endif

#define REC_CALLID_MAX   128
#define REC_PATH_MAX     256
#define REC_REASON_MAX   64

typedef enum {
    REC_MODE_SINGLE = 0,
    REC_MODE_MIXED,
    REC_MODE_SEPARATE
} rec_mode_t;

typedef enum {
    REC_FMT_WAV_PCM = 0,  /* P1 基线容器 */
    REC_FMT_OGG_OPUS,     /* P1 节省存储（专利无关） */
    REC_FMT_MP3           /* P2/商业（专利许可风险登记） */
} rec_fmt_t;

typedef enum {
    REC_ST_IDLE = 0,
    REC_ST_RECORDING,
    REC_ST_PAUSED,
    REC_ST_STOPPED,
    REC_ST_FAILED
} rec_state_t;

typedef struct rec_mgr rec_mgr_t;
typedef struct rec_sess rec_sess_t;

typedef struct {
    char        base_dir[REC_PATH_MAX]; /* /var/lib/nsbc/recordings */
    rec_fmt_t   fmt;
    int         rate;
    int         channels;
    int         max_concurrent;
    int         encrypt_rest;   /* 0600 + AES-256-GCM（REQ-SEC 隐私） */
    uint64_t    max_bytes;      /* 单文件硬上限（超限自动切段） */
} rec_mgr_cfg_t;

typedef struct {
    rec_mode_t mode;
    char       callid[REC_CALLID_MAX];
    char       reason[REC_REASON_MAX]; /* 启动来源：always/dtmf/manual */
    int        leg_mask;              /* bit0=A bit1=B（SINGLE 时指腿） */
    /* §18 可选：自定义录音路径模板。空串=系统默认 yyyymmdd/callid_mix_seq.wav
     * 变量替换：{caller} {callee} {callid} {date} {time} */
    char       rec_path_template[128];
    char       caller[64];   /* 主叫号，供 {caller} 替换 */
    char       callee[64];   /* 被叫号，供 {callee} 替换 */
} rec_open_t;

int  rec_mgr_create(const rec_mgr_cfg_t *cfg, rec_mgr_t **out);
void rec_mgr_destroy(rec_mgr_t *m);
/* REQ-REC-007：返回 mgr 的配置指针（只读，供 resolve_rec_path_template 拿 base_dir） */
const rec_mgr_cfg_t *rec_mgr_get_cfg(rec_mgr_t *m);

/* 打开即落临时文件头（tmp+rename 原子收尾）；NOSPC=达并发/磁盘水位 */
int  rec_open(rec_mgr_t *m, const rec_open_t *req, rec_sess_t **out);
int  rec_pause(rec_sess_t *s);
int  rec_resume(rec_sess_t *s);
/* 停止并 fsync/关文件，输出最终相对路径与字节数 */
int  rec_stop(rec_sess_t *s, char *path_out, size_t path_cap,
              uint64_t *bytes_out);
rec_state_t rec_state(const rec_sess_t *s);

/* 喂 PCM 帧（媒体快路径旁路：经 SPSC 队列入录音 worker，不阻塞转发；
 * leg=0/1；MIXED 模式内部复用 mixer，本 API 逐腿喂）。 */
int  rec_feed(rec_sess_t *s, int leg, const media_frame_t *frm);

/* REQ-REC-006：喂原始 RTP payload（G.711a/u 直写 .g711a 文件，无需 PCM 解码）
 * 无 WAV 头（纯原始字节），文件扩展名按 payload_type 映射：
 *   pt=0 (PCMU) → .g711u；pt=8 (PCMA) → .g711a；其他 pt → .raw
 * leg=0/1。rec_open 成功后即可调，内部若未开 raw_fp 则自动 open（延迟创建）。 */
int  rec_feed_raw(rec_sess_t *s, int leg, const void *payload,
                  size_t len, int payload_type);

/* 静音检测切段（VAD 长静是否停文件，策略由调用方决定） */
int  rec_is_silence(const media_frame_t *frm, int threshold_dbov);

/* REQ-REC-007：rec_path_template 变量替换
 * 模板中 {caller} {callee} {callid} {date} {time} 会被替换为实际值；
 * date=YYYYMMDD，time=HHMMSS（UTC）。template 为空则 out 写默认 yyyymmdd/callid_mix.wav */
int resolve_rec_path_template(char *out, size_t out_cap,
                              const char *template,
                              const char *caller, const char *callee,
                              const char *callid, const char *base_dir);

/* REQ-REC-008：合并两条 mono WAV → stereo WAV（A=左 B=右）
 * 读取 a_path/b_path 的 PCM 数据 → 交错写 stereo → 写到 resolve 后的 mix_path。
 * 短腿自动补零静音对齐。成功返回 NSBC_OK。 */
int merge_stereo_wav(const char *a_path, const char *b_path,
                     const char *out_path);

/* OAM 计数 */
typedef struct {
    uint64_t active;
    uint64_t total;
    uint64_t failed;
    uint64_t bytes_written;
} rec_stat_t;
void rec_mgr_stat(const rec_mgr_t *m, rec_stat_t *out);

/* ---------- 模块层接线 API ----------
 * 移动到 src/include/recording.h —— 那里定义了
 *   recording_apply_dp_action() / recording_feed_frame() / recording_parse_action()
 * 本头只保留纯录制设备 API，避免 media 反向 include module 层。 */

#ifdef __cplusplus
}
#endif
#endif /* NSBC_RECORDER_H */
