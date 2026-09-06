package com.metashare.client;

import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.util.Log;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.util.ArrayDeque;
import java.util.concurrent.ConcurrentLinkedQueue;

/**
 * Hardware H.265/H.264 decoder backed by {@link MediaCodec} in async mode.
 * Accepts Annex B access units (start codes + NALs, parameter sets in-band) and
 * renders decoded frames directly to the supplied {@link Surface}.
 *
 * <p>Input buffers are recycled asynchronously. {@link #feed} queues complete
 * frames within a time and memory bound; codec callbacks submit them as soon
 * as an input buffer is available.
 */
public final class VideoDecoder {

    private static final String TAG = "VideoDecoder";

    public interface Listener {
        void onFirstFrameRendered();
        void onFrameResolutionChanged(int width, int height);
    }

    /** Asks the transport to make the streamer emit a fresh keyframe (PLI). */
    public interface KeyframeRequester {
        void request();
    }

    private static final long INPUT_BUDGET_MS = 40;
    private static final int MAX_PENDING_FRAMES = 4;
    private static final int MAX_PENDING_BYTES = 4 * 1024 * 1024;
    private final Object inputLock = new Object();
    private final ArrayDeque<PendingFrame> pendingInputs = new ArrayDeque<>();
    private final Runnable drainInputsTask = this::drainPendingInputs;
    private int pendingBytes;
    // A queued IDR opens the dependency chain; any dropped input closes it.
    private boolean inputChainReady;

    private static final class PendingFrame {
        final byte[] data;
        final long ptsUsec;
        final boolean keyframe;
        final long queuedMs = SystemClock.uptimeMillis();
        PendingFrame(byte[] data, long ptsUsec, boolean keyframe) {
            this.data = data;
            this.ptsUsec = ptsUsec;
            this.keyframe = keyframe;
        }
    }

    // Client-side playout (dejitter) cushion. Decoded output used to be released
    // ASAP, so any arrival/decode timing variance — which Mutter's damage-driven
    // virtual monitors have in abundance — showed straight through as judder even
    // though the streamer stamps a perfectly even PTS. Instead of releasing
    // immediately we schedule each frame for a wall-clock render time derived
    // from its PTS, holding this much lead so early/late arrivals are absorbed.
    // MediaCodec/SurfaceFlinger composites the held buffer at the requested
    // vsync, so this costs no extra thread. But a future render time keeps the
    // output buffer parked in the surface's BufferQueue until then, and that
    // queue is only a few deep and shared by all three HEVC decoders on the
    // Quest — hold ~2.4 frames (40ms@60fps) and the queue exhausts, the codec
    // can't dequeue output, stops draining input, and feed() drops incoming
    // (reference!) frames → artifacts. So keep the lead well under one frame:
    // enough to nudge a frame off "right now" onto the next vsync, not enough
    // to park buffers. Larger = more jitter absorbed but more latency AND more
    // starvation risk on the shared decoder.
    private static final long LEAD_MS = 8;
    // Bound future holding even when a stalled decoder emits a burst.
    private static final long MAX_LEAD_MS = 16;
    private static final long MAX_DECODED_AGE_US = 50_000;
    private volatile long latestQueuedPtsUsec = Long.MIN_VALUE;

    private MediaCodec codec;
    private HandlerThread callbackThread;
    private final ConcurrentLinkedQueue<Integer> freeInputs = new ConcurrentLinkedQueue<>();
    private volatile boolean firstFrameDone = false;
    private volatile boolean released = false;
    private Listener listener;
    private volatile KeyframeRequester keyframeRequester;

    // Diagnostics: count client-side frame drops (no free input buffer) and how
    // many of those were inter frames, logged ~1 Hz. A non-zero inter-drop rate
    // with 0% network loss explains a "rewinding" picture: dropping a P-frame
    // orphans every P-frame after it until the next keyframe.
    private long diagDrops = 0;
    private long diagInterDrops = 0;
    private long diagFed = 0;
    private long diagLastLogMs = 0;

    // Saved configuration so we can rebuild the codec if it hits a fatal error
    // (see onError → recover()). `surface` is volatile because setSurface()
    // updates it from the UI thread while the codec callbacks read it on cbHandler.
    private volatile Surface surface;
    private String mime;
    private int width;
    private int height;
    private Handler cbHandler;
    private volatile boolean recovering = false;

