package com.metashare.client;

import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.util.Log;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.util.concurrent.ConcurrentLinkedQueue;

/**
 * Hardware H.265/H.264 decoder backed by {@link MediaCodec} in async mode.
 * Accepts Annex B access units (start codes + NALs, parameter sets in-band) and
 * renders decoded frames directly to the supplied {@link Surface}.
 *
 * <p>Input buffers are recycled asynchronously: {@link #feed} hands a frame to
 * the next free input buffer, or drops it if the decoder has none available
 * (keeps the receive thread from blocking on a slow decoder).
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

    // A keyframe (IDR) is the decoder's only resync point and IDRs are rare, so
    // when no input buffer is free we wait briefly for one rather than dropping
    // it outright. Bounded so a wedged decoder can't hang the receive thread.
    private static final long KEYFRAME_WAIT_MS = 40;

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
    // If a frame's scheduled render time lands this far outside the present the
    // playout timeline has broken (a long stall, an RTP-timestamp wrap, or slow
    // sender/client clock drift accumulating) — rebuild the cushion from the
    // current frame. Kept wide so an ordinary post-stall decode burst, whose
    // frames are legitimately spread across their PTS cadence, is paced out
    // rather than collapsed.
    private static final long MAX_LEAD_MS = 250;

    private MediaCodec codec;
    private HandlerThread callbackThread;
    private final ConcurrentLinkedQueue<Integer> freeInputs = new ConcurrentLinkedQueue<>();
    private volatile boolean firstFrameDone = false;
    private volatile boolean released = false;
    // The HEVC decoder is configured without codec-specific data, so it needs
    // the parameter sets (VPS/SPS/PPS) that ride in-band with each IDR before it
    // can decode anything. Until the first keyframe arrives we must NOT feed it
    // inter frames: their slices reference parameter sets it doesn't have yet,
    // which the firmware rejects as CONFIG_FLAG_MISSING and can wedge the
    // decoder permanently. Starts true (nothing decodable yet); cleared once a
    // keyframe is queued. The session sends a PLI at start and the streamer
    // emits periodic IDRs, so the gate clears within ~one keyframe interval.
    private volatile boolean awaitingKeyframe = true;
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
    // (see onError → recover()).
    private Surface surface;
    private String mime;
    private int width;
    private int height;
    private Handler cbHandler;
    private volatile boolean recovering = false;

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
            Log.i(TAG, "codec error but surface invalid — deferring to surface recreate");
            recovering = false;
            return;
        }
        try {
            freeInputs.clear();
            awaitingKeyframe = true;   // don't feed until parameter sets return
            firstFrameDone = false;
            haveAnchor = false;        // new IDR re-anchors the playout timeline
            lastRenderNs = 0;          // and restarts the forward-only clock
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
            freeInputs.add(index);
        }

        @Override
        public void onOutputBufferAvailable(MediaCodec c, int index,
                                            MediaCodec.BufferInfo info) {
            try {
                // Always schedule with an explicit, monotonically forward render
                // time (never the 2-arg "render now" path): mixing immediate and
                // timestamped releases on one surface can let a late frame land
                // ahead of a held one and flip the picture backward.
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
            awaitingKeyframe = true;
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

    /**
     * Feed one Annex B access unit (pts in microseconds). If the decoder has no
     * free input buffer it is falling behind (common when several HEVC streams
     * share the Quest's decoder); we drop this frame and keep going rather than
     * block the receive thread. Dropping a reference frame causes brief
     * corruption until the next periodic keyframe, but that is far less
     * disruptive than trying to "recover" — under a sustained throughput
     * deficit, requesting a keyframe only makes the backlog worse (an IDR is the
     * most expensive frame to decode), which spirals into a permanent stall.
     */
    public void feed(byte[] annexB, long ptsUsec, boolean keyframe) {
        if (released || codec == null) return;
        // Gate: drop everything until the first keyframe rebuilds the decoder's
        // parameter-set state. This is the single most important guard — feeding
        // pre-IDR slices is what wedges the hardware decoder (CONFIG_FLAG_MISSING
        // → rejected buffers → permanent black). No keyframe is requested here on
        // purpose: the session's start-of-stream PLI and the streamer's periodic
        // IDRs clear the gate, and requesting one per dropped frame would flood
        // the streamer with expensive IDRs and spiral the whole thing.
        if (awaitingKeyframe && !keyframe) return;
        Integer idx = freeInputs.poll();
        if (idx == null && keyframe) {
            // Don't drop an IDR on the first empty poll: dropping the sole
            // resync point strands the whole following GOP on references the
            // decoder never received, which shows as the picture "looping"
            // between stale frames until the next IDR. On the Quest's shared
            // HEVC decoder one stream's IDR transiently starves the others'
            // input buffers exactly when their own IDR lands, so a short wait
            // almost always recovers a buffer. Inter frames still drop below.
            idx = awaitInputBuffer(KEYFRAME_WAIT_MS);
        }
        if (idx == null) {  // decoder behind — drop and keep flowing
            diagDrops++;
            if (!keyframe) diagInterDrops++;
            maybeLogDiag();
            // A dropped keyframe leaves us with no resync point; ask the
            // streamer for a fresh one (RtpReceiver throttles these) rather
            // than riding out corruption until the next periodic IDR.
            if (keyframe && keyframeRequester != null) keyframeRequester.request();
            return;
        }
        try {
            ByteBuffer buf = codec.getInputBuffer(idx);
            int cap = buf.capacity();
            if (annexB.length > cap) {
                codec.queueInputBuffer(idx, 0, 0, ptsUsec, 0);
                return;
            }
            buf.clear();
            buf.put(annexB);
            int flags = keyframe ? MediaCodec.BUFFER_FLAG_KEY_FRAME : 0;
            codec.queueInputBuffer(idx, 0, annexB.length, ptsUsec, flags);
            if (keyframe) awaitingKeyframe = false;  // parameter sets now in
            diagFed++;
            maybeLogDiag();
        } catch (Exception e) {
            Log.w(TAG, "feed failed: " + e.getMessage());
        }
    }

    /** Wire the transport used to request a fresh keyframe on a dropped IDR. */
    public void setKeyframeRequester(KeyframeRequester requester) {
        this.keyframeRequester = requester;
    }

    /**
     * Poll for a free input buffer for up to {@code maxWaitMs}, returning null if
     * none frees up in time (or we're torn down). Runs on the receive thread; the
     * free-buffer callback fires on the codec's own thread, so this can't deadlock.
     */
    private Integer awaitInputBuffer(long maxWaitMs) {
        long deadline = SystemClock.uptimeMillis() + maxWaitMs;
        Integer idx;
        while ((idx = freeInputs.poll()) == null) {
            if (released || codec == null) return null;
            if (SystemClock.uptimeMillis() >= deadline) return null;
            try {
                Thread.sleep(2);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return null;
            }
        }
        return idx;
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
