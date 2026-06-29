package com.metashare.client;

import android.os.SystemClock;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.SocketTimeoutException;
import java.util.Map;
import java.util.TreeMap;

/**
 * Receives raw RTP (H.265/H.264 video + Opus audio) over UDP, demultiplexes by
 * SSRC, reassembles Annex B video frames and hands them to a {@link VideoSink};
 * Opus packets go straight to an {@link AudioSink}.
 *
 * <p>Reliability:
 * <ul>
 *   <li><b>Jitter buffer</b> — video RTP packets are briefly buffered and
 *       delivered to the depacketizer in sequence-number order, so a reordered
 *       fragment can't corrupt a NAL reassembly.
 *   <li><b>NACK</b> — detected sequence gaps trigger RTCP NACKs back at the
 *       streamer, which retransmits from its sliding window.
 *   <li><b>PLI</b> — {@link #requestKeyframe()} sends a Picture Loss
 *       Indication; large gaps also auto-fire a throttled PLI.
 * </ul>
 */
public final class RtpReceiver {

    private static final String TAG = "RtpReceiver";

    /** Complete Annex B access unit (start codes + NALs). */
    public interface VideoSink {
        void onFrame(byte[] annexB, long ptsUsec, boolean keyframe);
    }

    /** One Opus packet (RTP payload). */
    public interface AudioSink {
        void onOpusPacket(byte[] data, int offset, int length, long ptsUsec);
    }

    private static final byte[] START_CODE = {0, 0, 0, 1};
    private static final int JITTER_MAX = 32;  // packets held for reordering

    private final VideoSink videoSink;
    private final AudioSink audioSink;

    // Stream params (set from HELLO before start()).
    private int videoSsrc = -1;
    private int videoPt = 96;
    private String videoCodec = "h265";  // "h265" or "h264"
    private int videoClockRate = 90000;
    private int audioSsrc = -1;
    private int audioPt = 111;
    private int audioClockRate = 48000;

    private DatagramSocket socket;
    private int localPort = -1;
    private volatile boolean running;
    private Thread thread;

    // Remote endpoint, learned from the first RTP packet (where NACKs/PLI go).
    private volatile InetAddress remoteAddress;
    private volatile int remotePort = -1;

    public RtpReceiver(VideoSink videoSink, AudioSink audioSink) {
        this.videoSink = videoSink;
        this.audioSink = audioSink;
    }

    public void configure(int videoSsrc, int videoPt, String videoCodec,
                          int videoClockRate, int audioSsrc, int audioPt,
                          int audioClockRate) {
        this.videoSsrc = videoSsrc;
        this.videoPt = videoPt;
        this.videoCodec = videoCodec;
        this.videoClockRate = videoClockRate;
        this.audioSsrc = audioSsrc;
        this.audioPt = audioPt;
        this.audioClockRate = audioClockRate;
    }

    /** Bind to localPort (0 = ephemeral) and start receiving. */
    public void start(int localPort) throws Exception {
        socket = new DatagramSocket(null);
        socket.setReuseAddress(true);
        socket.bind(new InetSocketAddress(localPort));
        socket.setSoTimeout(200);
        // Large frames (4K) produce 100+ RTP packet bursts; the default
        // ~208 KB kernel buffer overflows during these bursts.
        socket.setReceiveBufferSize(4 * 1024 * 1024);
        this.localPort = socket.getLocalPort();
        running = true;
        thread = new Thread(this::loop, "RtpReceiver");
        thread.start();
    }

    public int getLocalPort() {
        return localPort;
    }

    public void stop() {
        running = false;
        if (socket != null) {
            socket.close();
            socket = null;
        }
        if (thread != null) {
            thread.interrupt();
            thread = null;
        }
    }

    /** Send a PLI so the streamer emits a keyframe (e.g. right after START). */
    public void requestKeyframe() {
        sendPli();
    }

