#ifndef HTTPD_ENGINE_INTERNAL_POLLER_H
#define HTTPD_ENGINE_INTERNAL_POLLER_H

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { POLLER_READ = 1u, POLLER_WRITE = 2u, POLLER_ERROR = 4u };

typedef struct {
    void *ptr;
    unsigned events;
    bool present;
} poll_slot_t;

typedef struct {
    poll_slot_t *slots;
    size_t cap;
    size_t end;
} poll_table_t;

/** 등록 가능한 읽기·쓰기 비트인지 검사한다. */
static inline bool poll_mask_valid(unsigned events)
{
    return (events & ~(POLLER_READ | POLLER_WRITE)) == 0;
}

/** fd 위치를 확보한다. 커널에는 fd만 두므로 배열이 이동해도 안전하다. */
static inline int poll_reserve(poll_table_t *table, int fd)
{
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    if ((size_t)fd < table->cap) {
        return 0;
    }
    size_t cap = table->cap == 0 ? 64 : table->cap;
    while (cap <= (size_t)fd) {
        if (cap > SIZE_MAX / 2 / sizeof(*table->slots)) {
            errno = ENOMEM;
            return -1;
        }
        cap *= 2;
    }
    poll_slot_t *slots = realloc(table->slots, cap * sizeof(*slots));
    if (slots == NULL) {
        return -1;
    }
    memset(slots + table->cap, 0, (cap - table->cap) * sizeof(*slots));
    table->slots = slots;
    table->cap = cap;
    return 0;
}

/** 등록된 슬롯을 빌려준다. 배열을 늘리기 전까지만 사용한다. */
static inline poll_slot_t *poll_lookup(poll_table_t *table, int fd)
{
    if (fd < 0 || (size_t)fd >= table->end || !table->slots[fd].present) {
        return NULL;
    }
    return &table->slots[fd];
}

/** 확보한 슬롯을 새 등록 또는 수정된 이벤트로 채운다. */
static inline void poll_store(poll_table_t *table, int fd, unsigned events, void *ptr)
{
    table->slots[fd] = (poll_slot_t){ptr, events, true};
    if (table->end <= (size_t)fd) {
        table->end = (size_t)fd + 1;
    }
}

/** 등록을 지우고 유효 fd 범위를 줄인다. */
static inline void poll_erase(poll_table_t *table, int fd)
{
    table->slots[fd] = (poll_slot_t){0};
    while (table->end > 0 && !table->slots[table->end - 1].present) {
        table->end--;
    }
}

#endif
