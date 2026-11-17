/*
 * Zero-copy P0, the launcher side (debug builds only, see ZeroCopyP0Activity).
 *
 * Allocates AHardwareBuffers, hands their dma-buf fds to a guest client over a
 * unix socket (zc_proto.h), and shows what the guest renders into them through
 * ASurfaceControl: no ANativeWindow_lock, no CPU copy. Mode "cpu" draws the same
 * test pattern on the CPU instead, to check the display half without the guest.
 *
 * When the buffers were allocated with CPU read usage, some presented frames are
 * read back and compared with the pattern (zc_pattern below, the guest's shader
 * draws the same), so a wrong stride, offset, format or tiling shows up in
 * logcat as mismatches, not only on screen.
 */
#include <android/data_space.h>
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <android/surface_control.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "zc_proto.h"

#define TAG "zc-p0"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define MAX_HANDLE_FDS 8
#define MAX_HANDLE_INTS 256

enum mode { MODE_CPU, MODE_GUEST };

struct buf {
    AHardwareBuffer *ahb;
    AHardwareBuffer_Desc desc;
    int fds[MAX_HANDLE_FDS];
    uint64_t sizes[MAX_HANDLE_FDS];
    int nfds;
    int pixel; /* index into fds of the pixel dma-buf */
    int busy;  /* with the compositor */
    uint32_t frame;
};

struct p0 {
    ANativeWindow *window;
    ASurfaceControl *sc;
    int width, height, nbuf;
    enum mode mode;
    uint64_t usage;
    int frames;
    int verify_every;
    char sock_path[108];
    int listen_fd, client_fd;
    struct buf bufs[ZC_MAX_BUFS];
    pthread_t thread;
    int thread_started;
    atomic_int stop;

    pthread_mutex_t lock; /* bufs[].busy, shown, inflight, client_fd writes */
    pthread_cond_t cond;
    int shown;    /* buffer on screen, -1 before the first frame */
    int inflight; /* applied transactions whose callback hasn't run */
    int configured;

    /* stats, per client (or the cpu run) */
    uint32_t presented;
    int64_t t_first, t_last;
    int64_t latch_sum, latch_n; /* apply -> latch */
    uint32_t verify_ok, verify_bad;
};