    // ------------------------------------------------------------------- loop

    private void loop() {
        VideoDepacketizer depack = new VideoDepacketizer(videoCodec);
        // Jitter buffer: seq -> held video packet, drained in seq order.
        TreeMap<Integer, HeldVideo> jitter = new TreeMap<>();
        int nextSeq = -1;        // next sequence number to deliver
        long lastAutoPli = 0;    // throttle auto-PLI on big gaps
        byte[] buf = new byte[65536];
        DatagramPacket pkt = new DatagramPacket(buf, buf.length);

        while (running) {
            int n;
            try {
                socket.receive(pkt);
                n = pkt.getLength();
            } catch (SocketTimeoutException ste) {
                continue;
            } catch (Exception e) {
                if (!running) break;
                Log.w(TAG, "receive error: " + e.getMessage());
                continue;
            }
            if (n < 12) continue;

            if (remoteAddress == null) {
                remoteAddress = pkt.getAddress();
                remotePort = pkt.getPort();
            }

            byte[] data = pkt.getData();
            int off = pkt.getOffset();
            if (((data[off] >>> 6) & 0x3) != 2) continue;
            boolean marker = (data[off + 1] & 0x80) != 0;
            int pt = data[off + 1] & 0x7F;
            int seq = ((data[off + 2] & 0xFF) << 8) | (data[off + 3] & 0xFF);
            long ts = ((long) (data[off + 4] & 0xFF) << 24)
                    | ((long) (data[off + 5] & 0xFF) << 16)
                    | ((long) (data[off + 6] & 0xFF) << 8)
                    | (data[off + 7] & 0xFF);
            int ssrc = ((data[off + 8] & 0xFF) << 24)
                    | ((data[off + 9] & 0xFF) << 16)
                    | ((data[off + 10] & 0xFF) << 8)
                    | (data[off + 11] & 0xFF);

            int cc = data[off] & 0x0F;
            int payloadOff = off + 12 + cc * 4;
            if ((data[off] & 0x10) != 0 && payloadOff + 4 <= off + n) {
                int extWords = ((data[payloadOff + 2] & 0xFF) << 8)
                        | (data[payloadOff + 3] & 0xFF);
                payloadOff += 4 + extWords * 4;
            }
            int payloadLen = (off + n) - payloadOff;
            if (payloadLen <= 0) continue;

            if (ssrc == videoSsrc && pt == videoPt) {
                // Buffer a copy of the payload for in-order delivery.
                byte[] payload = new byte[payloadLen];
                System.arraycopy(data, payloadOff, payload, 0, payloadLen);
                jitter.put(seq, new HeldVideo(payload, payloadLen, marker, ts));

                // Drain in seq order. When the buffer overflows past a gap,
                // advance and send NACKs for the truly-lost packets.
                while (!jitter.isEmpty()) {
                    Map.Entry<Integer, HeldVideo> e = jitter.firstEntry();
                    int s = e.getKey();
                    boolean due = (nextSeq < 0) || (s == nextSeq) ||
                                  (jitter.size() > JITTER_MAX);
                    if (!due) break;
                    jitter.remove(s);

                    if (nextSeq >= 0 && s != nextSeq) {
                        // Gap overflowed the jitter window — these packets
                        // are truly lost (not just reordered).
                        int gap = (s - nextSeq) & 0xFFFF;
                        sendNacksForGap(nextSeq, s);
                        if (gap > JITTER_MAX) {
                            long now = SystemClock.elapsedRealtime();
                            if (now - lastAutoPli > 500) {
                                lastAutoPli = now;
                                sendPli();
                            }
                        }
                    }

                    HeldVideo hv = e.getValue();
                    if (nextSeq < 0) nextSeq = s;
                    nextSeq = (s + 1) & 0xFFFF;
                    long ptsUsec = hv.ts * 1_000_000L / videoClockRate;
                    depack.feed(hv.payload, 0, hv.len, hv.marker, ptsUsec,
                                videoSink);
                }
            } else if (ssrc == audioSsrc && pt == audioPt) {
                long ptsUsec = ts * 1_000_000L / audioClockRate;
                audioSink.onOpusPacket(data, payloadOff, payloadLen, ptsUsec);
            }
        }
    }

