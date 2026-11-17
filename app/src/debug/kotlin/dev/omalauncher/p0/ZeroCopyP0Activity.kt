package dev.omalauncher.p0

import android.app.Activity
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowInsets
import android.view.WindowManager
import dev.omalauncher.App
import dev.omalauncher.linux.JobState
import dev.omalauncher.linux.Proot
import java.io.File
import kotlin.concurrent.thread

/**
 * Zero-copy P0 (debug builds only): frames from the guest's GPU on screen without a CPU copy.
 * The launcher allocates AHardwareBuffers and shows them through ASurfaceControl (zcp0.c); the guest
 * client (oma-zc-client, from the debug assets) imports their dma-bufs into Vulkan and renders into them.
 *
 *     adb shell am start -S -n dev.omalauncher.zc/dev.omalauncher.p0.ZeroCopyP0Activity \
 *         [--es mode guest|cpu] [--es usage linear|gpu|cpu|0x...] [--ei buffers 3] [--ei fd -1] \
 *         [--ei frames 600] [--ei verify 120] [--es args "--path auto|image|buffer ..."] [--es env "VAR=1 ..."]
 *
 * mode cpu draws the test pattern on the CPU (the display half alone, no Linux needed). With the activity
 * running, another start with only args/env runs the guest client again on the same buffers. Logs: tag zc-p0.
 */
class ZeroCopyP0Activity : Activity(), SurfaceHolder.Callback {
    private var handle = 0L
    private var guest: Process? = null
    private var guestThread: Thread? = null

    private val runtime get() = (application as App).runtime

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Starts Linux (and with it Gpu.recheck) when the app was started straight into this activity.
        runtime.start()
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        // A test run from adb shows even on a locked, sleeping phone (over the lock screen, nothing unlocked).
        setShowWhenLocked(true)
        setTurnScreenOn(true)
        val view = SurfaceView(this)
        view.holder.addCallback(this)
        setContentView(view)
        if (Build.VERSION.SDK_INT >= 30) window.insetsController?.hide(WindowInsets.Type.systemBars())
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        if (handle != 0L && mode == "guest") startGuest()
    }

    private val mode get() = intent.getStringExtra("mode") ?: "guest"

    override fun surfaceCreated(holder: SurfaceHolder) {}

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        // One size per run: a resize (rotation, fold) needs new buffers, which is P3.
        if (handle != 0L) return
        val socket = File(runtime.proot.runtimeDir, SOCKET)
        socket.parentFile?.mkdirs()
        handle =
            nativeStart(
                holder.surface, width, height, mode, intent.getStringExtra("usage"),
                intent.getIntExtra("buffers", 3), intent.getIntExtra("fd", -1), socket.path,
                intent.getIntExtra("frames", 600), intent.getIntExtra("verify", 120),
            )
        if (handle == 0L) {
            Log.e(TAG, "start failed")
            return
        }
        if (mode == "guest") startGuest()
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) = stop()

    override fun onDestroy() {
        stop()
        super.onDestroy()
    }

    private fun stop() {
        guest?.destroy()
        guest = null
        if (handle != 0L) nativeStop(handle)
        handle = 0L
    }

    /** Copies the client into the guest and runs it there, its output to logcat. */
    private fun startGuest() {
        if (guestThread?.isAlive == true) {
            Log.w(TAG, "the guest client is still running")
            return
        }
        val args = intent.getStringExtra("args").orEmpty()
        val env = intent.getStringExtra("env").orEmpty()
        guestThread =
            thread(name = "zc-p0-guest", isDaemon = true) {
                val proot = runtime.proot
                if (!proot.rootfs.isDirectory) {
                    Log.e(TAG, "Linux is not installed in this app")
                    return@thread
                }
                // The GPU env is set once Gpu.recheck passes, a moment after the app starts. --ez gpu true turns it on
                // like settings › linux does (driver download, pacman), and waits for that install and its check.
                val gpu = runtime.gpu
                // Linux starts in the background after the app does; until then setGpu and Gpu.recheck do nothing.
                val linuxBy = System.currentTimeMillis() + 60_000
                while (!runtime.linuxRunning && System.currentTimeMillis() < linuxBy) Thread.sleep(200)
                Log.i(TAG, "linux running=${runtime.linuxRunning}")
                if (intent.getBooleanExtra("gpu", false) && !gpu.wanted) {
                    Log.i(TAG, "turning the GPU on (supported=${gpu.supported}, ${gpu.driverLabel})")
                    runtime.setGpu(true)
                    Thread.sleep(1000)
                }
                val start = System.currentTimeMillis()
                var lastState = ""
                while (gpu.wanted && !gpu.active) {
                    val working = gpu.state is JobState.Working
                    if (!working && System.currentTimeMillis() - start > 15_000) break
                    if (System.currentTimeMillis() - start > 20 * 60_000) break
                    val state =
                        when (val s = gpu.state) {
                            is JobState.Working -> "working: ${s.log.lastOrNull().orEmpty()}"
                            is JobState.Failed -> "failed: ${s.log.takeLast(15).joinToString(" | ")}"
                            JobState.Idle -> "idle"
                        }
                    if (state != lastState) Log.i(TAG, "gpu: $state").also { lastState = state }
                    Thread.sleep(500)
                }
                (gpu.state as? JobState.Failed)?.let { Log.w(TAG, "gpu failed: ${it.log.takeLast(15).joinToString(" | ")}") }
                Log.i(TAG, "gpu wanted=${gpu.wanted} active=${gpu.active} device=${gpu.device}; env ${proot.graphicsEnv}")
                val bin = proot.hostFile(CLIENT)
                bin.parentFile?.mkdirs()
                try {
                    assets.open("zc/oma-zc-client").use { input -> bin.outputStream().use { input.copyTo(it) } }
                } catch (_: java.io.FileNotFoundException) {
                    Log.e(TAG, "no guest client in this APK: run scripts/zero-copy/build.sh and build again")
                    return@thread
                }
                bin.setExecutable(true, false)
                val command = "exec env $env $CLIENT --socket ${Proot.GUEST_RUNTIME_DIR}/$SOCKET $args"
                Log.i(TAG, "guest: $command")
                val process = runCatching { proot.process(command).start() }.getOrElse {
                    Log.e(TAG, "guest did not start", it)
                    return@thread
                }
                guest = process
                process.inputStream.bufferedReader().forEachLine { Log.i("$TAG/guest", it) }
                Log.i(TAG, "guest exited ${process.waitFor()}")
            }
    }

    private external fun nativeStart(
        surface: android.view.Surface,
        width: Int,
        height: Int,
        mode: String,
        usage: String?,
        buffers: Int,
        fdIndex: Int,
        socket: String,
        frames: Int,
        verifyEvery: Int,
    ): Long

    private external fun nativeStop(handle: Long)

    companion object {
        private const val TAG = "zc-p0"
        private const val SOCKET = "zc-p0.sock"
        private const val CLIENT = "/usr/local/bin/oma-zc-client"

        init {
            System.loadLibrary("zcp0")
        }
    }
}