struct txn {
    struct p0 *p;
    int prev;
    int64_t t_apply;
};

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* ---- the test pattern (keep in sync with the guest's zc.frag) ---- */

static uint8_t unorm(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    return (uint8_t)(v * 255.0f + 0.5f);
}

/* RGBA8 as a little-endian uint32 (R in the low byte), pixel (x, y) from the top left. */
static uint32_t zc_pattern(int x, int y, uint32_t frame, int w, int h) {
    float px = (float)x + 0.5f, py = (float)y + 0.5f;
    float r = px / (float)w, g = py / (float)h, b = 0.5f + 0.5f * sinf((float)frame / 60.0f);
    if (fmodf(px, 64.0f) < 2.0f || fmodf(py, 64.0f) < 2.0f) r = g = b = 1.0f;
    float bx = fmodf((float)frame * 8.0f, (float)w);
    if (px >= bx && px < bx + 32.0f) r = 1.0f, g = 0.0f, b = 1.0f;
    const float s = 96.0f;
    int left = px < s, right = px >= (float)w - s, top = py < s, bottom = py >= (float)h - s;
    if (left && top) r = 1, g = 0, b = 0;
    if (right && top) r = 0, g = 1, b = 0;
    if (left && bottom) r = 0, g = 0, b = 1;
    if (right && bottom) r = 1, g = 1, b = 1;
    return (uint32_t)unorm(r) | (uint32_t)unorm(g) << 8 | (uint32_t)unorm(b) << 16 | 0xffu << 24;
}

static int close_enough(uint32_t a, uint32_t b) {
    for (int i = 0; i < 32; i += 8) {
        int d = (int)((a >> i) & 0xff) - (int)((b >> i) & 0xff);
        if (d < -3 || d > 3) return 0;
    }
    return 1;
}

/* Reads [buf] back on the CPU and compares sample points with the pattern for [frame]. */
static void verify(struct p0 *p, struct buf *b, uint32_t frame) {
    if (!(b->desc.usage & AHARDWAREBUFFER_USAGE_CPU_READ_MASK)) return;
    void *addr = NULL;
    int64_t t0 = now_ns();
    int r = AHardwareBuffer_lock(b->ahb, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, NULL, &addr);
    if (r || !addr) {
        LOGW("verify: lock failed (%d)", r);
        return;
    }
    const int w = (int)b->desc.width, h = (int)b->desc.height;
    const int pts[][2] = {
        {48, 48}, {w - 49, 48}, {48, h - 49}, {w - 49, h - 49}, {w / 2, h / 2},
        {w / 3, h / 3}, {2 * w / 3, h / 4}, {w / 5, 4 * h / 5}, {w - 101, h - 101}, {130, 7},
    };
    const int n = (int)(sizeof(pts) / sizeof(pts[0]));
    int ok = 0;
    char bad[512] = "";
    for (int i = 0; i < n; i++) {
        int x = pts[i][0], y = pts[i][1];
        uint32_t got = ((const uint32_t *)addr)[(size_t)y * b->desc.stride + (size_t)x];
        uint32_t want = zc_pattern(x, y, frame, w, h);
        if (close_enough(got, want)) {
            ok++;
        } else {
            size_t len = strlen(bad);
            snprintf(bad + len, sizeof(bad) - len, " (%d,%d)=%08x want %08x", x, y, got, want);
        }
    }
    AHardwareBuffer_unlock(b->ahb, NULL);
    if (ok == n) p->verify_ok++; else p->verify_bad++;
    LOGI("verify frame=%u buf=%d: %d/%d match%s%s (%.1f ms)", frame, (int)(b - p->bufs), ok, n,
         ok == n ? "" : ":", bad, (double)(now_ns() - t0) / 1e6);
}

/* ---- buffers ---- */

static uint64_t parse_usage(const char *s, enum mode mode) {
    const uint64_t display = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
    if (!s || !*s) s = mode == MODE_CPU ? "cpu" : "linear";
    /* CPU read keeps gralloc from picking a compressed layout (UBWC, AFBC) and lets verify() read back. */
    if (!strcmp(s, "linear")) return display | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_CPU_READ_RARELY;
    /* What a real swapchain would ask for: gralloc may compress it, the guest's linear import would then be wrong. */
    if (!strcmp(s, "gpu")) return display | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
    if (!strcmp(s, "cpu")) return display | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_CPU_READ_RARELY;
    return strtoull(s, NULL, 0);
}

/* What /proc/self/fd says the fd is ("/dmabuf:..." or "anon_inode:dmabuf"). */
static void fd_kind(int fd, char *out, size_t n) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    ssize_t len = readlink(path, out, n - 1);
    out[len > 0 ? len : 0] = 0;
}

/*
 * The NDK has no public call for a buffer's native handle, but it can send one
 * over a socket for another process's AHardwareBuffer_recvHandleFromUnixSocket.
 * Sending it to ourselves gives dups of its fds and the flattened GraphicBuffer
 * (13 ints of header, then the handle's ints), from public API only.
 */
