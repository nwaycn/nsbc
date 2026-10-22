/* src/include/utils.h - Common utility helpers shared across modules
 *
 * Any C source file can include this header.
 * utils.c only depends on standard C + nsbc_common.h.
 */

#ifndef NSBC_UTILS_H
#define NSBC_UTILS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Directory/file helpers */
int mkdir_p(const char *path);    /* Recursively create directory (mkdir -p semantics) */

/* ---- Future extension points ----
 * str_trim / str_split / str_join
 * hex_encode / hex_decode / base64
 * safe_malloc / safe_calloc
 * ...
 */

#ifdef __cplusplus
}
#endif

#endif /* NSBC_UTILS_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
