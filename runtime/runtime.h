/* runtime.h — internal interface of the host runtime (shared by all shims). */
#ifndef RUNTIME_H
#define RUNTIME_H

#include <stdint.h>

void  rt_log(const char *fmt, ...);
void  rt_fatal(const char *fmt, ...);
void *rt_bad_code_ptr(unsigned int va);
int   image_load(const char *path);
int   image_load_ddse(const char *path);
unsigned int recomp_host_to_va(const void *p);

/* data directory on the target ("ux0:data/dayusadx/" on the Vita) */
const char *rt_data_dir(void);
/* map a game path (DOS style, relative or "X:\..." absolute) to a host path */
void  rt_map_path(const char *in, char *out, int outsz);

#endif