static int extract_handle(struct buf *b, int idx) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return -errno;
    int r = AHardwareBuffer_sendHandleToUnixSocket(b->ahb, sv[0]);
    if (r) {
        close(sv[0]);
        close(sv[1]);
        return r;
    }
    int32_t data[13 + MAX_HANDLE_INTS];
    char cbuf[CMSG_SPACE(sizeof(int) * MAX_HANDLE_FDS)];
    struct iovec iov = {.iov_base = data, .iov_len = sizeof(data)};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof(cbuf)};
    ssize_t n = recvmsg(sv[1], &msg, MSG_CMSG_CLOEXEC);
    int err = errno;
    close(sv[0]);
    close(sv[1]);
    if (n < 0) return -err;
    b->nfds = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        int count = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        for (int i = 0; i < count && b->nfds < MAX_HANDLE_FDS; i++) memcpy(&b->fds[b->nfds++], CMSG_DATA(c) + i * sizeof(int), sizeof(int));
    }
    int ints = n >= 13 * 4 ? data[11] : 0;
    LOGI("buf %d: GraphicBuffer magic=%08x %dx%d stride=%d format=%d layers=%d usage=%08x:%08x fds=%d ints=%d (%zd bytes, %d fds received)",
         idx, data[0], data[1], data[2], data[3], data[4], data[5], data[12], data[6], data[10], ints, n, b->nfds);
    if (ints > 0 && 13 + ints <= (int)(n / 4)) {
        char line[1024];
        size_t len = 0;
        for (int i = 0; i < ints && len < sizeof(line) - 12; i++) len += (size_t)snprintf(line + len, sizeof(line) - len, " %08x", data[13 + i]);
        LOGI("buf %d: handle ints:%s", idx, line);
    }
    b->pixel = -1;
    for (int i = 0; i < b->nfds; i++) {
        off_t size = lseek(b->fds[i], 0, SEEK_END);
        lseek(b->fds[i], 0, SEEK_SET);
        b->sizes[i] = size > 0 ? (uint64_t)size : 0;
        char kind[128];
        fd_kind(b->fds[i], kind, sizeof(kind));
        LOGI("buf %d: fd[%d] size=%llu %s", idx, i, (unsigned long long)b->sizes[i], kind);
        if (b->pixel < 0 || b->sizes[i] > b->sizes[b->pixel]) b->pixel = i;
    }
    return b->pixel >= 0 ? 0 : -ENOENT;
}

static int alloc_buffers(struct p0 *p, int fd_index) {
    for (int i = 0; i < p->nbuf; i++) {
        struct buf *b = &p->bufs[i];
        AHardwareBuffer_Desc want = {
            .width = (uint32_t)p->width,
            .height = (uint32_t)p->height,
            .layers = 1,
            .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
            .usage = p->usage,
        };
        if (i == 0) LOGI("usage %#llx supported: %d", (unsigned long long)p->usage, AHardwareBuffer_isSupported(&want));
        int r = AHardwareBuffer_allocate(&want, &b->ahb);
        if (r || !b->ahb) {
            LOGE("buf %d: AHardwareBuffer_allocate %dx%d usage %#llx failed: %d", i, p->width, p->height,
                 (unsigned long long)p->usage, r);
            return -1;
        }
        AHardwareBuffer_describe(b->ahb, &b->desc);
        LOGI("buf %d: %ux%u stride=%u px format=%u usage=%#llx", i, b->desc.width, b->desc.height, b->desc.stride,
             b->desc.format, (unsigned long long)b->desc.usage);
        r = extract_handle(b, i);
        if (r) {
            LOGE("buf %d: no fds from the native handle: %d", i, r);
            return -1;
        }
        if (fd_index >= 0 && fd_index < b->nfds) b->pixel = fd_index;
        uint64_t need = (uint64_t)b->desc.stride * b->desc.height * 4;
        LOGI("buf %d: pixel fd[%d], %llu bytes, %llu needed for stride x height%s", i, b->pixel,
             (unsigned long long)b->sizes[b->pixel], (unsigned long long)need,
             b->sizes[b->pixel] < need ? " (TOO SMALL: compressed, or not the pixel fd)" : "");
    }
    return 0;
}

/* ---- presenting ---- */

