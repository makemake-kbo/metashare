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
    private static final int JITTER_MAX = 512;  // memory bound; time controls playout
    private static final long REPAIR_BUDGET_MS = 30;
    // Largest gap we try to repair with NACKs; anything bigger is hopeless
    // (e.g. a long stall) and cheaper to fix with a single PLI + keyframe.
    private static final int NACK_MAX_GAP = 256;
    private static final long RR_INTERVAL_MS = 1000;  // receiver report cadence

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
    // Keyframe request issued before the remote endpoint is known; serviced as
    // soon as the first RTP packet reveals where to send the PLI.
    private volatile boolean pliPending = false;
    // Throttle for externally-requested PLIs (e.g. one per dropped frame during
    // a decoder overload): without it a drop storm floods the streamer with
    // keyframe requests, each of which is a big IDR that makes the overload
    // worse. One in-flight request every 250 ms is plenty to drive recovery.
    private volatile long lastExternalPliMs = 0;
    private static final long EXTERNAL_PLI_MIN_INTERVAL_MS = 250;

    // Receiver-report statistics (video SSRC only; loop thread).
    private long statFirstExtSeq = -1;   // extended seq of first packet
    private long statReceived = 0;       // total video packets received
    private long statExpectedPrior = 0;  // snapshot at last RR
    private long statReceivedPrior = 0;
    private long lastRrMs = 0;

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
        socket.setSoTimeout(10);  // service gap expiry even when RTP goes idle
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
        long now = SystemClock.elapsedRealtime();
        if (now - lastExternalPliMs < EXTERNAL_PLI_MIN_INTERVAL_MS) return;
        lastExternalPliMs = now;
        if (remoteAddress == null) {
            // No RTP received yet, so we don't know the streamer's UDP source
            // endpoint. Defer; the receive loop fires the PLI on first packet.
            pliPending = true;
            return;
        }
        sendPli();
    }

    // ------------------------------------------------------------------- loop

    private void loop() {
        VideoDepacketizer depack = new VideoDepacketizer(videoCodec);
        // Jitter buffer keyed by *extended* (unwrapped, monotonically growing)
        // sequence number, so ordering survives the 16-bit wrap and stale
        // retransmissions can be recognised and dropped.
        TreeMap<Long, HeldVideo> jitter = new TreeMap<>();
        long nextExtSeq = -1;     // next extended seq to deliver
        long highestExtSeq = -1;  // highest extended seq received so far
        long lastAutoPli = 0;     // throttle auto-PLI
        byte[] buf = new byte[65536];
        DatagramPacket pkt = new DatagramPacket(buf, buf.length);

        while (running) {
            // Expire repairs on time, including during socket timeouts.
            while (!jitter.isEmpty()) {
                Map.Entry<Long, HeldVideo> e = jitter.firstEntry();
                long s = e.getKey();
                boolean due = (nextExtSeq < 0) || (s == nextExtSeq) ||
                              (jitter.size() > JITTER_MAX) ||
                              (SystemClock.elapsedRealtime() - e.getValue().receivedMs
                                      >= REPAIR_BUDGET_MS);
                if (!due) break;
                jitter.remove(s);

                if (nextExtSeq >= 0 && s != nextExtSeq) {
                    depack.onGap();
                    // Retransmission didn't make it in time; the current
                    // frame is damaged — ask for a keyframe (throttled).
                    long now = SystemClock.elapsedRealtime();
                    if (now - lastAutoPli > 500) {
                        lastAutoPli = now;
                        sendPli();
                    }
                }

                HeldVideo hv = e.getValue();
                nextExtSeq = s + 1;
                long ptsUsec = hv.ts * 1_000_000L / videoClockRate;
                depack.feed(hv.payload, 0, hv.len, hv.marker, ptsUsec,
                            videoSink);
                if (depack.needsKeyframe()) requestKeyframe();
            }

            int n;
            try {
                pkt.setLength(buf.length);
                socket.receive(pkt);
                n = pkt.getLength();
            } catch (SocketTimeoutException ste) {
                maybeSendReceiverReport(highestExtSeq);
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
                if (pliPending) {
                    pliPending = false;
                    sendPli();
                }
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
                // Unwrap the 16-bit seq into the extended sequence space,
                // interpreting it as the closest value to the highest seen
                // (handles both reordering and wrap).
                long ext;
                if (highestExtSeq < 0) {
                    ext = seq;
                } else {
                    ext = highestExtSeq
                            + (short) (seq - (int) (highestExtSeq & 0xFFFF));
                }
                statReceived++;

                if (ext > highestExtSeq) {
                    // New territory. NACK any gap *now*, while the missing
                    // packets are still ahead of the playout point, so
                    // retransmissions can actually be used.
                    if (highestExtSeq >= 0 && ext > highestExtSeq + 1) {
                        long gap = ext - highestExtSeq - 1;
                        if (gap <= NACK_MAX_GAP) {
                            sendNacksForRange(highestExtSeq + 1, ext);
                        } else {
                            // Hopelessly large gap — resync via keyframe.
                            long now = SystemClock.elapsedRealtime();
                            if (now - lastAutoPli > 500) {
                                lastAutoPli = now;
                                sendPli();
                            }
                        }
                    }
                    highestExtSeq = ext;
                    if (statFirstExtSeq < 0) statFirstExtSeq = ext;
                }

                if (nextExtSeq >= 0 && ext < nextExtSeq) {
                    // Behind the playout point (late retransmission or dup) —
                    // useless now; feeding it forward would corrupt the
                    // depacketizer and poison the jitter buffer. Drop it.
                    maybeSendReceiverReport(highestExtSeq);
                    continue;
                }

                if (!jitter.containsKey(ext)) {
                    byte[] payload = new byte[payloadLen];
                    System.arraycopy(data, payloadOff, payload, 0, payloadLen);
                    jitter.put(ext, new HeldVideo(payload, payloadLen, marker, ts));
                }

            } else if (ssrc == audioSsrc && pt == audioPt) {
                long ptsUsec = ts * 1_000_000L / audioClockRate;
                audioSink.onOpusPacket(data, payloadOff, payloadLen, ptsUsec);
            }

            maybeSendReceiverReport(highestExtSeq);
        }
    }

    private static final class HeldVideo {
        final byte[] payload;
        final int len;
        final boolean marker;
        final long ts;
        final long receivedMs = SystemClock.elapsedRealtime();
        HeldVideo(byte[] payload, int len, boolean marker, long ts) {
            this.payload = payload;
            this.len = len;
            this.marker = marker;
            this.ts = ts;
        }
    }

    // ------------------------------------------------------------------- NACK

    /** NACK the extended-seq range [firstMissing, afterGap). */
    private void sendNacksForRange(long firstMissing, long afterGap) {
        InetAddress addr = remoteAddress;
        int port = remotePort;
        if (addr == null || port < 0 || socket == null) return;
        int mediaSsrc = videoSsrc;
        // Pack missing seqs into NACK FCI entries using the Bitmap Loss Pattern
        // (BLP). Each entry covers PID + up to 16 following seqs, drastically
        // reducing the number of RTCP packets vs one-per-seq.
        long extPid = firstMissing;
        while (extPid < afterGap) {
            int pid = (int) (extPid & 0xFFFF);
            int blp = 0;
            int count = 0;
            for (int i = 0; i < 16; i++) {
                if (extPid + 1 + i >= afterGap) break;
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
            extPid += 1 + count;
        }
    }

    // ------------------------------------------------------- receiver reports

    /**
     * Send an RTCP Receiver Report (RFC 3550) for the video stream roughly
     * once per second. The streamer uses the fraction-lost field to adapt its
     * encoder bitrate to what the WiFi link can actually carry.
     */
    private void maybeSendReceiverReport(long highestExtSeq) {
        long now = SystemClock.elapsedRealtime();
        if (now - lastRrMs < RR_INTERVAL_MS) return;
        lastRrMs = now;

        InetAddress addr = remoteAddress;
        int port = remotePort;
        if (addr == null || port < 0 || socket == null) return;
        if (statFirstExtSeq < 0 || highestExtSeq < 0) return;

        long expected = highestExtSeq - statFirstExtSeq + 1;
        long expectedInterval = expected - statExpectedPrior;
        long receivedInterval = statReceived - statReceivedPrior;
        statExpectedPrior = expected;
        statReceivedPrior = statReceived;

        int fractionLost = 0;
        if (expectedInterval > 0) {
            long lost = expectedInterval - receivedInterval;
            if (lost > 0) {
                fractionLost = (int) Math.min(255, lost * 256 / expectedInterval);
            }
        }
        long cumLost = Math.max(0, Math.min(0x7FFFFF, expected - statReceived));

        byte[] rr = new byte[32];
        rr[0] = (byte) 0x81;  // V=2, P=0, RC=1
        rr[1] = (byte) 201;   // PT=RR
        rr[2] = 0x00;         // length = 7 words (32 bytes)
        rr[3] = 0x07;
        // bytes 4..7: reporter SSRC = 0 (server keys on the block's SSRC)
        // Report block:
        rr[8] = (byte) (videoSsrc >>> 24);
        rr[9] = (byte) (videoSsrc >>> 16);
        rr[10] = (byte) (videoSsrc >>> 8);
        rr[11] = (byte) videoSsrc;
        rr[12] = (byte) fractionLost;
        rr[13] = (byte) (cumLost >>> 16);
        rr[14] = (byte) (cumLost >>> 8);
        rr[15] = (byte) cumLost;
        int extHigh = (int) highestExtSeq;  // cycles<<16 | seq
        rr[16] = (byte) (extHigh >>> 24);
        rr[17] = (byte) (extHigh >>> 16);
        rr[18] = (byte) (extHigh >>> 8);
        rr[19] = (byte) extHigh;
        // bytes 20..23 interarrival jitter, 24..27 LSR, 28..31 DLSR: zero.
        try {
            socket.send(new DatagramPacket(rr, rr.length, addr, port));
        } catch (Exception e) {
            Log.w(TAG, "RR send failed: " + e.getMessage());
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
        private static final int MAX_FRAME_BYTES = 4 * 1024 * 1024;
        private final boolean h265;
        private final ByteArrayOutputStream frame = new ByteArrayOutputStream(64 * 1024);
        private boolean fuActive;
        private int fuType;
        private boolean keyframe;
        private boolean damaged;
        private boolean pendingGap;
        private boolean awaitingKeyframe;
        private long framePts = Long.MIN_VALUE;

        VideoDepacketizer(String codec) {
            h265 = "h265".equalsIgnoreCase(codec);
        }

        void onGap() {
            pendingGap = true;
            invalidate();
        }

        boolean needsKeyframe() { return awaitingKeyframe; }

        private void invalidate() {
            damaged = true;
            awaitingKeyframe = true;
            frame.reset();
            fuActive = false;
        }

        void feed(byte[] data, int off, int len, boolean marker,
                  long ptsUsec, VideoSink sink) {
            if (framePts != ptsUsec) {
                // A timestamp change without a marker leaves the previous AU
                // incomplete. Never join its fragments to the next frame.
                if (framePts != Long.MIN_VALUE) invalidate();
                frame.reset();
                fuActive = false;
                keyframe = false;
                damaged = pendingGap;
                framePts = ptsUsec;
            }
            if (pendingGap) damaged = true;
            pendingGap = false;
            if (!damaged) {
                int header = h265 ? 2 : 1;
                if (len < header || frame.size() + len + 4 > MAX_FRAME_BYTES) {
                    invalidate();
                } else {
                    int first = data[off] & 0xFF;
                    int type = h265 ? (first >>> 1) & 63 : first & 31;
                    int apType = h265 ? 48 : 24;
                    int fragType = h265 ? 49 : 28;
                    if (type == fragType) {
                        if (len <= header + 1) {
                            invalidate();
                        } else {
                            int fh = data[off + header] & 0xFF;
                            boolean start = (fh & 0x80) != 0;
                            boolean end = (fh & 0x40) != 0;
                            int original = fh & (h265 ? 63 : 31);
                            if ((start && (fuActive || end)) ||
                                    (!start && (!fuActive || original != fuType))) {
                                invalidate();
                            } else {
                                if (start) {
                                    frame.write(START_CODE, 0, 4);
                                    frame.write(h265 ? (first & 0x81) | (original << 1)
                                                     : (first & 0xE0) | original);
                                    if (h265) frame.write(data[off + 1]);
                                    keyframe |= isIdr(original);
                                    fuActive = true;
                                    fuType = original;
                                }
                                frame.write(data, off + header + 1, len - header - 1);
                                if (end) fuActive = false;
                            }
                        }
                    } else if (fuActive) {
                        invalidate();
                    } else if (type == apType) {
                        int i = off + header;
                        while (i < off + len && !damaged) {
                            if (i + 2 > off + len) { invalidate(); break; }
                            int nlen = ((data[i] & 0xFF) << 8) | (data[i + 1] & 0xFF);
                            i += 2;
                            if (nlen < header || i + nlen > off + len ||
                                    frame.size() + nlen + 4 > MAX_FRAME_BYTES) {
                                invalidate(); break;
                            }
                            addNal(data, i, nlen);
                            i += nlen;
                        }
                    } else if ((h265 && type < 48) || (!h265 && type >= 1 && type <= 23)) {
                        addNal(data, off, len);
                    } else {
                        invalidate();
                    }
                }
            }
            if (marker) {
                if (fuActive) invalidate();
                if (!damaged && frame.size() > 0 && (!awaitingKeyframe || keyframe)) {
                    awaitingKeyframe = false;
                    sink.onFrame(frame.toByteArray(), ptsUsec, keyframe);
                }
                frame.reset();
                fuActive = false;
                keyframe = false;
                damaged = false;
                framePts = Long.MIN_VALUE;
            }
        }

        private boolean isIdr(int type) {
            return h265 ? type == 19 || type == 20 : type == 5;
        }

        private void addNal(byte[] data, int off, int len) {
            int type = h265 ? (data[off] >>> 1) & 63 : data[off] & 31;
            keyframe |= isIdr(type);
            frame.write(START_CODE, 0, 4);
            frame.write(data, off, len);
        }
    }
}
