#ifndef NSBC_XML_PARSE_H
#define NSBC_XML_PARSE_H
#include "nsbc_common.h"

/* XML 体解析占位（PIDF / RLMI / SIPREC，RFC3863/RFC4662/draft-siprec）
 *
 * P0：仅做 XML 声明与长度/编码基本校验，完整解析在 P1 以 libxml2
 * （禁用外部实体、禁 DTD，防 XXE）替换本接口，调用方接口不变。
 */

typedef struct xml_doc xml_doc_t;

/* 基本健全性检查：存在 <?xml 或根元素、UTF-8、无 DOCTYPE（P0 拒绝） */
int xml_wellformed_check(const char *buf, size_t len);

/* 提取首个 <element ...>...</element> 的文本内容（浅解析，仅供占位逻辑；
 * 找不到返回 NSBC_NOENT）。out 以 NUL 结尾，超出截断。 */
int xml_element_text(const char *buf, size_t len, const char *element,
                     char *out, size_t outsz);

#endif
