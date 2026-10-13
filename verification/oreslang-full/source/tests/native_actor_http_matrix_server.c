/* 5-actor HTTP benchmark: shared native owner domains or isolated OS processes.
 * The Oreslang Counter.add method is LLVM-compiled; HTTP and scheduling are C.
 * PRIVATE here = per-actor OS process, NOT the unimplemented source isoactor.
 */
#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include "oreslang/runtime/ActorArena.h"
#include "oreslang/runtime/NativeActorScheduler.h"
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern void oreslang_actor_Counter_init(OresActorI64Heap*, uint64_t, uint64_t);
extern int64_t oreslang_actor_Counter_add(OresActorI64Heap*, uint64_t, uint64_t, int64_t);
/* Compiled from exactly the same .ores guest kernel fixture used by Graal. */
extern int64_t counter_response(int64_t);
#define ACTORS 5
#define CARRIERS 16
typedef struct { int64_t count, value; } Answer;
typedef struct {
    OresActorI64Heap* heap;
    OresActorArena* arena;
    OresArenaHandle counter;
    OresNativeActorRef actor_ref;
    int ipc;
    pid_t child;
} Worker;
static Worker workers[ACTORS];
static int listener = -1;
static _Atomic int shutdown_requested;
static OresNativeActorScheduler* scheduler;
static int process_mode;
static void require(int condition, const char* message) {
    if (!condition) { fprintf(stderr, "LLVM HTTP MATRIX FAILED %s errno=%d\n", message, errno); exit(4); }
}
static void close_socket(int fd) { if (fd >= 0) close(fd); }
static int full_io(int fd, void* data, size_t size, int writing) {
    size_t done = 0;
    while (done < size) {
        ssize_t n = writing ?
            send(fd, (char*)data + done, size - done, MSG_NOSIGNAL) :
            recv(fd, (char*)data + done, size - done, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        done += (size_t)n;
    }
    return 1;
}
static void init_worker(Worker* w, int idx) {
    uint64_t id = 3001u + (uint64_t) idx;
    w->heap = ores_actor_i64_heap_open(id, 1, 3);
    w->arena = ores_actor_arena_open(id, 1, 2, 16, 256);
    OresActorTurn begin = ores_actor_i64_heap_enter(w->heap, id, 1);
    oreslang_actor_Counter_init(w->heap, begin.domain, begin.epoch);
    ores_actor_i64_heap_leave(w->heap, begin);
    OresArenaTurn at = ores_actor_arena_enter(w->arena, id, 1);
    w->counter = ores_actor_arena_alloc(w->arena, at, sizeof(int64_t));
    int64_t zero = 0;
    ores_actor_arena_write(w->arena, at, w->counter, 0, &zero, sizeof zero);
    ores_actor_arena_leave(w->arena, at);
}
static Answer step(Worker* w, int idx) {
    uint64_t id = 3001u + (uint64_t) idx;
    OresActorTurn t = ores_actor_i64_heap_enter(w->heap, id, 1);
    OresArenaTurn a = ores_actor_arena_enter(w->arena, id, 1);
    OresArenaScratch temporary = ores_actor_arena_scratch_alloc(w->arena, a, sizeof(int64_t), 8);
    int64_t count = 0;
    ores_actor_arena_read(w->arena, a, w->counter, 0, &count, sizeof count);
    count++;
    ores_actor_arena_scratch_write(w->arena, a, temporary, 0, &count, sizeof count);
    ores_actor_arena_write(w->arena, a, w->counter, 0, &count, sizeof count);
    int64_t value = oreslang_actor_Counter_add(w->heap, t.domain, t.epoch, 1);
    require(value == counter_response(count), "shared Oreslang HTTP handler and actor state disagree");
    ores_actor_arena_leave(w->arena, a);
    ores_actor_i64_heap_leave(w->heap, t);
    Answer ans = {count, value};
    return ans;
}
static void retire_worker(Worker* w) {
    ores_actor_arena_retire(w->arena);
    ores_actor_i64_heap_retire(w->heap);
    require(ores_actor_arena_try_destroy(w->arena) == 1, "arena physical release");
    require(ores_actor_i64_heap_try_destroy(w->heap) == 1, "actor heap physical release");
}
static void child_worker(int fd, int idx) {
    close_socket(listener);
    Worker local = {0};
    init_worker(&local, idx);
    for (;;) {
        int32_t op = 0;
        if (!full_io(fd, &op, sizeof op, 0)) break;
        if (op == -1) break;
        require(op == idx, "cross-actor message routed");
        Answer ans = step(&local, idx);
        if (!full_io(fd, &ans, sizeof ans, 1)) break;
    }
    retire_worker(&local);
    close_socket(fd);
    _exit(0);
}
static long rss_pid_kb(pid_t pid) {
    char path[80];
    if (pid == 0) snprintf(path, sizeof path, "/proc/self/status");
    else snprintf(path, sizeof path, "/proc/%ld/status", (long)pid);
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    long kb = -1;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "VmRSS: %ld kB", &kb) == 1) break;
    fclose(f);
    return kb;
}
static long total_rss(void) {
    long total = rss_pid_kb(0);
    if (total < 0) return -1;
    if (process_mode) for (int a = 0; a < ACTORS; ++a) {
        long child = rss_pid_kb(workers[a].child);
        if (child < 0) return -1;
        total += child;
    }
    return total;
}
static void reply(int fd, int code, const char* body) {
    const char* reason = code == 200 ? "OK" : code == 404 ? "Not Found" :
                         code == 405 ? "Method Not Allowed" : "Bad Request";
    char header[256];
    int n = snprintf(header, sizeof header,
                     "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",
                     code, reason, strlen(body));
    require(n > 0 && (size_t)n < sizeof header, "response header size");
    (void)full_io(fd, header, (size_t)n, 1);
    (void)full_io(fd, (void*)body, strlen(body), 1);
}
/* Scheduler owns the accepted descriptor after successful submission.
 * This callback always closes the socket, including on failed sends. */
