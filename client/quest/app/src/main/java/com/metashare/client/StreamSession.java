package com.metashare.client;

import android.content.Context;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.net.wifi.WifiManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Surface;

import java.io.DataInputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
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
import java.util.Collections;

/**
 * One streaming connection to a MetaShare streamer: UDP discovery, TCP
 * handshake, hardware-decoded H.264 rendered to a {@link Surface}.
 *
 * <p>Reusable across multiple windows — each Activity that wants a stream
 * creates its own {@code StreamSession} pointing at the same Surface.
 */
public final class StreamSession {

    private static final String TAG = "StreamSession";

    private static final byte[] DISCOVERY_MAGIC =
            new byte[] {'M', 'S', 'H', 'A', 'R', 'E', 'D', '1'};
    private static final byte[] STREAM_MAGIC =
            new byte[] {'M', 'S', 'H', 'A', 'R', 'E', 'S', '1'};
    private static final int DISCOVERY_PORT = 7777;
    private static final int PROTOCOL_VERSION = 1;
    private static final int CAP_H264 = 1 << 0;
    private static final int CAP_H265 = 1 << 1;
    private static final int CODEC_H264 = 0;
    private static final int CODEC_H265 = 1;
    private static final int FRAME_KEYFRAME = 1;

    /** Callbacks delivered on the main thread. */
    public interface Listener {
        void onStatus(String text);
        void onStreamSize(int width, int height);
    }

    private final Context context;
    private volatile Surface surface;
    private final int monitorIndex;
    private final Listener listener;
    private final Handler main = new Handler(Looper.getMainLooper());

    private volatile boolean running;
    private volatile DatagramSocket discoverySocket;
    private volatile Socket streamSocket;
    private Thread worker;

    public StreamSession(Context context, Surface surface, int monitorIndex,
                         Listener listener) {
        this.context = context.getApplicationContext();
        this.surface = surface;
        this.monitorIndex = monitorIndex;
        this.listener = listener;
    }

    /** Update the surface (used when TextureView recreates its surface). */
    public void setSurface(Surface surface) {
        this.surface = surface;
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
        closeStreamSocket();
        if (worker != null) {
            worker.interrupt();
            worker = null;
        }
    }

    // ------------------------------------------------------------------ loop