    // Self-heal a wedged codec. A codec can stop delivering input buffers for
    // good — it errored while the surface was momentarily invalid (so recover()
    // had to defer), or a shared hardware instance stalled. Nothing re-inits us
    // mid-session, so without this the decoder drops every keyframe for lack of a
    // buffer, the input gate never clears, and it PLI-loops on a frozen picture
    // forever. Two signals drive recovery: consecutive keyframes we could not
    // queue (codec isn't recycling buffers), and a deferred recover() waiting for
    // the surface to come back.
    private int keyframeDropStreak = 0;
    private static final int WEDGE_RESET_AFTER_KEYFRAME_DROPS = 2;
    private int recoverRetries = 0;
    private static final int RECOVER_MAX_RETRIES = 50;  // ~10s at 200ms
    private static final long RECOVER_RETRY_MS = 200;

    // Playout anchor mapping the sender's PTS timeline onto this device's
    // System.nanoTime() render clock (see scheduleRender). Established on the
    // first output frame and re-established on any discontinuity or codec reset.
    // Touched only from the codec callback thread (onOutputBufferAvailable and
    // recover both run on cbHandler), so no synchronization is needed.
    private boolean haveAnchor = false;
    private long anchorPtsUsec = 0;
    private long anchorRenderNs = 0;
    // The render time handed to the last frame. Every frame is scheduled at
    // strictly >= this, so frames only ever move forward on the surface — a late
    // or re-anchored frame is nudged up to the present instead of being placed
    // before the frame already queued ahead of it (which would flip the picture
    // back to an older frame: the "flicker between frames" we must never do).
    private long lastRenderNs = 0;

    public void init(Surface surface, String codecName, int width, int height,
                     Listener listener) throws Exception {
        this.listener = listener;
        this.surface = surface;
        this.width = width;
        this.height = height;
        this.haveAnchor = false;
        this.lastRenderNs = 0;
        this.latestQueuedPtsUsec = Long.MIN_VALUE;
        this.mime = "h265".equalsIgnoreCase(codecName)
                ? MediaFormat.MIMETYPE_VIDEO_HEVC
                : MediaFormat.MIMETYPE_VIDEO_AVC;

        callbackThread = new HandlerThread("VideoDecoderCb");
        callbackThread.start();
        cbHandler = new Handler(callbackThread.getLooper());

        codec = MediaCodec.createDecoderByType(mime);
        codec.setCallback(callback, cbHandler);
        codec.configure(buildFormat(), surface, null, 0);
        codec.start();
        Log.i(TAG, "opened " + mime + " " + width + "x" + height);
    }

