package com.metashare.client;

import android.content.Context;
import android.net.wifi.WifiManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import org.webrtc.DefaultVideoDecoderFactory;
import org.webrtc.EglBase;
import org.webrtc.IceCandidate;
import org.webrtc.MediaConstraints;
import org.webrtc.MediaStreamTrack;
import org.webrtc.PeerConnection;
import org.webrtc.PeerConnectionFactory;
import org.webrtc.RtpReceiver;
import org.webrtc.RtpTransceiver;
import org.webrtc.SdpObserver;
import org.webrtc.SessionDescription;
import org.webrtc.SurfaceViewRenderer;
import org.webrtc.VideoSink;
import org.webrtc.audio.JavaAudioDeviceModule;

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
import java.util.Base64;
import java.util.Collections;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * Streams one monitor from a MetaShare streamer over WebRTC (libdatachannel on
 * the server, libwebrtc on this side). SDP/ICE exchange happens over a tiny TCP
 * signaling protocol — newline-delimited base64-encoded SDP/candidate blobs.
 *
 * <p>Discovery (UDP) is unchanged from v1: broadcast probe, receive an offer
 * with the host's signaling port, connect, negotiate WebRTC, render.
 *
 * <p>Video decoding (HEVC on Quest 3) is handled by libwebrtc's
 * {@link HardwareVideoDecoderFactory}, rendered into the supplied
 * {@link SurfaceViewRenderer} (which is also a {@link VideoSink}).
 */
public final class StreamSession {

    private static final String TAG = "StreamSession";

    private static final byte[] DISCOVERY_MAGIC =
            new byte[] {'M', 'S', 'H', 'A', 'R', 'E', 'D', '1'};
    private static final int DISCOVERY_PORT = 7777;
    private static final int PROTOCOL_VERSION = 3;
    private static final int CAP_H264 = 1 << 0;
    private static final int CAP_H265 = 1 << 1;

    /** Callbacks delivered on the main thread. */
    public interface Listener {
        void onStatus(String text);
        void onStreamSize(int width, int height);
    }

    private final Context context;
    private volatile SurfaceViewRenderer renderer;
    private final int monitorIndex;
    private final Listener listener;
    private final Handler main = new Handler(Looper.getMainLooper());

    private volatile boolean running;
    private Thread worker;
    private DatagramSocket discoverySocket;
    private Socket signalingSocket;

    // libwebrtc state, created lazily once we actually need to negotiate.
    private PeerConnectionFactory factory;
    private EglBase eglBase;

    public StreamSession(Context context, SurfaceViewRenderer renderer,
                         int monitorIndex, Listener listener) {
        this.context = context;
        this.renderer = renderer;
        this.monitorIndex = monitorIndex;
        this.listener = listener;
    }