    private static final class HeldVideo {
        final byte[] payload;
        final int len;
        final boolean marker;
        final long ts;
        HeldVideo(byte[] payload, int len, boolean marker, long ts) {
            this.payload = payload;
            this.len = len;
            this.marker = marker;
            this.ts = ts;
        }
    }

    // ------------------------------------------------------------------- NACK

    private void sendNacksForGap(int firstMissing, int afterGap) {
        InetAddress addr = remoteAddress;
        int port = remotePort;
        if (addr == null || port < 0 || socket == null) return;
        int mediaSsrc = videoSsrc;
        // Pack missing seqs into NACK FCI entries using the Bitmap Loss Pattern
        // (BLP). Each entry covers PID + up to 16 following seqs, drastically
        // reducing the number of RTCP packets vs one-per-seq.
        int pid = firstMissing;
        while (pid != afterGap) {
            int blp = 0;
            int count = 0;
            for (int i = 0; i < 16; i++) {
                int s = (pid + 1 + i) & 0xFFFF;
                if (s == afterGap) break;
                blp |= (1 << i);
                count++;
            }
            byte[] nack = new byte[16];
            nack[0] = (byte) 0x81;  // V=2, P=0, FMT=1 (NACK)
            nack[1] = (byte) 205;  // PT=RTPFB
            nack[2] = 0x00;        // length = 3 (4 words - 1)
            nack[3] = 0x03;
            nack[8] = (byte) (mediaSsrc >>> 24);
            nack[9] = (byte) (mediaSsrc >>> 16);
            nack[10] = (byte) (mediaSsrc >>> 8);
            nack[11] = (byte) mediaSsrc;
            nack[12] = (byte) (pid >>> 8);
            nack[13] = (byte) pid;
            nack[14] = (byte) (blp >>> 8);
            nack[15] = (byte) blp;
            try {
                socket.send(new DatagramPacket(nack, nack.length, addr, port));
            } catch (Exception e) {
                break;
            }
            pid = (pid + 1 + count) & 0xFFFF;
        }
    }

    // -------------------------------------------------------------------- PLI

    private void sendPli() {
        InetAddress addr = remoteAddress;
        int port = remotePort;
        if (addr == null || port < 0 || socket == null || videoSsrc < 0) return;
        byte[] pli = new byte[12];
        pli[0] = (byte) 0x81;  // V=2, P=0, FMT=1
        pli[1] = (byte) 206;  // PT=PSFB
        pli[2] = 0x00;        // length = 2 (3 words - 1)
        pli[3] = 0x02;
        // bytes 4..7 sender SSRC = 0 (ignored by server)
        pli[8] = (byte) (videoSsrc >>> 24);
        pli[9] = (byte) (videoSsrc >>> 16);
        pli[10] = (byte) (videoSsrc >>> 8);
        pli[11] = (byte) videoSsrc;
        try {
            socket.send(new DatagramPacket(pli, pli.length, addr, port));
        } catch (Exception e) {
            Log.w(TAG, "PLI send failed: " + e.getMessage());
        }
    }

    // -------------------------------------------------------- video depacket.

    /**
     * Reassembles H.265 (RFC 7798) or H.264 (RFC 6184) RTP payloads into Annex
     * B access units, emitting one complete frame per marker bit.
     */
    private static final class VideoDepacketizer {
        private final boolean h265;
        private final ByteArrayOutputStream frame = new ByteArrayOutputStream(64 * 1024);
        private final ByteArrayOutputStream fu = new ByteArrayOutputStream(16 * 1024);
        private boolean fuActive = false;
        private boolean keyframe = false;

