# Smoke test for oma-zc-client without a phone: a stand-in launcher that offers memfds as
# "dma-bufs" (enough for the protocol and Vulkan setup; real import needs the phone). Checks the
# pattern the client renders against zc_pattern (zcp0.c). After build.sh, from this directory:
#   docker run --rm --platform linux/arm64 -v "$PWD:/t" debian:trixie sh -c 'apt-get update -qq &&
#     apt-get install -y -qq mesa-vulkan-drivers libvulkan1 python3 >/dev/null &&
#     python3 /t/fake_launcher.py /t/build/oma-zc-client --path image'
# lavapipe from Mesa 25 imports a memfd as a dma-buf; older ones have no VK_EXT_external_memory_dma_buf.
import math, mmap, os, socket, struct, subprocess, sys, time
path = "/tmp/zc-test.sock"
w, h, n = 256, 128, 3
stride = 256
if os.path.exists(path): os.unlink(path)
srv = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET); srv.bind(path); srv.listen(1)
client = subprocess.Popen([sys.argv[1], "--socket", path, "--frames", "30"] + sys.argv[2:])
c, _ = srv.accept()
c.send(struct.pack("<IIIIIII", 1, 28, 1, n, w, h, 0))
maps = []
def pattern(x, y, frame):  # zc_pattern in zcp0.c
    px, py = x + 0.5, y + 0.5
    r, g, b = px / w, py / h, 0.5 + 0.5 * math.sin(frame / 60.0)
    if px % 64 < 2 or py % 64 < 2: r = g = b = 1.0
    bx = (frame * 8.0) % w
    if bx <= px < bx + 32: r, g, b = 1.0, 0.0, 1.0
    s = 96.0; L, R, T, B = px < s, px >= w - s, py < s, py >= h - s
    if L and T: r, g, b = 1, 0, 0
    if R and T: r, g, b = 0, 1, 0
    if L and B: r, g, b = 0, 0, 1
    if R and B: r, g, b = 1, 1, 1
    u = lambda v: 0 if v <= 0 else 255 if v >= 1 else int(v * 255 + 0.5)
    return (u(r), u(g), u(b), 255)
checked = [0, 0]
for i in range(n):
    fd = os.memfd_create(f"buf{i}"); os.ftruncate(fd, stride * h * 4)
    maps.append(mmap.mmap(fd, stride * h * 4))
    msg = struct.pack("<IIIIIIIIIIQQQII", 2, 72, i, w, h, 1, 0x34324241, stride, stride * 4, 0, stride * h * 4, 0, 0x300, 1, 0)
    assert len(msg) == 72, len(msg)
    socket.send_fds(c, [msg], [fd])
c.settimeout(20)
try:
    while True:
        try: m = c.recv(256)
        except ConnectionResetError: break
        if not m: break
        t, size = struct.unpack_from("<II", m)
        if t == 3: print("fake: READY ok=%d path=%d device=%s" % (struct.unpack_from("<II", m, 8) + (m[16:80].split(b"\0")[0].decode(),)))
        elif t == 4:
            idx, frame = struct.unpack_from("<II", m, 8)
            for x in range(0, w, 7):
                for y in range(0, h, 5):
                    o = (y * stride + x) * 4
                    got, want = tuple(maps[idx][o:o + 4]), pattern(x, y, frame)
                    ok = all(abs(a - b) <= 3 for a, b in zip(got, want))
                    checked[0 if ok else 1] += 1
                    if not ok and checked[1] <= 3: print("fake: mismatch frame", frame, (x, y), got, want)
            try: c.send(struct.pack("<IIII", 5, 16, idx, frame))
            except OSError: pass
        elif t == 6: print("fake: BYE"); break
except socket.timeout:
    print("fake: timeout")
print("fake: client exit", client.wait(), "pixels ok/bad:", checked)
