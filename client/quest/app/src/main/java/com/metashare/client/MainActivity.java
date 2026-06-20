package com.metashare.client;

import android.app.Activity;
import android.graphics.Color;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;

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

public final class MainActivity extends Activity implements SurfaceHolder.Callback {
    private static final String TAG = "MetaShare2D";

    private static final byte[] DISCOVERY_MAGIC =
            new byte[] {'M', 'S', 'H', 'A', 'R', 'E', 'D', '1'};
    private static final byte[] STREAM_MAGIC =
            new byte[] {'M', 'S', 'H', 'A', 'R', 'E', 'S', '1'};
    private static final int DISCOVERY_PORT = 7777;
    private static final int PROTOCOL_VERSION = 1;
    private static final int CAP_H264 = 1;
    private static final int CODEC_H264 = 0;
    private static final int FRAME_KEYFRAME = 1;

    private SurfaceView surfaceView;
    private FrameLayout root;
    private TextView statusView;
    private volatile boolean running;
    private volatile DatagramSocket discoverySocket;
    private volatile Socket streamSocket;
    private Thread worker;
    private volatile int streamWidth;
    private volatile int streamHeight;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        surfaceView = new SurfaceView(this);
        surfaceView.getHolder().addCallback(this);

        statusView = new TextView(this);
        statusView.setTextColor(Color.WHITE);
        statusView.setTextSize(14);
        statusView.setGravity(Gravity.START);
        statusView.setBackgroundColor(0xAA000000);
        statusView.setPadding(18, 10, 18, 10);

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        root.addView(surfaceView, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));
        FrameLayout.LayoutParams statusParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.START);
        statusParams.setMargins(18, 18, 18, 18);
        root.addView(statusView, statusParams);
        setContentView(root);
        this.root = root;
        root.addOnLayoutChangeListener(
                (v, l, t, r, b, ol, ot, or, ob) -> applyAspectRatio());

        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        setStatus("Searching for MetaShare streamer...");
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        startWorker(holder.getSurface());
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        stopWorker();
    }

    private void applyAspectRatio() {
        int sw = streamWidth;
        int sh = streamHeight;
        int cw = root.getWidth();
        int ch = root.getHeight();
        if (sw <= 0 || sh <= 0 || cw <= 0 || ch <= 0) return;
        float streamRatio = (float) sw / sh;
        float containerRatio = (float) cw / ch;
        int targetW;
        int targetH;
        if (streamRatio > containerRatio) {
            targetW = cw;
            targetH = Math.round(cw / streamRatio);
        } else {
            targetH = ch;
            targetW = Math.round(ch * streamRatio);
        }
        FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) surfaceView.getLayoutParams();
        if (lp == null
                || lp.width != targetW || lp.height != targetH
                || lp.gravity != Gravity.CENTER) {
            surfaceView.setLayoutParams(
                    new FrameLayout.LayoutParams(targetW, targetH, Gravity.CENTER));
        }
    }

    @Override
    protected void onDestroy() {
        stopWorker();
        super.onDestroy();
    }

    private synchronized void startWorker(Surface surface) {
        if (running || surface == null || !surface.isValid()) return;
        running = true;
        worker = new Thread(() -> runClient(surface), "MetaShareStream");
        worker.start();
    }

    private synchronized void stopWorker() {
        running = false;
        closeDiscoverySocket();
        closeStreamSocket();
        if (worker != null) {
            worker.interrupt();
            worker = null;
        }
    }

    private void runClient(Surface surface) {
        WifiManager.MulticastLock multicastLock = null;
        try {
            WifiManager wifi = (WifiManager) getApplicationContext()
                    .getSystemService(WIFI_SERVICE);
            if (wifi != null) {
                multicastLock = wifi.createMulticastLock("metashare-discovery");
                multicastLock.setReferenceCounted(false);
                multicastLock.acquire();
            }

            while (running) {
                try {
                    setStatus("Searching for MetaShare streamer...");
                    InetSocketAddress offer = discover(3000);
                    if (offer == null) {
                        sleepQuietly(1000);
                        continue;
                    }
                    setStatus("Connecting to " + offer.getAddress().getHostAddress() + "...");
                    connectAndDecode(offer, surface);
                } catch (Exception e) {
                    Log.e(TAG, "stream loop failed", e);
                    if (running) {
                        setStatus("Disconnected. Reconnecting...");
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
                    .putInt(CAP_H264)
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
                if (version == PROTOCOL_VERSION && port > 0) {
                    Log.i(TAG, "discovered streamer at "
                            + packet.getAddress().getHostAddress() + ":" + port);
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

    private void connectAndDecode(InetSocketAddress offer, Surface surface) throws IOException {
        Socket socket = new Socket();
        streamSocket = socket;
        socket.setTcpNoDelay(true);
        socket.connect(offer, 3000);

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
        if (version != PROTOCOL_VERSION || codec != CODEC_H264 || width <= 0 || height <= 0) {
            throw new IOException("unsupported stream");
        }

        streamWidth = width;
        streamHeight = height;
        runOnUiThread(this::applyAspectRatio);

        MediaCodec decoder = null;
        try {
            MediaFormat format = MediaFormat.createVideoFormat(
                    MediaFormat.MIMETYPE_VIDEO_AVC, width, height);
            if (fpsNum > 0 && fpsDen > 0) {
                format.setInteger(MediaFormat.KEY_FRAME_RATE, Math.max(1, fpsNum / fpsDen));
            }
            format.setInteger("low-latency", 1);
            decoder = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
            decoder.configure(format, surface, null, 0);
            decoder.start();

            Log.i(TAG, "connected: stream " + width + "x" + height);
            setStatus("Waiting for first frame...");
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
                        setStatus("");
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
                    setStatus("");
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

    private void setStatus(String text) {
        runOnUiThread(() -> {
            if (text == null || text.isEmpty()) {
                statusView.setVisibility(View.GONE);
            } else {
                statusView.setVisibility(View.VISIBLE);
                statusView.setText(text);
            }
        });
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
