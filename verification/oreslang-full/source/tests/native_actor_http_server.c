/* Real loopback HTTP/1.1 host for LLVM-compiled Counter actor methods.
 * Network transport is trusted C, not LLVM source-level http lowering.
 * One admitted actor turn per connection; no guest tracing collector. */
#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include "oreslang/runtime/ActorArena.h"
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
extern void oreslang_actor_Counter_init(OresActorI64Heap*, uint64_t, uint64_t);
extern int64_t oreslang_actor_Counter_read(OresActorI64Heap*, uint64_t, uint64_t);
extern int64_t oreslang_actor_Counter_add(OresActorI64Heap*, uint64_t, uint64_t, int64_t);
static void insist(int ok) { if (!ok) { perror("http-actor-server"); exit(3); } }
static void send_all(int fd, const char* text, size_t len) {
    while (len) {
        ssize_t n = send(fd, text, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        text += n; len -= (size_t) n;
    }
}
static void reply(int fd, int status, const char* text) {
    char header[256];
    const char* reason = status == 200 ? "OK" : status == 404 ? "Not Found" :
                         status == 405 ? "Method Not Allowed" : "Bad Request";
    int n = snprintf(header, sizeof header, "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",
                     status, reason, strlen(text));
    insist(n > 0 && (size_t) n < sizeof header);
    send_all(fd, header, (size_t) n);
    send_all(fd, text, strlen(text));
}
int main(void) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    insist(listener >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    insist(bind(listener, (const struct sockaddr*) &addr, sizeof addr) == 0);
    insist(listen(listener, 128) == 0);
    socklen_t size = sizeof addr;
    insist(getsockname(listener, (struct sockaddr*) &addr, &size) == 0);
    OresActorI64Heap* heap = ores_actor_i64_heap_open(2001, 1, 3);
    OresActorArena* arena = ores_actor_arena_open(2001, 1, 2, 16, 256);
    OresActorTurn initial = ores_actor_i64_heap_enter(heap, 2001, 1);
    oreslang_actor_Counter_init(heap, initial.domain, initial.epoch);
    ores_actor_i64_heap_leave(heap, initial);
    OresArenaTurn init_arena = ores_actor_arena_enter(arena, 2001, 1);
    OresArenaHandle completed = ores_actor_arena_alloc(arena, init_arena, sizeof(int64_t));
    int64_t requests = 0;
    ores_actor_arena_write(arena, init_arena, completed, 0, &requests, sizeof requests);
    ores_actor_arena_leave(arena, init_arena);
    printf("PORT=%u\n", (unsigned) ntohs(addr.sin_port));
    fflush(stdout);
    int done = 0;
    while (!done) {
        int client = accept(listener, NULL, NULL);
        if (client < 0 && errno == EINTR) continue;
        insist(client >= 0);
        struct timeval timeout = {3, 0};
        (void) setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        char input[512];
        ssize_t got = recv(client, input, sizeof input - 1, 0);
        if (got <= 0) { close(client); continue; }
        input[got] = 0;
        OresActorTurn turn = ores_actor_i64_heap_enter(heap, 2001, 1);
        OresArenaTurn frame = ores_actor_arena_enter(arena, 2001, 1);
        /* Actually allocate per-turn memory and drop it on every connection. */
        OresArenaScratch scratch = ores_actor_arena_scratch_alloc(arena, frame, 32, 8);
        ores_actor_arena_scratch_write(arena, frame, scratch, 0, input,
                                       (size_t) got < 32 ? (size_t) got : 32);
        char payload[80];
        if (strncmp(input, "GET /health HTTP/1.", 19) == 0) {
            snprintf(payload, sizeof payload, "%" PRId64, oreslang_actor_Counter_read(heap, turn.domain, turn.epoch));
            reply(client, 200, payload);
        } else if (strncmp(input, "GET /counter HTTP/1.", 20) == 0) {
            ++requests;
            ores_actor_arena_write(arena, frame, completed, 0, &requests, sizeof requests);
            int64_t value = oreslang_actor_Counter_add(heap, turn.domain, turn.epoch, 1);
            snprintf(payload, sizeof payload, "%" PRId64 ",%" PRId64, requests, value);
            reply(client, 200, payload);
        } else if (strncmp(input, "GET /shutdown HTTP/1.", 21) == 0) {
            reply(client, 200, "bye");
            done = 1;
        } else if (strncmp(input, "GET ", 4) == 0) {
            reply(client, 404, "not found");
        } else if (strchr(input, ' ') != NULL) {
            reply(client, 405, "method not allowed");
        } else {
            reply(client, 400, "bad request");
        }
        ores_actor_arena_leave(arena, frame);
        ores_actor_i64_heap_leave(heap, turn);
        close(client);
    }
    close(listener);
    ores_actor_arena_retire(arena);
    ores_actor_i64_heap_retire(heap);
    insist(ores_actor_arena_try_destroy(arena) == 1);
    insist(ores_actor_i64_heap_try_destroy(heap) == 1);
    return 0;
}