        VideoDepacketizer(String codec) {
            h265 = "h265".equalsIgnoreCase(codec);
        }

        void feed(byte[] data, int off, int len, boolean marker,
                  long ptsUsec, VideoSink sink) {
            if (len < 1) return;
            int firstByte = data[off] & 0xFF;
            if (h265) {
                int naluType = (firstByte >>> 1) & 0x3F;
                if (naluType < 48) {
                    if (fuActive) fuActive = false;  // abandon incomplete FU
                    addNal(data, off, len);
                    if (naluType == 19 || naluType == 20) keyframe = true;
                } else if (naluType == 48) {  // AP
                    if (fuActive) fuActive = false;
                    int i = off + 2;
                    while (i + 2 <= off + len) {
                        int nlen = ((data[i] & 0xFF) << 8) | (data[i + 1] & 0xFF);
                        i += 2;
                        if (nlen == 0 || i + nlen > off + len) break;
                        addNal(data, i, nlen);
                        i += nlen;
                    }
                } else if (naluType == 49) {  // FU
                    if (len < 3) return;
                    boolean s = (data[off + 2] & 0x80) != 0;
                    boolean e = (data[off + 2] & 0x40) != 0;
                    int origType = data[off + 2] & 0x3F;
                    if (s) {
                        fu.reset();
                        fu.write(START_CODE, 0, 4);
                        byte h0 = (byte) ((firstByte & 0x81) | (origType << 1));
                        fu.write(h0);
                        fu.write(data[off + 1]);
                        if (origType == 19 || origType == 20) keyframe = true;
                        fuActive = true;
                    }
                    if (fuActive) {
                        fu.write(data, off + 3, len - 3);
                        if (e) {
                            byte[] nalu = fu.toByteArray();
                            frame.write(nalu, 0, nalu.length);
                            fuActive = false;
                        }
                    }
                }
            } else {
                int naluType = firstByte & 0x1F;
                if (naluType >= 1 && naluType <= 23) {
                    if (fuActive) fuActive = false;  // abandon incomplete FU
                    addNal(data, off, len);
                    if (naluType == 5) keyframe = true;
                } else if (naluType == 24) {  // STAP-A
                    if (fuActive) fuActive = false;
                    int i = off + 1;
                    while (i + 2 <= off + len) {
                        int nlen = ((data[i] & 0xFF) << 8) | (data[i + 1] & 0xFF);
                        i += 2;
                        if (nlen == 0 || i + nlen > off + len) break;
                        addNal(data, i, nlen);
                        i += nlen;
                    }
                } else if (naluType == 28) {  // FU-A
                    if (len < 2) return;
                    boolean s = (data[off + 1] & 0x80) != 0;
                    boolean e = (data[off + 1] & 0x40) != 0;
                    int origType = data[off + 1] & 0x1F;
                    if (s) {
                        fu.reset();
                        fu.write(START_CODE, 0, 4);
                        byte ind = (byte) ((firstByte & 0xE0) | origType);
                        fu.write(ind);
                        if (origType == 5) keyframe = true;
                        fuActive = true;
                    }
                    if (fuActive) {
                        fu.write(data, off + 2, len - 2);
                        if (e) {
                            byte[] nalu = fu.toByteArray();
                            frame.write(nalu, 0, nalu.length);
                            fuActive = false;
                        }
                    }
                }
            }

            if (marker) {
                fuActive = false;  // frame boundary — abandon any partial FU
                byte[] annexB = frame.toByteArray();
                boolean kf = keyframe;
                frame.reset();
                keyframe = false;
                if (annexB.length > 0) {
                    sink.onFrame(annexB, ptsUsec, kf);
                }
            }
        }

        private void addNal(byte[] data, int off, int len) {
            frame.write(START_CODE, 0, 4);
            frame.write(data, off, len);
        }
    }
}
