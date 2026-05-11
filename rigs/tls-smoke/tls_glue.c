/* tls_glue — provide `mp_stream_posix_read` / `mp_stream_posix_write`
 * symbols so axtls' SOCKET_READ/WRITE macros (defined in
 * extmod/axtls-include/axtls_os_port.h to call these names) plug into
 * our raw-lwIP socket without dragging in MicroPython's mp_stream
 * machinery. The smoke main passes `(long)&g_sock` as the "fd"
 * argument to ssl_client_new, so axtls hands us the same pointer at
 * each callback.
 *
 * Return shape mirrors mp_stream_posix_read: positive byte count on
 * progress, 0 on EOF, negative on error. axtls treats negative as
 * SSL_ERROR_CONN_LOST and -EAGAIN as "try later" — we use -EAGAIN
 * (== -11 on glibc, -35 on macOS, -10035 in winsock; pick the value
 * axtls expects). axtls just checks `r > 0` for success and `r == 0`
 * for EOF; anything < 0 is treated as a soft retry on the first
 * iteration of the handshake state machine. We return -1 for
 * "would block" and 0 for "closed".
 */

#include <string.h>

/* Smoke socket type — definition kept in main.c, only the fwd-decl
 * needed here. Field layout doesn't matter; we only use the
 * smoke_sock_read/write entry points. */
struct smoke_sock_t;
extern int smoke_sock_read(struct smoke_sock_t *s, unsigned char *buf, unsigned int max);
extern int smoke_sock_write(struct smoke_sock_t *s, const unsigned char *buf, unsigned int len);

extern int write(int fd, const void *buf, unsigned int n);
static void _glue_hex(const char *tag, unsigned int v) {
    static const char hex[] = "0123456789abcdef";
    char b[16];
    int n = 0;
    while (tag[n]) { b[n] = tag[n]; n++; }
    for (int i = 0; i < 4; i++) b[n + i] = hex[(v >> ((3 - i) * 4)) & 0xF];
    b[n + 4] = ' ';
    write(1, b, n + 5);
}

/* axtls calls SOCKET_READ((self, buf, size)) — we receive `void *s`
 * which is the (long)g_sock pointer we passed to ssl_client_new. */
int mp_stream_posix_read(void *s, void *buf, unsigned int size) {
    _glue_hex("[sr:", size);
    int r = smoke_sock_read((struct smoke_sock_t *)s,
                            (unsigned char *)buf, size);
    _glue_hex("=", (unsigned int)r);
    return r;
}

int mp_stream_posix_write(void *s, const void *buf, unsigned int size) {
    _glue_hex("[sw:", size);
    int r = smoke_sock_write((struct smoke_sock_t *)s,
                             (const unsigned char *)buf, size);
    _glue_hex("=", (unsigned int)r);
    return r;
}

/* libc's `errno` already covers this — the -Dmp_stream_errno=errno
 * compile flag rewrites all axtls references onto it. */

/* time()/mktime()/gettimeofday() come from
 * port/time_real_uc386dos.c — uses the DOS RTC for real epoch
 * values. axtls' cert verification path needs them. */

/* Self-contained patched crypto-algorithms sha256 — axtls'
 * os_port.h redirects SHA256_* to these lowercase names. Pulled in
 * here as a TU so uc386 sees it as part of an enclosing
 * compilation; standalone it trips on `BYTE data[]` array params. */
#include "patches/sha256.c"
