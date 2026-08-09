#ifndef KRTSP_SOURCE_INTERNAL_H
#define KRTSP_SOURCE_INTERNAL_H

#include "kilix_rtsp.h"

/* Internal viewer hooks: exact ring sequence avoids racing a separate stats
 * snapshot, while the count/age accessors are lock-free scheduling hints. */
const uint8_t *krtsp_source_borrow_latest(
    krtsp_source *source, uint64_t *sequence, int *age_ms)
    __attribute__((visibility("hidden")));
uint64_t krtsp_source_frame_count(const krtsp_source *source)
    __attribute__((visibility("hidden")));
int krtsp_source_frame_age_ms(const krtsp_source *source)
    __attribute__((visibility("hidden")));

#endif
