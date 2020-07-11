#if !defined(OPS_H)
#define OPS_H

#include <stdbool.h>

bool do_scrub(const char *mountpoint, bool verbose);
bool do_devstats(const char *mountpoint, bool verbose);
bool do_defrag(const char *mountpoint, bool verbose);
bool do_balance(const char *mountpoint, bool verbose);
bool do_trim(const char *mountpoint, bool verbose);

#endif