static int send_packet(struct p0 *p, const void *data, size_t size, int fd) {
    if (p->client_fd < 0) return -1;
    struct iovec iov = {.iov_base = (void *)data, .iov_len = size};
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
    if (fd >= 0) {
        memset(cbuf, 0, sizeof(cbuf));
        msg.msg_control = cbuf;
        msg.msg_controllen = sizeof(cbuf);
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    ssize_t n;
    do n = sendmsg(p->client_fd, &msg, MSG_NOSIGNAL);
    while (n < 0 && errno == EINTR);
    return n == (ssize_t)size ? 0 : -1;
}

/* The compositor is done with buffer [idx]: it can be drawn again. Caller holds the lock. */
static void release_locked(struct p0 *p, int idx) {
    struct buf *b = &p->bufs[idx];
    if (!b->busy) return;
    b->busy = 0;
    pthread_cond_broadcast(&p->cond);
    if (p->mode == MODE_GUEST && p->client_fd >= 0) {
        struct zc_release m = {{ZC_MSG_RELEASE, sizeof(m)}, (uint32_t)idx, b->frame};
        send_packet(p, &m, sizeof(m), -1);
    }
}

static void wait_fence(int fd, int timeout_ms) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int r;
    do r = poll(&pfd, 1, timeout_ms);
    while (r < 0 && errno == EINTR);
    if (r == 0) LOGW("release fence did not signal in %d ms", timeout_ms);
}

static void on_complete(void *ctx, ASurfaceTransactionStats *stats) {
    struct txn *t = ctx;
    struct p0 *p = t->p;
    int fence = t->prev >= 0 ? ASurfaceTransactionStats_getPreviousReleaseFenceFd(stats, p->sc) : -1;
    if (fence >= 0) {
        wait_fence(fence, 1000);
        close(fence);
    }
    int64_t latch = ASurfaceTransactionStats_getLatchTime(stats);
    pthread_mutex_lock(&p->lock);
    if (latch > t->t_apply) {
        p->latch_sum += latch - t->t_apply;
        p->latch_n++;
    }
    if (t->prev >= 0) release_locked(p, t->prev);
    p->inflight--;
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->lock);
    free(t);
}

/* Shows buffer [idx]; the one shown before goes back once the compositor releases it. */
static void present(struct p0 *p, int idx, uint32_t frame, int acquire_fence) {
    struct buf *b = &p->bufs[idx];
    struct txn *t = calloc(1, sizeof(*t));
    t->p = p;
    pthread_mutex_lock(&p->lock);
    t->prev = p->shown != idx ? p->shown : -1;
    p->shown = idx;
    b->busy = 1;
    b->frame = frame;
    p->inflight++;
    pthread_mutex_unlock(&p->lock);

    ASurfaceTransaction *tx = ASurfaceTransaction_create();
    if (!p->configured) {
        ARect r = {0, 0, p->width, p->height};
        ASurfaceTransaction_setGeometry(tx, p->sc, &r, &r, 0);
        ASurfaceTransaction_setVisibility(tx, p->sc, ASURFACE_TRANSACTION_VISIBILITY_SHOW);
        ASurfaceTransaction_setZOrder(tx, p->sc, 1);
        ASurfaceTransaction_setBufferTransparency(tx, p->sc, ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE);
        ASurfaceTransaction_setBufferDataSpace(tx, p->sc, ADATASPACE_SRGB);
        p->configured = 1;
    }
    ASurfaceTransaction_setBuffer(tx, p->sc, b->ahb, acquire_fence);
    ASurfaceTransaction_setOnComplete(tx, t, on_complete);
    t->t_apply = now_ns();
    ASurfaceTransaction_apply(tx);
    ASurfaceTransaction_delete(tx);

    if (!p->presented) p->t_first = t->t_apply;
    p->t_last = t->t_apply;
    p->presented++;
}

static void report(struct p0 *p, const char *what) {
    double secs = (double)(p->t_last - p->t_first) / 1e9;
    LOGI("%s: %u frames in %.1f s (%.1f fps), apply->latch %.2f ms avg, verify %u ok / %u bad", what, p->presented, secs,
         secs > 0 ? (p->presented - 1) / secs : 0.0, p->latch_n ? (double)p->latch_sum / p->latch_n / 1e6 : 0.0,
         p->verify_ok, p->verify_bad);
}

