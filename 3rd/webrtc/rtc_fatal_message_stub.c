/* rtc_FatalMessage stub — WebRTC checks.h C 版本唯一外部符号。
 * 生产构建（NDEBUG / -O2）下 RTC_DCHECK 是 no-op，不会调用到这里。
 * 真进来了说明 VAD 内部数据结构被破坏，直接 abort 即可。 */
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

void rtc_FatalMessage(const char *file, int line, const char *msg)
{
    fprintf(stderr, "WEBRTC FATAL: %s:%d %s\n", file, line, msg ? msg : "");
    fflush(stderr);
    abort();
}

#ifdef __cplusplus
}
#endif