    /** Swap the render target after a surface recreation. */
    public void setRenderer(SurfaceViewRenderer renderer) {
        this.renderer = renderer;
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
                    .putInt(CAP_H264 | CAP_H265)
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

    // ----------------------------------------------------- WebRTC negotiation

    private void negotiateAndRender(InetSocketAddress offer) throws Exception {
        int targetPort = offer.getPort() + monitorIndex;
        Log.i(TAG, "monitor " + monitorIndex + " -> signaling port " + targetPort);

        // 1. Connect the signaling TCP channel and wait for OK.
        Socket socket = new Socket();
        signalingSocket = socket;
        socket.setTcpNoDelay(true);
        socket.connect(new InetSocketAddress(offer.getAddress(), targetPort), 3000);

        BufferedReader sigIn = new BufferedReader(
                new InputStreamReader(socket.getInputStream(),
                                      StandardCharsets.US_ASCII));
        OutputStream sigOut = socket.getOutputStream();

        String ok = sigIn.readLine();
        if (!"OK".equals(ok)) throw new IOException("expected OK, got: " + ok);

        ensureFactory();
        eglBase = EglBase.create();

        // 2. PeerConnectionFactory is built lazily; the per-session PC uses it.
        PeerConnection.RTCConfiguration rtcConfig =
                new PeerConnection.RTCConfiguration(Collections.emptyList());
        rtcConfig.iceTransportsType =
                PeerConnection.IceTransportsType.ALL;
        rtcConfig.continualGatheringPolicy =
                PeerConnection.ContinualGatheringPolicy.GATHER_CONTINUALLY;
        rtcConfig.sdpSemantics = PeerConnection.SdpSemantics.UNIFIED_PLAN;

        PeerConnection pc = factory.createPeerConnection(rtcConfig, pcObserver);
        if (pc == null) throw new IOException("createPeerConnection failed");

        // 3. Add RECVONLY transceivers — video (HEVC preferred) + audio (Opus).
        //    Using RTPTransceiverInit lets us force RECVONLY direction.
        RtpTransceiver.RtpTransceiverInit recvOnly =
                new RtpTransceiver.RtpTransceiverInit(
                        RtpTransceiver.RtpTransceiverDirection.RECV_ONLY);
        pc.addTransceiver(MediaStreamTrack.MediaType.MEDIA_TYPE_VIDEO,
                          recvOnly);
        pc.addTransceiver(MediaStreamTrack.MediaType.MEDIA_TYPE_AUDIO,
                          recvOnly);

        // 4. Create an SDP offer, send it via signaling.
        SessionDescription[] offerHolder = new SessionDescription[1];
        CountDownLatch offerLatch = new CountDownLatch(1);
        pc.createOffer(new SdpObserver() {
            @Override public void onCreateSuccess(SessionDescription sdp) {
                offerHolder[0] = sdp; offerLatch.countDown();
            }
            @Override public void onSetSuccess() {}
            @Override public void onCreateFailure(String s) {
                Log.e(TAG, "createOffer failed: " + s); offerLatch.countDown();
            }
            @Override public void onSetFailure(String s) {}
        }, new MediaConstraints());

        if (!offerLatch.await(5, TimeUnit.SECONDS) || offerHolder[0] == null)
            throw new IOException("createOffer timeout");

        // Set the local description; libwebrtc fires the local sdp via observer.
        CountDownLatch setLocalLatch = new CountDownLatch(1);
        pc.setLocalDescription(new SdpObserver() {
            @Override public void onCreateSuccess(SessionDescription sdp) {}
            @Override public void onSetSuccess() { setLocalLatch.countDown(); }
            @Override public void onCreateFailure(String s) {}
            @Override public void onSetFailure(String s) {
                Log.e(TAG, "setLocalDescription failed: " + s);
                setLocalLatch.countDown();
            }
        }, offerHolder[0]);

        if (!setLocalLatch.await(5, TimeUnit.SECONDS))
            throw new IOException("setLocalDescription timeout");

        // Send OFFER + any ICE candidates collected so far.
        sendLine(sigOut, "OFFER " + Base64.getEncoder().encodeToString(
                offerHolder[0].description.getBytes(StandardCharsets.UTF_8)));

        // 5. Wait for the streamer's answer and apply it.
        String answerLine = sigIn.readLine();
        if (answerLine == null || !answerLine.startsWith("ANSWER "))
            throw new IOException("expected ANSWER, got: " + answerLine);
        String answerB64 = answerLine.substring("ANSWER ".length()).trim();
        byte[] answerSdp = Base64.getDecoder().decode(answerB64);
        SessionDescription answer = new SessionDescription(
                SessionDescription.Type.ANSWER,
                new String(answerSdp, StandardCharsets.UTF_8));

        CountDownLatch setRemoteLatch = new CountDownLatch(1);
        pc.setRemoteDescription(new SdpObserver() {
            @Override public void onCreateSuccess(SessionDescription sdp) {}
            @Override public void onSetSuccess() { setRemoteLatch.countDown(); }
            @Override public void onCreateFailure(String s) {}
            @Override public void onSetFailure(String s) {
                Log.e(TAG, "setRemoteDescription failed: " + s);
                setRemoteLatch.countDown();
            }
        }, answer);
        if (!setRemoteLatch.await(10, TimeUnit.SECONDS))
            throw new IOException("setRemoteDescription timeout");

        postStatus("Streaming");

        // 6. Pump signaling messages until the connection ends. Local ICE
        //    candidates are forwarded as ICE lines; remote ones are added to PC.
        //    libwebrtc fires onAddStream/onAddTrack when the video track is up;
        //    the renderer is wired up there.
        boolean gotFirstFrame = false;
        while (running) {
            String line = sigIn.readLine();
            if (line == null) break;

            if (line.startsWith("ICE ")) {
                String[] parts = line.substring(4).split(" ", 2);
                if (parts.length != 2) continue;
                byte[] candBytes;
                try {
                    candBytes = Base64.getDecoder().decode(parts[0]);
                } catch (IllegalArgumentException ignored) {
                    continue;
                }
                String candidateStr =
                        new String(candBytes, StandardCharsets.UTF_8);
                String sdpMid = parts[1];
                IceCandidate cand = new IceCandidate(
                        sdpMid, 0, candidateStr);
                try { pc.addIceCandidate(cand); }
                catch (Exception e) { Log.w(TAG, "bad ICE: " + e.getMessage()); }
            } else if (line.equals("BYE")) {
                break;
            }
        }

        try { pc.dispose(); } catch (Exception ignored) {}
    }

    private final PeerConnection.Observer pcObserver = new PeerConnection.Observer() {
        @Override public void onSignalingChange(PeerConnection.SignalingState s) {}
        @Override public void onIceConnectionChange(PeerConnection.IceConnectionState s) {
            Log.i(TAG, "ICE state: " + s);
            if (s == PeerConnection.IceConnectionState.CONNECTED)
                postStatus("Streaming");
            else if (s == PeerConnection.IceConnectionState.FAILED)
                postStatus("ICE failed — reconnecting");
        }
        @Override public void onConnectionChange(PeerConnection.PeerConnectionState s) {}
        @Override public void onIceConnectionReceivingChange(boolean b) {}
        @Override public void onIceGatheringChange(PeerConnection.IceGatheringState s) {}

        @Override public void onIceCandidate(IceCandidate candidate) {
            // Send our local candidate to the streamer via signaling.
            try {
                String b64 = Base64.getEncoder().encodeToString(
                        candidate.sdp.getBytes(StandardCharsets.UTF_8));
                sendLine(signalingSocket.getOutputStream(),
                         "ICE " + b64 + " " + candidate.sdpMid);
            } catch (IOException e) {
                Log.w(TAG, "failed to send ICE: " + e.getMessage());
            }
        }

        @Override public void onIceCandidatesRemoved(IceCandidate[] c) {}
        @Override public void onAddStream(org.webrtc.MediaStream s) {}
        @Override public void onRemoveStream(org.webrtc.MediaStream s) {}
        @Override public void onDataChannel(org.webrtc.DataChannel d) {}
        @Override public void onRenegotiationNeeded() {}

        @Override public void onAddTrack(RtpReceiver receiver,
                                         org.webrtc.MediaStream[] mediaStreams) {
            // Unified Plan: each RECVONLY transceiver fires onAddTrack when
            // its remote track is up. We wire the video track to the renderer.
            MediaStreamTrack track = receiver.track();
            Log.i(TAG, "onAddTrack: " + track.kind()
                    + " id=" + track.id());
            if (track instanceof org.webrtc.VideoTrack) {
                org.webrtc.VideoTrack vt = (org.webrtc.VideoTrack) track;
                if (vt != null) {
                    vt.setEnabled(true);
                    final SurfaceViewRenderer r = renderer;
                    if (r != null) {
                        main.post(() -> {
                            try { vt.addSink(r); }
                            catch (Exception e) {
                                Log.w(TAG, "addSink failed: " + e.getMessage());
                            }
                        });
                    }
                }
            }
        }
    };

    private synchronized void ensureFactory() {
        if (factory != null) return;

        PeerConnectionFactory.initialize(
                PeerConnectionFactory.InitializationOptions.builder(context)
                        .createInitializationOptions());

        JavaAudioDeviceModule audio = JavaAudioDeviceModule.builder(context)
                .createAudioDeviceModule();

        // DefaultVideoDecoderFactory wraps HardwareVideoDecoderFactory with a
        // SoftwareVideoDecoderFactory fallback. On Quest 3 the HW factory
        // picks up HEVC via MediaCodec.
        DefaultVideoDecoderFactory decoders = new DefaultVideoDecoderFactory(
                eglBase != null ? eglBase.getEglBaseContext() : null);

        factory = PeerConnectionFactory.builder()
                .setAudioDeviceModule(audio)
                .setVideoDecoderFactory(decoders)
                .createPeerConnectionFactory();
        Log.i(TAG, "PeerConnectionFactory ready");
    }

    // --------------------------------------------------------------- helpers

    private static void sendLine(OutputStream os, String line) throws IOException {
        os.write((line + "\n").getBytes(StandardCharsets.US_ASCII));
        os.flush();
    }

    private void postStatus(String text) {
        main.post(() -> listener.onStatus(text));
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
