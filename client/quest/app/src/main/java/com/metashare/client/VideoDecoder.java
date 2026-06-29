package com.metashare.client;

import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Handler;
import android.os.HandlerThread;
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

    private MediaCodec codec;
    private HandlerThread callbackThread;
    private final ConcurrentLinkedQueue<Integer> freeInputs = new ConcurrentLinkedQueue<>();
    private volatile boolean firstFrameDone = false;
    private volatile boolean released = false;
    private Listener listener;

    public void init(Surface surface, String codecName, int width, int height,
                     Listener listener) throws Exception {
        this.listener = listener;
        String mime = "h265".equalsIgnoreCase(codecName)
                ? MediaFormat.MIMETYPE_VIDEO_HEVC
                : MediaFormat.MIMETYPE_VIDEO_AVC;

        MediaFormat fmt = MediaFormat.createVideoFormat(mime,
                Math.max(1, width), Math.max(1, height));
        fmt.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 4 * 1024 * 1024);

        codec = MediaCodec.createDecoderByType(mime);
        callbackThread = new HandlerThread("VideoDecoderCb");
        callbackThread.start();
        codec.setCallback(callback, new Handler(callbackThread.getLooper()));
        codec.configure(fmt, surface, null, 0);
        codec.start();
        Log.i(TAG, "opened " + mime + " " + width + "x" + height);
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
                c.releaseOutputBuffer(index, /*render=*/true);
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
            Log.e(TAG, "codec error: " + e.getErrorCode() + " " + e.getMessage());
        }
    };

    /** Feed one Annex B access unit (pts in microseconds). */
    public void feed(byte[] annexB, long ptsUsec, boolean keyframe) {
        if (released || codec == null) return;
        Integer idx = freeInputs.poll();
        if (idx == null) return;  // no input buffer ready — drop rather than block
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
        } catch (Exception e) {
            Log.w(TAG, "feed failed: " + e.getMessage());
        }
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