static void reset_stats(struct p0 *p) {
    p->presented = 0;
    p->latch_sum = p->latch_n = 0;
    p->verify_ok = p->verify_bad = 0;
}

static int should_verify(struct p0 *p, uint32_t frame) {
    return frame < 3 || (p->verify_every > 0 && frame % (uint32_t)p->verify_every == 0);
}

/* ---- mode cpu: the pattern drawn on the CPU, to check gralloc and the display path alone ---- */

static void *cpu_thread(void *arg) {
    struct p0 *p = arg;
    for (uint32_t frame = 0; !atomic_load(&p->stop) && (p->frames <= 0 || frame < (uint32_t)p->frames); frame++) {
        int idx = -1;
        pthread_mutex_lock(&p->lock);
        while (!atomic_load(&p->stop)) {
            for (int i = 0; i < p->nbuf && idx < 0; i++)
                if (!p->bufs[i].busy) idx = i;
            if (idx >= 0) break;
            pthread_cond_wait(&p->cond, &p->lock);
        }
        pthread_mutex_unlock(&p->lock);
        if (idx < 0) break;
        struct buf *b = &p->bufs[idx];
        void *addr = NULL;
        int r = AHardwareBuffer_lock(b->ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &addr);
        if (r || !addr) {
            LOGE("cpu: lock failed (%d)", r);
            break;
        }
        for (int y = 0; y < (int)b->desc.height; y++) {
            uint32_t *row = (uint32_t *)addr + (size_t)y * b->desc.stride;
            for (int x = 0; x < (int)b->desc.width; x++) row[x] = zc_pattern(x, y, frame, (int)b->desc.width, (int)b->desc.height);
        }
        int fence = -1;
        AHardwareBuffer_unlock(b->ahb, &fence);
        if (should_verify(p, frame)) verify(p, b, frame);
        present(p, idx, frame, fence);
        if (frame % 120 == 119) report(p, "cpu");
    }
    report(p, "cpu done");
    return NULL;
}

/* ---- mode guest: the buffers go to a client in the guest, which renders and presents them ---- */

/* Caller holds the lock, so no RELEASE from a callback goes out before HELLO. */
static int send_buffers_locked(struct p0 *p) {
    uint32_t busy = 0;
    for (int i = 0; i < p->nbuf; i++)
        if (p->bufs[i].busy) busy |= 1u << i;
    struct zc_hello hello = {{ZC_MSG_HELLO, sizeof(hello)}, ZC_PROTO_VERSION, (uint32_t)p->nbuf, (uint32_t)p->width,
                             (uint32_t)p->height, busy};
    if (send_packet(p, &hello, sizeof(hello), -1)) return -1;
    for (int i = 0; i < p->nbuf; i++) {
        struct buf *b = &p->bufs[i];
        struct zc_buffer m = {
            .h = {ZC_MSG_BUFFER, sizeof(m)},
            .index = (uint32_t)i,
            .width = b->desc.width,
            .height = b->desc.height,
            .ahb_format = b->desc.format,
            .drm_format = ZC_DRM_FORMAT_ABGR8888,
            .stride_px = b->desc.stride,
            .stride_bytes = b->desc.stride * 4,
            .offset = 0,
            .dmabuf_size = b->sizes[b->pixel],
            .modifier = ZC_DRM_FORMAT_MOD_LINEAR,
            .usage = b->desc.usage,
            .handle_fds = (uint32_t)b->nfds,
            .fd_index = (uint32_t)b->pixel,
        };
        if (send_packet(p, &m, sizeof(m), b->fds[b->pixel])) return -1;
    }
    return 0;
}

