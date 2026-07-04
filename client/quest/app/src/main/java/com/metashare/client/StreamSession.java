package com.metashare.client;

import android.content.Context;
import android.net.wifi.WifiManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceView;

import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.Collections;

/**
 * Streams one monitor from a MetaShare streamer over raw RTP (H.265/H.264 video
 * + Opus audio). A tiny TCP signaling protocol bootstraps the session:
 *
 * <pre>
 *   server -> client: HELLO {json stream params}
 *   client -> server: READY {"port": <client udp port>}
 *   server -> client: START
 *   ... raw RTP over UDP ...
 *   either        : BYE
 * </pre>
 *
 * <p>Discovery (UDP broadcast) is unchanged: probe, receive an offer with the
 * host's signaling port, connect, then render.
 *
 * <p>Video decoding is native MediaCodec (hardware H.265 on Quest 3) rendering
 * straight to the supplied {@link SurfaceView}'s Surface — no libwebrtc, no EGL.
 */
public final class StreamSession {

    private static final String TAG = "StreamSession";

    private static final byte[] DISCOVERY_MAGIC =
            new byte[] {'M', 'S', 'H', 'A', 'R', 'E', 'D', '1'};
    private static final int DISCOVERY_PORT = 7777;
    private static final int PROTOCOL_VERSION = 4;

    /** Callbacks delivered on the main thread. */
    public interface Listener {
        void onStatus(String text);
        void onStreamSize(int width, int height);
    }

    private final Context context;
    private volatile SurfaceView surfaceView;
    private final int monitorIndex;
    private final Listener listener;
    private final Handler main = new Handler(Looper.getMainLooper());

    private volatile boolean running;
    private Thread worker;
    private DatagramSocket discoverySocket;
    private Socket signalingSocket;

    public StreamSession(Context context, SurfaceView surfaceView,
                         int monitorIndex, Listener listener) {
        this.context = context;
        this.surfaceView = surfaceView;
        this.monitorIndex = monitorIndex;
        this.listener = listener;
    }

    /** Swap the render target after a surface recreation. */
    public void setRenderer(SurfaceView surfaceView) {
        this.surfaceView = surfaceView;
    }

    public synchronized void start() {
        if (running) return;
        running = true;
        worker = new Thread(this::runLoop, "MetaShareStream");
        worker.start();
    }

    public synchronized void stop() {
        running = false;
        closeDiscoverySocket();
        closeSignalingSocket();
        if (worker != null) {
            worker.interrupt();
            worker = null;
        }
    }

    // ------------------------------------------------------------------ loop

    private void runLoop() {
        WifiManager.MulticastLock multicastLock = null;
        try {
            WifiManager wifi =
                    (WifiManager) context.getSystemService(Context.WIFI_SERVICE);
            if (wifi != null) {
                multicastLock = wifi.createMulticastLock("metashare-discovery");
                multicastLock.setReferenceCounted(false);
                multicastLock.acquire();
            }

            while (running) {
                try {
                    postStatus("Searching for MetaShare streamer...");
                    InetSocketAddress offer = discover(3000);
                    if (offer == null) {
                        sleepQuietly(1000);
                        continue;
                    }
                    postStatus("Connecting to " + offer.getAddress().getHostAddress()
                               + "...");
                    negotiateAndRender(offer);
                } catch (Exception e) {
                    Log.e(TAG, "stream loop failed", e);
                    if (running) {
                        postStatus("Disconnected. Reconnecting...");
                        sleepQuietly(1200);
                    }
                } finally {
                    closeSignalingSocket();
                }
            }
        } finally {
            if (multicastLock != null && multicastLock.isHeld()) {
                multicastLock.release();
            }
        }
    }

    // ------------------------------------------------------------- discovery

