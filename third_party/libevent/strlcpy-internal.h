#ifndef _STRLCPY_INTERNAL_H_
#define _STRLCPY_INTERNAL_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifndef NO_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#ifndef HAVE_STRLCPY
#include <string.h>
size_t _event_strlcpy(char *dst, const char *src, size_t siz);
#define strlcpy _event_strlcpy
#endif

#ifdef __cplusplus
}
#endif

#endif