static void serve_client(struct p0 *p) {
    reset_stats(p);
    pthread_mutex_lock(&p->lock);
    int sent = send_buffers_locked(p);
    pthread_mutex_unlock(&p->lock);
    if (sent) {
        LOGE("guest: sending the buffers failed: %s", strerror(errno));
        return;
    }
    LOGI("guest: sent %d buffers", p->nbuf);
    int64_t last_report = now_ns();
    while (!atomic_load(&p->stop)) {
        struct pollfd pfd = {.fd = p->client_fd, .events = POLLIN};
        int r = poll(&pfd, 1, 200);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) break;
        if (r == 0) continue;
        union {
            struct zc_hdr h;
            struct zc_ready ready;
            struct zc_present present;
            struct zc_bye bye;
            char raw[256];
        } m;
        ssize_t n = recv(p->client_fd, &m, sizeof(m), 0);
        if (n <= 0) {
            LOGI("guest: disconnected (%s)", n == 0 ? "eof" : strerror(errno));
            break;
        }
        if (n < (ssize_t)sizeof(struct zc_hdr) || m.h.size != (uint32_t)n) {
            LOGW("guest: bad packet (%zd bytes, type %u)", n, m.h.type);
            continue;
        }
        switch (m.h.type) {
            case ZC_MSG_READY:
                m.ready.device[sizeof(m.ready.device) - 1] = 0;
                LOGI("guest: ready ok=%u path=%s device=%s", m.ready.ok,
                     m.ready.path == ZC_PATH_IMAGE ? "image" : m.ready.path == ZC_PATH_BUFFER ? "buffer" : "none",
                     m.ready.device);
                break;
            case ZC_MSG_PRESENT: {
                uint32_t idx = m.present.index;
                if (idx >= (uint32_t)p->nbuf) {
                    LOGW("guest: present of buffer %u", idx);
                    break;
                }
                if (should_verify(p, m.present.frame)) verify(p, &p->bufs[idx], m.present.frame);
                present(p, (int)idx, m.present.frame, -1);
                if (now_ns() - last_report > 2000000000LL) {
                    report(p, "guest");
                    last_report = now_ns();
                }
                break;
            }
            case ZC_MSG_BYE:
                LOGI("guest: bye (%u)", m.bye.reason);
                goto out;
            default:
                LOGW("guest: unknown message %u", m.h.type);
        }
    }
out:
    report(p, "guest done");
}

static int listen_on(struct p0 *p);

static void *guest_thread(void *arg) {
    struct p0 *p = arg;
    while (!atomic_load(&p->stop)) {
        /* Linux starting (Proot.prepare) empties the runtime dir, our socket with it: listen again. */
        if (access(p->sock_path, F_OK)) {
            LOGI("socket gone, listening again");
            if (p->listen_fd >= 0) close(p->listen_fd);
            p->listen_fd = -1;
            if (listen_on(p)) {
                usleep(500000);
                continue;
            }
        }
        struct pollfd pfd = {.fd = p->listen_fd, .events = POLLIN};
        int r = poll(&pfd, 1, 200);
        if (r <= 0) continue;
        int fd = accept4(p->listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0) continue;
        LOGI("guest: connected");
        pthread_mutex_lock(&p->lock);
        p->client_fd = fd;
        pthread_mutex_unlock(&p->lock);
        serve_client(p);
        pthread_mutex_lock(&p->lock);
        p->client_fd = -1;
        pthread_mutex_unlock(&p->lock);
        close(fd);
    }
    return NULL;
}

static int listen_on(struct p0 *p) {
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(p->sock_path) >= sizeof(addr.sun_path)) {
        LOGE("socket path too long: %s", p->sock_path);
        return -1;
    }
    strcpy(addr.sun_path, p->sock_path);
    unlink(p->sock_path);
    p->listen_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (p->listen_fd < 0 || bind(p->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(p->listen_fd, 1)) {
        LOGE("listen on %s: %s", p->sock_path, strerror(errno));
        return -1;
    }
    LOGI("listening on %s", p->sock_path);
    return 0;
}

/* ---- JNI ---- */