    private InetSocketAddress discover(int timeoutMs) throws IOException {
        DatagramSocket socket = new DatagramSocket();
        discoverySocket = socket;
        try {
            socket.setBroadcast(true);
            socket.setSoTimeout(timeoutMs);

            byte[] probe = ByteBuffer.allocate(16)
                    .order(ByteOrder.LITTLE_ENDIAN)
                    .put(DISCOVERY_MAGIC)
                    .putInt(PROTOCOL_VERSION)
                    .putInt(0)  // reserved client_caps (v4 ignores codec bits)
                    .array();

            sendProbe(socket, probe, InetAddress.getByName("255.255.255.255"));
            sendProbe(socket, probe, InetAddress.getByName("127.0.0.1"));
            for (NetworkInterface nif :
                    Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!nif.isUp() || nif.isLoopback()) continue;
                for (InterfaceAddress addr : nif.getInterfaceAddresses()) {
                    InetAddress broadcast = addr.getBroadcast();
                    if (broadcast instanceof Inet4Address) {
                        sendProbe(socket, probe, broadcast);
                    }
                }
            }

            byte[] offer = new byte[80];
            DatagramPacket packet = new DatagramPacket(offer, offer.length);
            while (running) {
                try {
                    socket.receive(packet);
                } catch (SocketTimeoutException timeout) {
                    return null;
                }
                if (packet.getLength() < offer.length) continue;
                if (!startsWith(offer, DISCOVERY_MAGIC)) continue;

                ByteBuffer buf = ByteBuffer.wrap(offer).order(ByteOrder.LITTLE_ENDIAN);
                buf.position(8);
                int version = buf.getInt();
                int port = Short.toUnsignedInt(buf.getShort());
                int monitorCount = Short.toUnsignedInt(buf.getShort());
                if (version == PROTOCOL_VERSION && port > 0) {
                    Log.i(TAG, "discovered streamer at "
                            + packet.getAddress().getHostAddress()
                            + ":" + port + " signaling ("
                            + monitorCount + " monitor"
                            + (monitorCount != 1 ? "s" : "") + ")");
                    return new InetSocketAddress(packet.getAddress(), port);
                }
            }
            return null;
        } finally {
            closeDiscoverySocket();
        }
    }

    private static void sendProbe(DatagramSocket socket, byte[] probe,
                                  InetAddress address) {
        try {
            DatagramPacket packet = new DatagramPacket(
                    probe, probe.length, address, DISCOVERY_PORT);
            socket.send(packet);
        } catch (IOException ignored) {
        }
    }

    // --------------------------------------------------------- RTP negotiation

    private void negotiateAndRender(InetSocketAddress offer) throws Exception {
        int targetPort = offer.getPort() + monitorIndex;
        Log.i(TAG, "monitor " + monitorIndex + " -> signaling port " + targetPort);

        Socket socket = new Socket();
        signalingSocket = socket;
        socket.setTcpNoDelay(true);
        socket.connect(new InetSocketAddress(offer.getAddress(), targetPort), 3000);
        // Don't hang forever if the server has our connection queued behind a
        // dead one — a silent handshake is treated as a failure and retried.
        socket.setSoTimeout(5000);

        BufferedReader sigIn = new BufferedReader(
                new InputStreamReader(socket.getInputStream(),
                                      StandardCharsets.US_ASCII));
        OutputStream sigOut = socket.getOutputStream();

        // 1. Read HELLO with stream params.
        String helloLine = sigIn.readLine();
        if (helloLine == null || !helloLine.startsWith("HELLO "))
            throw new IOException("expected HELLO, got: " + helloLine);
        JSONObject hello = new JSONObject(helloLine.substring("HELLO ".length()));
        JSONObject video = hello.getJSONObject("video");
        final String vCodec = video.getString("codec");
        final int vWidth = video.getInt("width");
        final int vHeight = video.getInt("height");
        final int vSsrc = video.getInt("ssrc");
        final int vPt = video.getInt("pt");
        final int vClock = video.optInt("clock", 90000);

        final int aSsrc;
        final int aPt;
        final int aClock;
        final int aRate;
        final int aChannels;
        boolean haveAudio = hello.has("audio");
        if (haveAudio) {
            JSONObject audio = hello.getJSONObject("audio");
            aSsrc = audio.getInt("ssrc");
            aPt = audio.getInt("pt");
            aClock = audio.optInt("clock", 48000);
            aRate = audio.optInt("rate", 48000);
            aChannels = audio.optInt("channels", 2);
        } else {
            aSsrc = -1; aPt = -1; aClock = 48000; aRate = 48000; aChannels = 2;
        }

        // 2. Set up decoders.
        final SurfaceView sv = surfaceView;
        final VideoDecoder videoDecoder = new VideoDecoder();
        VideoDecoder.Listener vListener = new VideoDecoder.Listener() {
            @Override public void onFirstFrameRendered() {
                postStatus("");  // hide the status overlay
            }
            @Override public void onFrameResolutionChanged(int w, int h) {
                postSize(w, h);
            }
        };
        Surface surface = (sv != null) ? sv.getHolder().getSurface() : null;
        if (surface != null && surface.isValid()) {
            try {
                videoDecoder.init(surface, vCodec, vWidth, vHeight, vListener);
            } catch (Exception e) {
                Log.e(TAG, "video decoder init failed: " + e.getMessage());
                throw e;
            }
        } else {
            Log.w(TAG, "surface not ready — video disabled for this session");
        }

        final AudioDecoder audioDecoder;
        if (haveAudio) {
            AudioDecoder ad = new AudioDecoder();
            boolean audioOk = false;
            try {
                ad.init(aRate, aChannels);
                audioOk = true;
            } catch (Exception e) {
                Log.w(TAG, "audio decoder init failed (audio disabled): "
                        + e.getMessage());
                ad = null;
            }
            audioDecoder = audioOk ? ad : null;
        } else {
            audioDecoder = null;
        }

        // 3. Bring up the RTP receiver on an ephemeral port.
        final RtpReceiver rtp = new RtpReceiver(
                (annexB, ptsUsec, keyframe) ->
                        videoDecoder.feed(annexB, ptsUsec, keyframe),
                (haveAudio && audioDecoder != null)
                        ? (data, off, len, ptsUsec) -> audioDecoder.feed(data, off, len, ptsUsec)
                        : null);
        rtp.configure(vSsrc, vPt, vCodec, vClock, aSsrc, aPt, aClock);
        // Let the decoder ask for a fresh IDR (throttled) if it is ever forced
        // to drop one under input-buffer starvation.
        videoDecoder.setKeyframeRequester(rtp::requestKeyframe);
        rtp.start(0);
        int udpPort = rtp.getLocalPort();
        Log.i(TAG, "RTP receiver on udp/" + udpPort);

        // 4. Tell the streamer where to send media, then wait for START.
        sendLine(sigOut, "READY {\"port\":" + udpPort + "}");
        String startLine = sigIn.readLine();
        if (startLine == null || !startLine.equals("START"))
            throw new IOException("expected START, got: " + startLine);

        postStatus("Streaming");

        // Ask for an immediate keyframe so decode starts without waiting up to
        // the encoder's full keyframe interval for the first IDR.
        rtp.requestKeyframe();

        // 5. Block until BYE / disconnect. Media flows on the RTP thread.
        // Keepalive: ping the streamer every read-timeout so it can tell a
        // live-but-idle client from a vanished one (its recv times out at 10s).
        socket.setSoTimeout(3000);
        try {
            while (running) {
                String line;
                try {
                    line = sigIn.readLine();
                } catch (SocketTimeoutException idle) {
                    sendLine(sigOut, "PING");  // throws when the link is dead
                    continue;
                }
                if (line == null) break;
                if (line.equals("BYE")) break;
            }
        } finally {
            try { rtp.stop(); } catch (Exception ignored) {}
            try { videoDecoder.release(); } catch (Exception ignored) {}
            if (audioDecoder != null) {
                try { audioDecoder.release(); } catch (Exception ignored) {}
            }
        }
    }

    // --------------------------------------------------------------- helpers

    private static void sendLine(OutputStream os, String line) throws IOException {
        os.write((line + "\n").getBytes(StandardCharsets.US_ASCII));
        os.flush();
    }

    private void postStatus(String text) {
        main.post(() -> listener.onStatus(text));
    }

    private void postSize(int w, int h) {
        main.post(() -> listener.onStreamSize(w, h));
    }

    private static boolean startsWith(byte[] data, byte[] magic) {
        if (data.length < magic.length) return false;
        for (int i = 0; i < magic.length; i++) {
            if (data[i] != magic[i]) return false;
        }
        return true;
    }

    private static void sleepQuietly(long millis) {
        try { Thread.sleep(millis); } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        }
    }

    private void closeDiscoverySocket() {
        DatagramSocket s = discoverySocket;
        discoverySocket = null;
        if (s != null) try { s.close(); } catch (Exception ignored) {}
    }

    private void closeSignalingSocket() {
        Socket s = signalingSocket;
        signalingSocket = null;
        if (s != null) try { s.close(); } catch (IOException ignored) {}
    }
}
