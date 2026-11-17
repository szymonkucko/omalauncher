package dev.omalauncher.wayland

import java.util.concurrent.atomic.AtomicInteger

/** DRM fourccs of the formats offered: bytes R, G, B, A, as AHardwareBuffer's R8G8B8A8. */
internal const val DRM_FORMAT_ABGR8888 = 0x34324241
internal const val DRM_FORMAT_XBGR8888 = 0x34324258

/** Largest width or height of a buffer, as for a pixel store. */
private const val MAX_SIZE = 16384

/**
 * oma_ahb_allocator_v1 (scripts/zero-copy/oma-ahb-v1.xml): buffers we allocate as AHardwareBuffers and the client
 * renders into through their dma-bufs (GPU apps, through a Vulkan layer in the guest). A surface showing one is a
 * layer of its own on screen, see [WindowRenderer]: the pixels are never copied, unlike wl_shm's.
 */
class AhbAllocator(
    client: Client,
    id: Int,
    version: Int,
) : Resource(client, id, version) {
    override val iface = "oma_ahb_allocator_v1"

    override fun request(
        opcode: Int,
        args: Args,
    ) {
        when (opcode) {
            0 -> destroy()
            1 -> allocate(args.newId(), args.int(), args.int(), args.uint())
        }
    }

    private fun allocate(
        bufferId: Int,
        width: Int,
        height: Int,
        format: Int,
    ) {
        if (format != DRM_FORMAT_ABGR8888 && format != DRM_FORMAT_XBGR8888) return client.postError(id, INVALID_FORMAT, "format $format")
        if (width !in 1..MAX_SIZE || height !in 1..MAX_SIZE) return client.postError(id, INVALID_SIZE, "size ${width}x$height")
        val info = IntArray(3)
        // A few milliseconds under the server lock, a handful of times per window: not worth deferring.
        val ahb = Native.ahbAllocate(width, height, info)
        // Registered either way: the id is the client's, and a failed buffer still needs destroying.
        client.register(AhbBuffer(client, bufferId, ahb, width, height, opaque = format == DRM_FORMAT_XBGR8888))
        if (ahb == 0L) return send(event(1).obj(bufferId))
        // fd, offset, stride, modifier (linear: 0, 0); the fd isn't part of the message body.
        send(event(0).obj(bufferId).fd(info[0], close = true).uint(info[1]).uint(info[2]).uint(0).uint(0))
    }

    private companion object {
        const val INVALID_FORMAT = 0
        const val INVALID_SIZE = 1
    }
}

/**
 * The wl_buffer of an AHardwareBuffer from [AhbAllocator]. The client gets it back (wl_buffer.release) once nothing
 * uses its pixels: its surface moved on to another buffer, no copy of it is in progress, and the display let go.
 */
class AhbBuffer(
    client: Client,
    id: Int,
    ahb: Long,
    val width: Int,
    val height: Int,
    /** XBGR: alpha is to be ignored. */
    val opaque: Boolean,
) : Resource(client, id, 1) {
    override val iface = "wl_buffer"

    /** The AHardwareBuffer, 0 if allocating failed or the client destroyed it. Hold [this] while using it. */
    @Volatile
    var ptr = ahb
        private set

    private val holds = AtomicInteger()

    fun hold() {
        holds.incrementAndGet()
    }

    fun unhold() {
        if (holds.decrementAndGet() == 0 && ptr != 0L && client.alive) send(event(0))
    }

    /**
     * Holds the buffer if something still does (its surface, say); false once the client got it back, when it may be
     * drawing into it again: then it's not to be shown or copied.
     */
    fun tryHold(): Boolean {
        while (true) {
            val n = holds.get()
            if (n <= 0) return false
            if (holds.compareAndSet(n, n + 1)) return true
        }
    }

    /** A hold for the display, which gives it back by running it once the buffer is off screen; null as [tryHold]. */
    fun displayHold(): Runnable? = if (tryHold()) Runnable(::unhold) else null

    override fun request(
        opcode: Int,
        args: Args,
    ) {
        if (opcode == 0) destroy()
    }

    // The display keeps its own reference to a buffer it shows, so the client may destroy one that's still on screen.
    override fun onDestroy() =
        synchronized(this) {
            Native.ahbRelease(ptr)
            ptr = 0
        }
}
