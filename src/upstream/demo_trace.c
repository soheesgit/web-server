/* 선택을 관측하는 옵트인 루프백 텔레메트리. 송신은 실패해도 요청에 영향을 주지 않는다. */
#include "upstream/internal_demo_trace.h"

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "core/platform.h"

#define DEMO_SESSION_MAX 64
#define DEMO_CHOICES_MAX 2

typedef struct {
    const backend_t *backend;
    const char *reason;
    size_t active, rank;
} candidate_t;

typedef struct {
    const char *policy;
    const backend_t *preferred;
    size_t limit, count;
    uint64_t duration;
    candidate_t candidates[CFG_MAX_BACKENDS];
} choice_t;

static pthread_once_t demo_once = PTHREAD_ONCE_INIT;
static int demo_fd = -1;
static _Atomic uint64_t state_seq;
static _Thread_local struct {
    const proxy_job_t *job;
    size_t count;
    choice_t choices[DEMO_CHOICES_MAX];
} current;

static void open_demo_socket(void)
{
    const char *value = getenv("HTTPD_DEMO_TRACE_PORT");
    if (value == NULL || *value == '\0') {
        return;
    }
    char *end;
    unsigned long port = strtoul(value, &end, 10);
    if (*end != '\0' || port == 0 || port > 65535) {
        return;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)},
    };
    if (platform_set_nonblock(fd) != 0 || platform_set_cloexec(fd) != 0 ||
        connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return;
    }
    demo_fd = fd;
}

static bool enabled(void)
{
    int saved = errno;
    (void)pthread_once(&demo_once, open_demo_socket);
    errno = saved;
    return demo_fd >= 0;
}

static bool has_session(const proxy_job_t *job)
{
    size_t len = job->affinity_key.len;
    if (len == 0 || len > DEMO_SESSION_MAX || strcmp(job->group->name, "llm") != 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)job->affinity_key.ptr[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

static void send_event(buf_t *event)
{
    if (event->len != 0) {
        (void)send(demo_fd, event->data, event->len, 0);
    }
    buf_cleanup(event);
}

void demo_trace_begin(const proxy_job_t *job)
{
    current.job = enabled() && has_session(job) ? job : NULL;
    current.count = 0;
}

uint64_t demo_trace_clock_us(void)
{
    struct timespec time;
    if (current.job == NULL || clock_gettime(CLOCK_MONOTONIC, &time) != 0) {
        return 0;
    }
    return (uint64_t)time.tv_sec * UINT64_C(1000000) + (uint64_t)time.tv_nsec / 1000;
}

void demo_trace_choice(const char *policy, size_t limit, uint64_t started,
                       const backend_t *preferred)
{
    if (current.job == NULL || current.count == DEMO_CHOICES_MAX) {
        return;
    }
    uint64_t end = demo_trace_clock_us();
    current.choices[current.count++] = (choice_t){
        .policy = policy,
        .preferred = preferred,
        .limit = limit,
        .duration = started != 0 && end >= started ? end - started : 0,
    };
}

void demo_trace_candidate(const backend_t *backend, const char *reason, size_t rank)
{
    if (current.job == NULL || current.count == 0) {
        return;
    }
    choice_t *choice = &current.choices[current.count - 1];
    if (choice->count < CFG_MAX_BACKENDS) {
        choice->candidates[choice->count++] = (candidate_t){
            .backend = backend,
            .reason = reason,
            .active = backend->active,
            .rank = rank,
        };
    }
}

static const char *endpoint(const backend_t *backend)
{
    return backend == NULL ? "-" : backend->hostport;
}

static int begin_event(buf_t *event, const char *type, const proxy_job_t *job)
{
    if (buf_init(event, 0, 16u << 10) != 0) {
        return -1;
    }
    return buf_appendf(event, "%s\t%.*s\t%d", type, (int)job->affinity_key.len,
                       job->affinity_key.ptr, job->attempts);
}

void demo_trace_attempt(const backend_t *selected, int error)
{
    const proxy_job_t *job = current.job;
    current.job = NULL;
    if (job == NULL) {
        return;
    }

    int saved = errno;
    buf_t event = {0};
    int rc = begin_event(&event, "attempt", job);
    if (rc == 0) {
        rc = buf_appendf(&event, "\t%s\t%d", endpoint(selected), error);
    }
    for (size_t i = 0; rc == 0 && i < current.count; i++) {
        const choice_t *choice = &current.choices[i];
        rc = buf_appendf(&event, "\nchoice\t%s\t%zu\t%" PRIu64 "\t%s", choice->policy,
                         choice->limit, choice->duration, endpoint(choice->preferred));
        for (size_t j = 0; rc == 0 && j < choice->count; j++) {
            const candidate_t *c = &choice->candidates[j];
            rc = buf_appendf(&event, "\ncandidate\t%s\t%s\t%zu\t%zu", endpoint(c->backend),
                             c->reason, c->active, c->rank);
        }
    }
    if (rc == 0) {
        send_event(&event);
    } else {
        buf_cleanup(&event);
    }
    errno = saved;
}

void demo_trace_failure(const proxy_job_t *job, proxy_fail_t reason)
{
    if (!enabled() || !has_session(job)) {
        return;
    }

    int saved = errno;
    const char *name;
    switch (reason) {
    case PFAIL_CONNECT:
        name = "connect";
        break;
    case PFAIL_SEND:
        name = "send";
        break;
    case PFAIL_TIMEOUT:
        name = "timeout";
        break;
    default:
        name = "response";
        break;
    }
    buf_t event = {0};
    if (begin_event(&event, "failure", job) == 0 &&
        buf_appendf(&event, "\t%s\t%s", endpoint(job->backend), name) == 0) {
        send_event(&event);
    } else {
        buf_cleanup(&event);
    }
    errno = saved;
}

void demo_trace_state(upstream_group_t *group)
{
    if (!enabled() || strcmp(group->name, "llm") != 0) {
        return;
    }

    int saved = errno;
    struct {
        const backend_t *backend;
        size_t active;
        up_health_t health;
    } snapshots[CFG_MAX_BACKENDS];
    pthread_mutex_lock(&group->lock);
    uint64_t seq = atomic_fetch_add_explicit(&state_seq, 1, memory_order_relaxed);
    size_t count = group->count;
    for (size_t i = 0; i < count; i++) {
        snapshots[i].backend = &group->backends[i];
        snapshots[i].active = group->backends[i].active;
        snapshots[i].health = group->backends[i].health;
    }
    pthread_mutex_unlock(&group->lock);

    buf_t event = {0};
    int rc = buf_init(&event, 0, 16u << 10);
    if (rc == 0) {
        rc = buf_appendf(&event, "state\t%" PRIu64, seq);
    }
    for (size_t i = 0; rc == 0 && i < count; i++) {
        static const char *const health[] = {"healthy", "cooldown", "recovering"};
        rc = buf_appendf(&event, "\n%s\t%zu\t%s", endpoint(snapshots[i].backend),
                         snapshots[i].active, health[snapshots[i].health]);
    }
    if (rc == 0) {
        send_event(&event);
    } else {
        buf_cleanup(&event);
    }
    errno = saved;
}