static void destroy(struct p0 *p) {
    atomic_store(&p->stop, 1);
    pthread_mutex_lock(&p->lock);
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->lock);
    if (p->thread_started) pthread_join(p->thread, NULL);
    if (p->listen_fd >= 0) {
        close(p->listen_fd);
        unlink(p->sock_path);
    }
    int leak = 0;
    if (p->sc) {
        ASurfaceTransaction *tx = ASurfaceTransaction_create();
        ASurfaceTransaction_reparent(tx, p->sc, NULL);
        ASurfaceTransaction_apply(tx);
        ASurfaceTransaction_delete(tx);
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += 1;
        pthread_mutex_lock(&p->lock);
        while (p->inflight > 0)
            if (pthread_cond_timedwait(&p->cond, &p->lock, &until)) break;
        leak = p->inflight > 0;
        pthread_mutex_unlock(&p->lock);
        ASurfaceControl_release(p->sc);
    }
    if (leak) {
        /* A callback may still come and touch p: keep everything, it's a debug tool. */
        LOGW("destroy: %d transactions still in flight, leaking the buffers", p->inflight);
        return;
    }
    for (int i = 0; i < p->nbuf; i++) {
        struct buf *b = &p->bufs[i];
        for (int j = 0; j < b->nfds; j++) close(b->fds[j]);
        if (b->ahb) AHardwareBuffer_release(b->ahb);
    }
    if (p->window) ANativeWindow_release(p->window);
    pthread_mutex_destroy(&p->lock);
    pthread_cond_destroy(&p->cond);
    free(p);
}

JNIEXPORT jlong JNICALL Java_dev_omalauncher_p0_ZeroCopyP0Activity_nativeStart(
    JNIEnv *env, jobject thiz, jobject surface, jint width, jint height, jstring jmode, jstring jusage, jint nbuf,
    jint fd_index, jstring jsock, jint frames, jint verify_every) {
    (void)thiz;
    struct p0 *p = calloc(1, sizeof(*p));
    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->cond, NULL);
    p->listen_fd = p->client_fd = -1;
    p->shown = -1;
    p->width = width;
    p->height = height;
    p->nbuf = nbuf < 2 ? 2 : nbuf > ZC_MAX_BUFS ? ZC_MAX_BUFS : nbuf;
    p->frames = frames;
    p->verify_every = verify_every;

    const char *mode = (*env)->GetStringUTFChars(env, jmode, NULL);
    p->mode = !strcmp(mode, "cpu") ? MODE_CPU : MODE_GUEST;
    (*env)->ReleaseStringUTFChars(env, jmode, mode);
    const char *usage = jusage ? (*env)->GetStringUTFChars(env, jusage, NULL) : NULL;
    p->usage = parse_usage(usage, p->mode);
    if (usage) (*env)->ReleaseStringUTFChars(env, jusage, usage);
    const char *sock = (*env)->GetStringUTFChars(env, jsock, NULL);
    snprintf(p->sock_path, sizeof(p->sock_path), "%s", sock);
    (*env)->ReleaseStringUTFChars(env, jsock, sock);

    LOGI("start: %s %dx%d, %d buffers, usage %#llx, frames %d", p->mode == MODE_CPU ? "cpu" : "guest", width, height,
         p->nbuf, (unsigned long long)p->usage, frames);

    p->window = ANativeWindow_fromSurface(env, surface);
    if (!p->window) goto fail;
    p->sc = ASurfaceControl_createFromWindow(p->window, "zc-p0");
    if (!p->sc) {
        LOGE("ASurfaceControl_createFromWindow failed");
        goto fail;
    }
    if (alloc_buffers(p, fd_index)) goto fail;
    if (p->mode == MODE_GUEST && listen_on(p)) goto fail;
    if (pthread_create(&p->thread, NULL, p->mode == MODE_CPU ? cpu_thread : guest_thread, p)) goto fail;
    p->thread_started = 1;
    return (jlong)(intptr_t)p;
fail:
    destroy(p);
    return 0;
}

JNIEXPORT void JNICALL Java_dev_omalauncher_p0_ZeroCopyP0Activity_nativeStop(JNIEnv *env, jobject thiz, jlong handle) {
    (void)env;
    (void)thiz;
    if (handle) destroy((struct p0 *)(intptr_t)handle);
}
