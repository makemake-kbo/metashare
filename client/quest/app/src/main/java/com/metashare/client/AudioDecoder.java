package com.metashare.client;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;

import java.nio.ByteBuffer;
import java.util.concurrent.ConcurrentLinkedQueue;

/**
 * Opus decoder backed by {@link MediaCodec}, writing decoded PCM to an
 * {@link AudioTrack} for immediate playback. RFC 7587 Opus-in-RTP carries raw
 * Opus packets with no container headers, so we synthesize the 19-byte OpusHead
 * ("OpusHead" + version/channels/pre-skip/sample-rate/gain/mapping) the MediaCodec
 * Opus decoder expects as {@code csd-0}.
 */
public final class AudioDecoder {

    private static final String TAG = "AudioDecoder";

    private MediaCodec codec;
    private HandlerThread cbThread;
    private AudioTrack track;
    private final ConcurrentLinkedQueue<Integer> freeInputs = new ConcurrentLinkedQueue<>();
    private volatile boolean released = false;

    public void init(int sampleRate, int channels) throws Exception {
        MediaFormat fmt = MediaFormat.createAudioFormat(
                MediaFormat.MIMETYPE_AUDIO_OPUS, sampleRate, channels);
        fmt.setByteBuffer("csd-0", ByteBuffer.wrap(opusHead(channels, sampleRate)));

        codec = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_AUDIO_OPUS);
        cbThread = new HandlerThread("AudioDecoderCb");
        cbThread.start();
        codec.setCallback(callback, new Handler(cbThread.getLooper()));
        codec.configure(fmt, null, null, 0);
        codec.start();

        int channelMask = (channels >= 2)
                ? AudioFormat.CHANNEL_OUT_STEREO
                : AudioFormat.CHANNEL_OUT_MONO;
        int minBuf = AudioTrack.getMinBufferSize(sampleRate, channelMask,
                AudioFormat.ENCODING_PCM_16BIT);
        int bufSize = Math.max(minBuf * 2, 8192);
        track = new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                        .build())
                .setAudioFormat(new AudioFormat.Builder()
                        .setSampleRate(sampleRate)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setChannelMask(channelMask)
                        .build())
                .setBufferSizeInBytes(bufSize)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .setSessionId(AudioManager.AUDIO_SESSION_ID_GENERATE)
                .build();
        track.play();
        Log.i(TAG, "opened opus " + sampleRate + "Hz " + channels + "ch");
    }

    private final MediaCodec.Callback callback = new MediaCodec.Callback() {
        @Override
        public void onInputBufferAvailable(MediaCodec c, int index) {
            freeInputs.add(index);
        }

        @Override
        public void onOutputBufferAvailable(MediaCodec c, int index,
                                            MediaCodec.BufferInfo info) {
            if (info.size > 0) {
                try {
                    ByteBuffer out = c.getOutputBuffer(index);
                    out.position(info.offset);
                    out.limit(info.offset + info.size);
                    // Blocking write paces the decoder to real-time playback.
                    track.write(out, info.size, AudioTrack.WRITE_BLOCKING);
                } catch (Exception e) {
                    Log.w(TAG, "audio write failed: " + e.getMessage());
                }
            }
            try {
                c.releaseOutputBuffer(index, false);
            } catch (Exception ignored) {
            }
        }

        @Override
        public void onOutputFormatChanged(MediaCodec c, MediaFormat format) {
            Log.i(TAG, "output format: " + format);
        }

        @Override
        public void onError(MediaCodec c, MediaCodec.CodecException e) {
            Log.e(TAG, "codec error: " + e.getErrorCode() + " " + e.getMessage());
        }
    };

    public void feed(byte[] data, int offset, int length, long ptsUsec) {
        if (released || codec == null) return;
        Integer idx = freeInputs.poll();
        if (idx == null) return;
        try {
            ByteBuffer buf = codec.getInputBuffer(idx);
            buf.clear();
            buf.put(data, offset, length);
            codec.queueInputBuffer(idx, 0, length, ptsUsec, 0);
        } catch (Exception e) {
            Log.w(TAG, "feed failed: " + e.getMessage());
        }
    }

    public void release() {
        released = true;
        freeInputs.clear();
        if (track != null) {
            try {
                track.stop();
            } catch (Exception ignored) {
            }
            track.release();
            track = null;
        }
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
        if (cbThread != null) {
            cbThread.quitSafely();
            cbThread = null;
        }
    }

    /** 19-byte OpusHead identification header (channel mapping family 0). */
    private static byte[] opusHead(int channels, int sampleRate) {
        byte[] h = new byte[19];
        h[0] = 'O'; h[1] = 'p'; h[2] = 'u'; h[3] = 's';
        h[4] = 'H'; h[5] = 'e'; h[6] = 'a'; h[7] = 'd';
        h[8] = 1;                              // version
        h[9] = (byte) channels;               // channel count
        h[10] = 0; h[11] = 0;                 // pre-skip (LE) = 0
        h[12] = (byte) sampleRate;            // sample rate (LE)
        h[13] = (byte) (sampleRate >>> 8);
        h[14] = (byte) (sampleRate >>> 16);
        h[15] = (byte) (sampleRate >>> 24);
        h[16] = 0; h[17] = 0;                 // output gain (LE) = 0
        h[18] = 0;                            // channel mapping family
        return h;
    }
}