    private MediaFormat buildFormat() {
        MediaFormat fmt = MediaFormat.createVideoFormat(mime,
                Math.max(1, width), Math.max(1, height));
        fmt.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 4 * 1024 * 1024);
        if (codec.getCodecInfo().getCapabilitiesForType(mime).isFeatureSupported(
                MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency)) {
            fmt.setInteger(MediaFormat.KEY_LOW_LATENCY, 1);
        }
        return fmt;
    }

    /**
     * Rebuild the codec after a fatal error. A single malformed access unit (or
     * a transient hardware hiccup while several HEVC decoders share the Quest's
     * decoder) can push MediaCodec into an unrecoverable Error state, after which
     * every feed() just fails against a dead codec — a permanent black window,
     * since the session loop only reconnects on a *signaling* drop, not a decoder
     * fault. Reset back to a fresh, re-gated decoder and let the next keyframe
     * (the streamer emits them periodically) bring the picture back. Runs on the
     * callback thread so it is serialized with the codec's other callbacks.
     */
    private void recover() {
        if (released || codec == null) return;
        // If the output surface is gone (window closed/recreated), there is
        // nothing to render to — reconfiguring would just fail. Leave the gate
        // armed and let the Activity's surface-recreation path re-init us.
        if (surface == null || !surface.isValid()) {
            // The surface is momentarily gone (panel/window churn). We can't
            // reconfigure onto an invalid surface, but nothing else re-inits us
            // mid-session, so don't drop the recovery on the floor — re-check
            // shortly and rebuild once the surface is valid again. Keep
            // recovering=true across the wait so feed()/onError don't re-trigger.
            if (++recoverRetries <= RECOVER_MAX_RETRIES) {
                Log.i(TAG, "codec error but surface invalid — retrying recover ("
                        + recoverRetries + ")");
                cbHandler.postDelayed(this::recover, RECOVER_RETRY_MS);
                return;
            }
            Log.w(TAG, "surface still invalid after " + recoverRetries
                    + " retries — giving up recovery until re-init");
            recoverRetries = 0;
            recovering = false;
            return;
        }
        try {
            freeInputs.clear();
            synchronized (inputLock) { clearPendingInputs(); }
            firstFrameDone = false;
            haveAnchor = false;        // new IDR re-anchors the playout timeline
            lastRenderNs = 0;          // and restarts the forward-only clock
            latestQueuedPtsUsec = Long.MIN_VALUE;
            keyframeDropStreak = 0;
            recoverRetries = 0;
            codec.reset();
            codec.setCallback(callback, cbHandler);
            codec.configure(buildFormat(), surface, null, 0);
            codec.start();
            Log.i(TAG, "codec recovered (reset + awaiting keyframe)");
        } catch (Exception e) {
            Log.e(TAG, "codec recovery failed: " + e.getMessage());
        } finally {
            recovering = false;
        }
    }

    private final MediaCodec.Callback callback = new MediaCodec.Callback() {
        @Override
        public void onInputBufferAvailable(MediaCodec c, int index) {
            if (released || recovering) return;
            freeInputs.add(index);
            drainPendingInputs();
        }

        @Override
        public void onOutputBufferAvailable(MediaCodec c, int index,
                                            MediaCodec.BufferInfo info) {
            try {
                // Always schedule with an explicit, monotonically forward render
                // time (never the 2-arg "render now" path): mixing immediate and
                // timestamped releases on one surface can let a late frame land
                // ahead of a held one and flip the picture backward.
                if (latestQueuedPtsUsec != Long.MIN_VALUE &&
                        info.presentationTimeUs < latestQueuedPtsUsec - MAX_DECODED_AGE_US) {
                    c.releaseOutputBuffer(index, false);
                    return;
                }
                long renderNs = scheduleRender(info.presentationTimeUs);
                c.releaseOutputBuffer(index, renderNs);
            } catch (Exception ignored) {
            }
            if (!firstFrameDone) {
                firstFrameDone = true;
                if (listener != null) listener.onFirstFrameRendered();
            }
        }

        @Override
        public void onOutputFormatChanged(MediaCodec c, MediaFormat format) {
            try {
                int w = format.getInteger(MediaFormat.KEY_WIDTH);
                int h = format.getInteger(MediaFormat.KEY_HEIGHT);
                int rotation = 0;
                if (format.containsKey("rotation-degrees"))
                    rotation = format.getInteger("rotation-degrees");
                int reportedW = (rotation == 90 || rotation == 270) ? h : w;
                int reportedH = (rotation == 90 || rotation == 270) ? w : h;
                Log.i(TAG, "format changed " + w + "x" + h + " rot=" + rotation);
                if (listener != null)
                    listener.onFrameResolutionChanged(reportedW, reportedH);
            } catch (Exception e) {
                Log.w(TAG, "format change parse failed: " + e.getMessage());
            }
        }

        @Override
        public void onError(MediaCodec c, MediaCodec.CodecException e) {
            // Re-arm the gate: whatever comes next, don't feed inter frames until
            // a keyframe re-establishes decodable state.
            synchronized (inputLock) { clearPendingInputs(); }
            Log.e(TAG, "codec error: " + e.getErrorCode() + " " + e.getMessage()
                    + " recoverable=" + e.isRecoverable()
                    + " transient=" + e.isTransient());
            // Rebuild the codec so it doesn't stay wedged forever. Serialized on
            // this (callback) thread via post to avoid reentrancy with the codec.
            if (released || recovering) return;
            recovering = true;
            cbHandler.post(VideoDecoder.this::recover);
        }
    };

    /**
     * Map a decoded frame's PTS to a target render time on the system monotonic
     * clock ({@link System#nanoTime}), the timebase the 3-arg {@link
     * MediaCodec#releaseOutputBuffer(int, long)} schedules against. Always
     * returns a concrete render time that is >= now and >= the previous frame's,
     * so playout only ever advances.
     *
     * <p>The streamer stamps an even PTS, so honouring that spacing here — rather
     * than rendering whenever the decoder happens to emit a frame — is what turns
     * jittery arrival into smooth playout. Runs only on the codec callback thread.
     */
    private long scheduleRender(long ptsUsec) {
        long nowNs = System.nanoTime();
        long targetNs = 0;  // always overwritten below; set for definite-assignment
        // Normal case: place the frame on the established PTS timeline.
        boolean onTimeline = haveAnchor && ptsUsec >= anchorPtsUsec;
        if (onTimeline) {
            targetNs = anchorRenderNs + (ptsUsec - anchorPtsUsec) * 1000L;
            long window = MAX_LEAD_MS * 1_000_000L;
            if (targetNs > nowNs + window || targetNs < nowNs - window) {
                onTimeline = false;  // timeline broke (stall / wrap / drift)
            }
        }
        if (!onTimeline) {
            // (Re)anchor: build a fresh LEAD_MS cushion on this frame and peg the
            // PTS timeline to it. A backward PTS jump (stream reset / RTP-ts wrap)
            // also lands here.
            anchorPtsUsec = ptsUsec;
            anchorRenderNs = nowNs + LEAD_MS * 1_000_000L;
            haveAnchor = true;
            targetNs = anchorRenderNs;
        }
        // Forward-only floor: never before the present, never before the frame
        // already queued ahead of this one. A frame that arrived late is nudged
        // to "as soon as possible, but after its predecessor"; the surface then
        // shows the newest frame due at each vsync and simply skips the ones that
        // bunched up — moving forward, never back.
        long floorNs = Math.max(nowNs, lastRenderNs + 1);
        if (targetNs < floorNs) targetNs = floorNs;
        lastRenderNs = targetNs;
        return targetNs;
    }

    /** Hand off a bounded, dependency-preserving queue without waiting on RTP. */
    public void feed(byte[] annexB, long ptsUsec, boolean keyframe) {
        if (released || recovering || codec == null) return;
        synchronized (inputLock) {
            PendingFrame oldest = pendingInputs.peekFirst();
            if (pendingInputs.size() >= MAX_PENDING_FRAMES ||
                    pendingBytes + annexB.length > MAX_PENDING_BYTES ||
                    (oldest != null && SystemClock.uptimeMillis() - oldest.queuedMs >= INPUT_BUDGET_MS)) {
                dropPendingInputs();
            }
            if (annexB.length > MAX_PENDING_BYTES) return;
            if (!inputChainReady && !keyframe) return;
            inputChainReady = true;
            pendingInputs.addLast(new PendingFrame(annexB, ptsUsec, keyframe));
            pendingBytes += annexB.length;
            cbHandler.removeCallbacks(drainInputsTask);
            cbHandler.post(drainInputsTask);
        }
    }

    // Called with inputLock held. Codec work remains on the callback thread.
    private void clearPendingInputs() {
        pendingInputs.clear();
        pendingBytes = 0;
        inputChainReady = false;
    }

    private void dropPendingInputs() {
        boolean lostKeyframe = false;
        for (PendingFrame pending : pendingInputs) {
            diagDrops++;
            if (pending.keyframe) lostKeyframe = true;
            else diagInterDrops++;
        }
        clearPendingInputs();
        if (lostKeyframe && ++keyframeDropStreak >= WEDGE_RESET_AFTER_KEYFRAME_DROPS && !recovering) {
            recovering = true;
            cbHandler.post(this::recover);
        }
        // Post feedback too: feed() never waits for codec buffers or socket I/O.
        cbHandler.post(() -> {
            if (!released && keyframeRequester != null) keyframeRequester.request();
        });
    }

    private void drainPendingInputs() {
        while (!released && !recovering) {
            PendingFrame pending;
            Integer idx;
            synchronized (inputLock) {
                cbHandler.removeCallbacks(drainInputsTask);
                pending = pendingInputs.peekFirst();
                if (pending == null) return;
                long remaining = INPUT_BUDGET_MS - (SystemClock.uptimeMillis() - pending.queuedMs);
                if (remaining <= 0) {
                    dropPendingInputs();
                    return;
                }
                idx = freeInputs.poll();
                if (idx == null) {
                    // Input callbacks wake this immediately; the timer only
                    // expires stale work if the codec stops returning buffers.
                    cbHandler.postDelayed(drainInputsTask, remaining);
                    return;
                }
                pendingInputs.removeFirst();
                pendingBytes -= pending.data.length;
            }
            try {
                ByteBuffer buf = codec.getInputBuffer(idx);
                if (buf == null || pending.data.length > buf.capacity()) {
                    codec.queueInputBuffer(idx, 0, 0, pending.ptsUsec, 0);
                    synchronized (inputLock) { dropPendingInputs(); }
                    continue;
                }
                buf.clear();
                buf.put(pending.data);
                int flags = pending.keyframe ? MediaCodec.BUFFER_FLAG_KEY_FRAME : 0;
                codec.queueInputBuffer(idx, 0, pending.data.length, pending.ptsUsec, flags);
                latestQueuedPtsUsec = pending.ptsUsec;
                keyframeDropStreak = 0;
                diagFed++;
                maybeLogDiag();
            } catch (Exception e) {
                synchronized (inputLock) { dropPendingInputs(); }
                Log.w(TAG, "feed failed: " + e.getMessage());
            }
        }
    }

    /** Wire the transport used to request a fresh keyframe on a dropped IDR. */
    public void setKeyframeRequester(KeyframeRequester requester) {
        this.keyframeRequester = requester;
    }

    /**
     * Rebind the codec to a recreated output surface. The Quest destroys and
     * recreates a SurfaceView's Surface during normal panel churn; the codec
     * bound to the old (now invalid) Surface errors and — with nothing else
     * re-initing it mid-session — wedges permanently. Prefer a live {@link
     * MediaCodec#setOutputSurface} swap (seamless, no keyframe wait); if the
     * codec already errored on the dead surface that throws, so fall back to a
     * full reset onto the new surface. Runs the codec work on cbHandler so it is
     * serialized with the codec's error/output callbacks.
     */
    public void setSurface(Surface s) {
        if (released || s == null || !s.isValid()) return;
        this.surface = s;
        Handler h = cbHandler;
        if (h == null || codec == null) return;
        h.post(() -> {
            if (released || codec == null) return;
            Surface cur = surface;
            if (cur == null || !cur.isValid()) return;
            if (!recovering) {
                try {
                    codec.setOutputSurface(cur);
                    recoverRetries = 0;
                    Log.i(TAG, "output surface swapped (no reset)");
                    return;
                } catch (Exception e) {
                    Log.w(TAG, "setOutputSurface failed (" + e.getMessage()
                            + ") — full reset onto new surface");
                }
            }
            // Codec was already errored/wedged on the dead surface: rebuild it
            // onto the new one. recover() re-reads `surface`, now valid.
            recovering = true;
            recover();
        });
    }

    private void maybeLogDiag() {
        long now = SystemClock.uptimeMillis();
        if (diagLastLogMs == 0) { diagLastLogMs = now; return; }
        if (now - diagLastLogMs < 1000) return;
        if (diagDrops > 0) {
            Log.w(TAG, "feed 1s: fed=" + diagFed + " dropped=" + diagDrops
                    + " (inter=" + diagInterDrops + ") — input-buffer starvation");
        }
        diagFed = 0; diagDrops = 0; diagInterDrops = 0;
        diagLastLogMs = now;
    }

    public void release() {
        released = true;
        synchronized (inputLock) {
            clearPendingInputs();
            if (cbHandler != null) cbHandler.removeCallbacks(drainInputsTask);
        }
        freeInputs.clear();
        if (codec != null) {
            try {
                codec.stop();
            } catch (Exception ignored) {
            }
            try {
                codec.release();
            } catch (Exception ignored) {
            }
            codec = null;
        }
        if (callbackThread != null) {
            callbackThread.quitSafely();
            callbackThread = null;
        }
    }
}
