/*
 * Zero-copy P0: the wire protocol between the launcher (Android, bionic) and
 * the guest client (Linux, glibc, under proot). Debug builds only.
 *
 * One AF_UNIX SOCK_SEQPACKET connection: every message is one packet, a buffer's
 * dma-buf fd rides on its BUFFER packet (SCM_RIGHTS). Both ends are aarch64
 * little-endian, so the structs go over the wire as they are.
 *
 *   launcher -> guest   HELLO, BUFFER x nbuf, RELEASE..., BYE
 *   guest -> launcher   READY, PRESENT..., BYE
 *
 * Every buffer starts free on the guest side. PRESENT hands a finished buffer
 * to the launcher (stage 1: the guest waited for its GPU work, no fence); the
 * launcher shows it and sends RELEASE once the compositor let go of it.
 */
#ifndef ZC_PROTO_H
#define ZC_PROTO_H

#include <stdint.h>

#define ZC_PROTO_VERSION 1
#define ZC_MAX_BUFS 4
#define ZC_SOCKET_NAME "zc-p0.sock"

/* DRM fourcc of AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM ('AB24', bytes R,G,B,A). */
#define ZC_DRM_FORMAT_ABGR8888 0x34324241u
#define ZC_DRM_FORMAT_MOD_LINEAR 0ull

enum zc_msg_type {
    ZC_MSG_HELLO = 1,
    ZC_MSG_BUFFER = 2,
    ZC_MSG_READY = 3,
    ZC_MSG_PRESENT = 4,
    ZC_MSG_RELEASE = 5,
    ZC_MSG_BYE = 6,
};

struct zc_hdr {
    uint32_t type;
    uint32_t size; /* whole packet, header included */
};

struct zc_hello {
    struct zc_hdr h;
    uint32_t version;
    uint32_t nbuf;
    uint32_t width;
    uint32_t height;
    /* Buffers the compositor still holds (a previous client's last frame); a RELEASE frees each. */
    uint32_t busy_mask;
};

/* One buffer; its pixel dma-buf fd is attached. */
struct zc_buffer {
    struct zc_hdr h;
    uint32_t index;
    uint32_t width;
    uint32_t height;
    uint32_t ahb_format;   /* AHARDWAREBUFFER_FORMAT_* */
    uint32_t drm_format;   /* fourcc */
    uint32_t stride_px;    /* AHardwareBuffer_Desc.stride */
    uint32_t stride_bytes; /* row pitch the guest must use */
    uint32_t offset;       /* plane 0 offset in the dma-buf */
    uint64_t dmabuf_size;  /* lseek(fd, 0, SEEK_END) */
    uint64_t modifier;     /* what the launcher believes the layout is */
    uint64_t usage;        /* AHARDWAREBUFFER_USAGE_* it was allocated with */
    uint32_t handle_fds;   /* fds in the native handle */
    uint32_t fd_index;     /* which one is attached */
};

struct zc_ready {
    struct zc_hdr h;
    uint32_t ok;
    uint32_t path; /* enum zc_path */
    char device[64];
};

enum zc_path {
    ZC_PATH_NONE = 0,
    ZC_PATH_IMAGE = 1,  /* rendered straight into the imported image */
    ZC_PATH_BUFFER = 2, /* rendered offscreen, copied by the GPU into the imported buffer */
};

struct zc_present {
    struct zc_hdr h;
    uint32_t index;
    uint32_t frame;
    uint64_t render_ns; /* CPU time from frame start to GPU idle, guest side */
};

struct zc_release {
    struct zc_hdr h;
    uint32_t index;
    uint32_t frame; /* the frame that was shown in it */
};

struct zc_bye {
    struct zc_hdr h;
    uint32_t reason;
};

#endif