static void actor_task(void* unused, size_t idx, uint64_t payload) {
    (void)unused;
    const int fd = (int)payload;
    Worker* w = &workers[idx];
    Answer ans;
    if (!process_mode) ans = step(w, (int)idx);
    else {
        int32_t op = (int32_t)idx;
        require(full_io(w->ipc, &op, sizeof op, 1), "send to isolated actor");
        require(full_io(w->ipc, &ans, sizeof ans, 0), "read isolated actor response");
    }
    char body[100];
    snprintf(body, sizeof body, "%zu,%" PRId64 ",%" PRId64,
             idx, ans.count, ans.value);
    reply(fd, 200, body);
    close_socket(fd);
}
static void client_request(int fd) {
    char input[512];
    ssize_t len = recv(fd, input, sizeof input - 1, 0);
    if (len <= 0) { close_socket(fd); return; }
    input[len] = 0;
    if (!strncmp(input, "GET /health HTTP/1.", 19)) reply(fd, 200, "ok");
    else if (!strncmp(input, "GET /metrics HTTP/1.", 20)) {
        char body[64];
        snprintf(body, sizeof body, "%ld", total_rss());
        reply(fd, 200, body);
    } else if (!strncmp(input, "GET /shutdown HTTP/1.", 21)) {
        reply(fd, 200, "bye");
        atomic_store(&shutdown_requested, 1);
        shutdown(listener, SHUT_RDWR);
    } else if (!strncmp(input, "GET /work/", 10)) {
        int index = -1;
        char tail = 0;
        if (sscanf(input, "GET /work/%d%c", &index, &tail) == 2 &&
            tail == ' ' && index >= 0 && index < ACTORS) {
            /* Successful submit transfers ownership of fd to the worker.
             * The TCP ingress carrier must not wait for actor completion. */
            if (ores_native_actor_scheduler_send(scheduler, workers[index].actor_ref,
                                                  (uint64_t)(unsigned int)fd)) {
                return;
            }
            reply(fd, 503, "actor admission at capacity");
        } else reply(fd, 404, "missing");
    } else if (!strncmp(input, "GET ", 4)) reply(fd, 404, "missing");
    else if (strchr(input, ' ')) reply(fd, 405, "method");
    else reply(fd, 400, "bad request");
    close_socket(fd);
}
static void* carrier(void* unused) {
    (void) unused;
    while (!atomic_load(&shutdown_requested)) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (atomic_load(&shutdown_requested)) break;
            require(0, "accept");
        }
        client_request(fd);
    }
    return NULL;
}
int main(int argc, char** argv) {
    require(argc == 2 && (!strcmp(argv[1], "shared") ||
            !strcmp(argv[1], "private")), "usage shared|private");
    process_mode = !strcmp(argv[1], "private");
    signal(SIGPIPE, SIG_IGN);
    listener = socket(AF_INET, SOCK_STREAM, 0);
    require(listener >= 0, "socket");
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    require(bind(listener, (const struct sockaddr*)&addr, sizeof addr) == 0, "bind");
    require(listen(listener, 2048) == 0, "listen");
    socklen_t size = sizeof addr;
    require(getsockname(listener, (struct sockaddr*)&addr, &size) == 0, "getsockname");
    for (int i = 0; i < ACTORS; ++i) {
        if (!process_mode) { init_worker(&workers[i], i); continue; }
        int fd[2];
        require(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0, "socketpair");
        pid_t child = fork();
        require(child >= 0, "fork");
        if (child == 0) {
            close_socket(fd[0]);
            /* Child closes all inherited IPC parent sockets. */
            for (int j = 0; j < i; ++j) close_socket(workers[j].ipc);
            child_worker(fd[1], i);
        }
        close_socket(fd[1]);
        workers[i].child = child;
        workers[i].ipc = fd[0];
    }
    /* Spawn workers only after forking the isolated children: no fork with
     * an active multithreaded host or inherited scheduler mutex state. */
    scheduler = ores_native_actor_scheduler_open(ACTORS, 1024, CARRIERS,
                                                 actor_task, NULL);
    require(scheduler != NULL, "native ready-queue scheduler open");
    for (int i = 0; i < ACTORS; ++i) {
        workers[i].actor_ref = ores_native_actor_scheduler_spawn(scheduler, (size_t)i);
        require(workers[i].actor_ref.scheduler_domain != 0
                && ores_native_actor_scheduler_poll_ready(scheduler, workers[i].actor_ref),
                "actor ref initialized and ready");
    }
    printf("PORT=%u MODE=%s\n", (unsigned)ntohs(addr.sin_port), argv[1]);
    fflush(stdout);
    pthread_t threads[CARRIERS];
    for (int i = 0; i < CARRIERS; ++i)
        require(pthread_create(&threads[i], NULL, carrier, NULL) == 0, "carrier creation");
    for (int i = 0; i < CARRIERS; ++i)
        require(pthread_join(threads[i], NULL) == 0, "carrier joined");
    close_socket(listener);
    /* All TCP ingress threads are joined, so no new submitters can race
     * physical reclamation. Previously accepted sockets must finish before
     * we stop isolated children or retire their native heaps. */
    for (int i = 0; i < ACTORS; ++i)
        require(ores_native_actor_scheduler_stop(scheduler, workers[i].actor_ref),
                "actor ref retired");
    ores_native_actor_scheduler_retire(scheduler);
    require(ores_native_actor_scheduler_drain(scheduler) == 1, "all actor HTTP replies drained");
    require(ores_native_actor_scheduler_accepted(scheduler)
            == ores_native_actor_scheduler_completed(scheduler), "accepted requests completed");
    for (int i = 0; i < ACTORS; ++i)
        require(ores_native_actor_scheduler_poll_done(scheduler, workers[i].actor_ref),
                "native actor completion barrier");
    require(ores_native_actor_scheduler_try_destroy(scheduler) == 1,
            "worker pool retired and drained");
    for (int i = 0; i < ACTORS; ++i) {
        if (process_mode) {
            int32_t end = -1;
            require(full_io(workers[i].ipc, &end, sizeof end, 1), "isolated actor termination");
            close_socket(workers[i].ipc);
            int status = 0;
            require(waitpid(workers[i].child, &status, 0) == workers[i].child
                    && WIFEXITED(status) && WEXITSTATUS(status) == 0, "isolated child finalized");
        } else retire_worker(&workers[i]);
    }
    puts("NATIVE_MATRIX_SHUTDOWN_OK");
    return 0;
}