    private void runLoop() {
        WifiManager.MulticastLock multicastLock = null;
        try {
            WifiManager wifi = (WifiManager) context.getSystemService(Context.WIFI_SERVICE);
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
                    postStatus("Connecting to " + offer.getAddress().getHostAddress() + "...");
                    connectAndDecode(offer);
                } catch (Exception e) {
                    Log.e(TAG, "stream loop failed", e);
                    if (running) {
                        postStatus("Disconnected. Reconnecting...");
                        sleepQuietly(1200);
                    }
                } finally {
                    closeStreamSocket();
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
            for (NetworkInterface nif : Collections.list(NetworkInterface.getNetworkInterfaces())) {
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
                            + packet.getAddress().getHostAddress() + ":" + port
                            + " (" + monitorCount + " monitor"
                            + (monitorCount != 1 ? "s" : "") + ")");
                    return new InetSocketAddress(packet.getAddress(), port);
                }
            }
            return null;
        } finally {
            closeDiscoverySocket();
        }
    }

    private static void sendProbe(DatagramSocket socket, byte[] probe, InetAddress address) {
        try {
            DatagramPacket packet = new DatagramPacket(
                    probe, probe.length, address, DISCOVERY_PORT);
            socket.send(packet);
        } catch (IOException ignored) {
        }
    }

    // ----------------------------------------------------------- stream+decode

    private void connectAndDecode(InetSocketAddress offer) throws IOException {
        int targetPort = offer.getPort() + monitorIndex;
        Log.i(TAG, "monitor " + monitorIndex + " -> connecting to port " + targetPort);
        Socket socket = new Socket();
        streamSocket = socket;
        socket.setTcpNoDelay(true);
        socket.connect(new InetSocketAddress(offer.getAddress(), targetPort), 3000);

        DataInputStream in = new DataInputStream(socket.getInputStream());
        byte[] header = readFully(in, 32);
        if (!startsWith(header, STREAM_MAGIC)) throw new IOException("bad stream header");

        ByteBuffer h = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
        h.position(8);
        int version = h.getInt();
        int codec = h.getInt();
        int width = h.getInt();
        int height = h.getInt();
        int fpsNum = h.getInt();
        int fpsDen = h.getInt();
        if (version != PROTOCOL_VERSION || width <= 0 || height <= 0) {
            throw new IOException("unsupported stream");
        }
        String mime = codec == CODEC_H265 ? MediaFormat.MIMETYPE_VIDEO_HEVC
                : codec == CODEC_H264 ? MediaFormat.MIMETYPE_VIDEO_AVC
                : null;
        if (mime == null) throw new IOException("unsupported codec " + codec);

        Log.i(TAG, "stream codec=" + (codec == CODEC_H265 ? "HEVC" : "H264")
                + " " + width + "x" + height);
        main.post(() -> listener.onStreamSize(width, height));

        Surface surface = this.surface;
        if (surface == null || !surface.isValid()) {
            throw new IOException("surface unavailable");
        }

        MediaCodec decoder = null;
        try {
            MediaFormat format = MediaFormat.createVideoFormat(mime, width, height);
            if (fpsNum > 0 && fpsDen > 0) {
                format.setInteger(MediaFormat.KEY_FRAME_RATE, Math.max(1, fpsNum / fpsDen));
            }
            format.setInteger("low-latency", 1);
            decoder = MediaCodec.createDecoderByType(mime);
            decoder.configure(format, surface, null, 0);
            decoder.start();

            Log.i(TAG, "connected: stream " + width + "x" + height);
            postStatus("Waiting for first frame...");
            boolean firstOutput = false;
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            while (running && !socket.isClosed()) {
                byte[] frameHeader = readFully(in, 16);
                ByteBuffer fh = ByteBuffer.wrap(frameHeader).order(ByteOrder.LITTLE_ENDIAN);
                int payloadSize = fh.getInt();
                int flags = fh.getInt();
                long ptsUsec = fh.getLong();
                if (payloadSize <= 0 || payloadSize > 32 * 1024 * 1024) {
                    throw new IOException("bad payload size " + payloadSize);
                }
                byte[] payload = readFully(in, payloadSize);

                int inputIndex;
                do {
                    if (drainDecoder(decoder, info, firstOutput)) {
                        firstOutput = true;
                        postStatus("");
                    }
                    inputIndex = decoder.dequeueInputBuffer(10_000);
                } while (running && inputIndex < 0);
                if (!running || inputIndex < 0) break;

                ByteBuffer input = decoder.getInputBuffer(inputIndex);
                if (input == null || input.capacity() < payload.length) {
                    decoder.queueInputBuffer(inputIndex, 0, 0, ptsUsec, 0);
                    continue;
                }
                input.clear();
                input.put(payload);
                int codecFlags = ((flags & FRAME_KEYFRAME) != 0)
                        ? MediaCodec.BUFFER_FLAG_KEY_FRAME : 0;
                decoder.queueInputBuffer(inputIndex, 0, payload.length, ptsUsec, codecFlags);

                if (drainDecoder(decoder, info, firstOutput)) {
                    firstOutput = true;
                    postStatus("");
                }
            }
        } finally {
            if (decoder != null) {
                try {
                    decoder.stop();
                } catch (IllegalStateException ignored) {
                }
                decoder.release();
            }
        }
    }

    private static boolean drainDecoder(MediaCodec decoder, MediaCodec.BufferInfo info,
                                        boolean alreadyHadOutput) {
        boolean produced = false;
        for (;;) {
            int out = decoder.dequeueOutputBuffer(info, 0);
            if (out >= 0) {
                decoder.releaseOutputBuffer(out, true);
                if (!alreadyHadOutput && !produced) {
                    Log.i(TAG, "decoder produced first output frame");
                }
                produced = true;
            } else if (out == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                Log.i(TAG, "decoder output format: " + decoder.getOutputFormat());
            } else if (out == MediaCodec.INFO_TRY_AGAIN_LATER) {
                break;
            } else {
                break;
            }
        }
        return produced;
    }

    // -------------------------------------------------------------- utilities

    private void postStatus(String text) {
        main.post(() -> listener.onStatus(text));
    }

    private static byte[] readFully(InputStream in, int len) throws IOException {
        byte[] data = new byte[len];
        int off = 0;
        while (off < len) {
            int n = in.read(data, off, len - off);
            if (n < 0) throw new EOFException();
            off += n;
        }
        return data;
    }

    private static boolean startsWith(byte[] data, byte[] prefix) {
        if (data.length < prefix.length) return false;
        for (int i = 0; i < prefix.length; ++i) {
            if (data[i] != prefix[i]) return false;
        }
        return true;
    }

    private void closeDiscoverySocket() {
        DatagramSocket socket = discoverySocket;
        discoverySocket = null;
        if (socket != null) socket.close();
    }

    private void closeStreamSocket() {
        Socket socket = streamSocket;
        streamSocket = null;
        if (socket != null) {
            try {
                socket.close();
            } catch (IOException ignored) {
            }
        }
    }

    private static void sleepQuietly(long millis) {
        try {
            Thread.sleep(millis);
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        }
    }
}
