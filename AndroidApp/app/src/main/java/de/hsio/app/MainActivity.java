package de.hsio.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.LayerDrawable;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.SoundPool;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.text.TextUtils;
import android.util.Base64;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.GridLayout;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.net.URI;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
import java.util.Locale;

import javax.net.ssl.SSLSocketFactory;

public class MainActivity extends Activity {
    private static final int NUM_CH = 12;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final String[] names = new String[NUM_CH];
    private final boolean[] outputs = new boolean[NUM_CH];
    private final int[] outModes = new int[NUM_CH];
    private final View[] leds = new View[NUM_CH];
    private final Button[] buttons = new Button[NUM_CH];

    private SharedPreferences prefs;
    private WsClient ws;
    private GridLayout grid;
    private TextView status;
    private String host;
    private String user;
    private String pass;
    private boolean settingsDialogOpen;
    private boolean currentConnectionReceivedState;
    private boolean hasSuccessfulConnection;
    private SoundPool soundPool;
    private int clickOnSoundId;
    private int clickOffSoundId;
    private boolean clickOnSoundLoaded;
    private boolean clickOffSoundLoaded;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        prefs = getSharedPreferences("hsio", MODE_PRIVATE);
        host = prefs.getString("host", "hs-io.local");
        user = prefs.getString("user", "admin");
        pass = prefs.getString("pass", "admin");
        for (int i = 0; i < NUM_CH; i++) names[i] = "Ausgang " + (i + 1);
        setupSound();
        buildUi();
        connect();
    }

    @Override
    protected void onDestroy() {
        if (ws != null) ws.close();
        if (soundPool != null) soundPool.release();
        super.onDestroy();
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        buildUi();
        renderState();
    }

    private void buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.rgb(15, 23, 42));

        status = new TextView(this);
        status.setVisibility(View.GONE);
        status.setTextColor(Color.rgb(148, 163, 184));
        status.setTextSize(12);
        status.setGravity(Gravity.CENTER);
        status.setPadding(dp(8), dp(6), dp(8), dp(6));

        grid = new GridLayout(this);
        boolean landscape = getResources().getConfiguration().orientation == Configuration.ORIENTATION_LANDSCAPE;
        grid.setColumnCount(landscape ? 6 : 1);
        grid.setRowCount(landscape ? 2 : 12);
        grid.setPadding(dp(4), dp(8), dp(4), dp(8));
        root.addView(grid, new LinearLayout.LayoutParams(-1, 0, 1));
        setContentView(root);

        for (int i = 0; i < NUM_CH; i++) addOutputRow(i);
    }

    private void addOutputRow(int ch) {
        boolean landscape = getResources().getConfiguration().orientation == Configuration.ORIENTATION_LANDSCAPE;
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(landscape ? LinearLayout.VERTICAL : LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(5), dp(2), dp(5), dp(2));
        row.setBackground(rowBg());

        GridLayout.LayoutParams gp = new GridLayout.LayoutParams();
        gp.width = landscape ? 0 : GridLayout.LayoutParams.MATCH_PARENT;
        gp.height = 0;
        gp.columnSpec = GridLayout.spec(GridLayout.UNDEFINED, 1f);
        gp.rowSpec = GridLayout.spec(GridLayout.UNDEFINED, 1f);
        gp.setMargins(dp(2), dp(2), dp(2), dp(2));
        grid.addView(row, gp);

        View led = new View(this);
        LinearLayout.LayoutParams lpLed = new LinearLayout.LayoutParams(dp(20), dp(20));
        lpLed.setMargins(0, 0, landscape ? 0 : dp(8), landscape ? dp(3) : 0);
        row.addView(led, lpLed);
        leds[ch] = led;

        TextView label = new TextView(this);
        label.setTextColor(Color.WHITE);
        label.setTextSize(landscape ? 9 : 11);
        label.setIncludeFontPadding(false);
        label.setSingleLine(true);
        label.setEllipsize(TextUtils.TruncateAt.MARQUEE);
        label.setMarqueeRepeatLimit(-1);
        label.setHorizontallyScrolling(true);
        label.setSelected(true);
        label.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
        row.addView(label, new LinearLayout.LayoutParams(landscape ? -1 : 0, -2, landscape ? 0 : 1));

        Button btn = new Button(this);
        btn.setAllCaps(false);
        btn.setTextSize(landscape ? 9 : 11);
        btn.setIncludeFontPadding(false);
        btn.setMinWidth(0);
        btn.setMinHeight(0);
        btn.setPadding(dp(2), 0, dp(2), 0);
        row.addView(btn, new LinearLayout.LayoutParams(landscape ? -1 : dp(78), dp(30)));
        buttons[ch] = btn;

        final Runnable[] longPressTask = new Runnable[1];
        final boolean[] longPressDone = new boolean[1];
        btn.setOnTouchListener((v, event) -> {
            if (outModes[ch] == 1) {
                if (event.getAction() == MotionEvent.ACTION_DOWN) {
                    playClick(true);
                    sendSet(ch, true);
                    return true;
                }
                if (event.getAction() == MotionEvent.ACTION_UP || event.getAction() == MotionEvent.ACTION_CANCEL) {
                    playClick(false);
                    sendSet(ch, false);
                    return true;
                }
                return true;
            }
            if (event.getAction() == MotionEvent.ACTION_DOWN) {
                longPressDone[0] = false;
                longPressTask[0] = () -> {
                    longPressDone[0] = true;
                    sendCommand("{\"cmd\":\"alloff\"}");
                    playClick(false);
                    Toast.makeText(this, "Alle Ausgaenge AUS", Toast.LENGTH_SHORT).show();
                };
                main.postDelayed(longPressTask[0], 2000);
                return true;
            }
            if (event.getAction() == MotionEvent.ACTION_UP) {
                if (longPressTask[0] != null) main.removeCallbacks(longPressTask[0]);
                if (!longPressDone[0]) {
                    playClick(!outputs[ch]);
                    sendCommand(String.format(Locale.US, "{\"cmd\":\"toggle\",\"ch\":%d}", ch));
                }
                longPressTask[0] = null;
                return true;
            }
            if (event.getAction() == MotionEvent.ACTION_CANCEL) {
                if (longPressTask[0] != null) main.removeCallbacks(longPressTask[0]);
                longPressTask[0] = null;
                return true;
            }
            return true;
        });

        row.setTag(label);
        label.setText("A" + (ch + 1) + "  " + names[ch]);
        led.setBackground(ledBg(false));
        btn.setText(outModes[ch] == 1 ? "Taster" : "Toggle");
        btn.setTextColor(Color.WHITE);
        btn.setBackgroundColor(outModes[ch] == 1 ? Color.rgb(220, 38, 38) : Color.rgb(14, 165, 233));

        row.setOnLongClickListener(v -> {
            showSettings();
            return true;
        });
    }

    private void renderState() {
        if (status != null) status.setText("Verbunden mit " + host);
        for (int i = 0; i < NUM_CH; i++) {
            View row = (View) leds[i].getParent();
            TextView label = (TextView) row.getTag();
            label.setText("A" + (i + 1) + "  " + names[i]);
            leds[i].setBackground(ledBg(outputs[i]));
            buttons[i].setText(outModes[i] == 1 ? "Taster" : "Toggle");
            buttons[i].setBackgroundColor(outModes[i] == 1 ? Color.rgb(220, 38, 38) : Color.rgb(14, 165, 233));
            buttons[i].setTextColor(Color.WHITE);
        }
    }

    private void setupSound() {
        AudioAttributes attrs = new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_ASSISTANCE_SONIFICATION)
                .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                .build();
        soundPool = new SoundPool.Builder()
                .setMaxStreams(2)
                .setAudioAttributes(attrs)
                .build();
        soundPool.setOnLoadCompleteListener((pool, sampleId, status) -> {
            if (sampleId == clickOnSoundId) clickOnSoundLoaded = status == 0;
            if (sampleId == clickOffSoundId) clickOffSoundLoaded = status == 0;
        });
        try {
            File on = new File(getCacheDir(), "click_on.wav");
            try (FileOutputStream fos = new FileOutputStream(on)) {
                fos.write(createClickWav(1900.0));
            }
            clickOnSoundId = soundPool.load(on.getAbsolutePath(), 1);

            File off = new File(getCacheDir(), "click_off.wav");
            try (FileOutputStream fos = new FileOutputStream(off)) {
                fos.write(createClickWav(850.0));
            }
            clickOffSoundId = soundPool.load(off.getAbsolutePath(), 1);
        } catch (Exception ignored) {
            clickOnSoundId = 0;
            clickOffSoundId = 0;
        }
    }

    private void playClick(boolean on) {
        if (soundPool == null) return;
        int soundId = on ? clickOnSoundId : clickOffSoundId;
        boolean loaded = on ? clickOnSoundLoaded : clickOffSoundLoaded;
        if (!loaded || soundId == 0) return;
        soundPool.play(soundId, 1.0f, 1.0f, 1, 0, 1.0f);
    }

    private byte[] createClickWav(double frequency) {
        final int sampleRate = 22050;
        final int samples = 1200;
        byte[] pcm = new byte[samples * 2];
        for (int i = 0; i < samples; i++) {
            double t = i / (double)sampleRate;
            double env = Math.exp(-t * 95.0);
            double noise = ((i * 1103515245L + 12345L) & 0xffff) / 32768.0 - 1.0;
            double tone = Math.sin(2.0 * Math.PI * frequency * t);
            short v = (short)(Math.max(-1.0, Math.min(1.0, (tone * 0.65 + noise * 0.35) * env)) * 32767);
            pcm[i * 2] = (byte)(v & 0xff);
            pcm[i * 2 + 1] = (byte)((v >> 8) & 0xff);
        }

        byte[] wav = new byte[44 + pcm.length];
        writeAscii(wav, 0, "RIFF");
        write32(wav, 4, 36 + pcm.length);
        writeAscii(wav, 8, "WAVEfmt ");
        write32(wav, 16, 16);
        write16(wav, 20, 1);
        write16(wav, 22, 1);
        write32(wav, 24, sampleRate);
        write32(wav, 28, sampleRate * 2);
        write16(wav, 32, 2);
        write16(wav, 34, 16);
        writeAscii(wav, 36, "data");
        write32(wav, 40, pcm.length);
        System.arraycopy(pcm, 0, wav, 44, pcm.length);
        return wav;
    }

    private static void writeAscii(byte[] out, int off, String s) {
        for (int i = 0; i < s.length(); i++) out[off + i] = (byte)s.charAt(i);
    }

    private static void write16(byte[] out, int off, int v) {
        out[off] = (byte)(v & 0xff);
        out[off + 1] = (byte)((v >> 8) & 0xff);
    }

    private static void write32(byte[] out, int off, int v) {
        out[off] = (byte)(v & 0xff);
        out[off + 1] = (byte)((v >> 8) & 0xff);
        out[off + 2] = (byte)((v >> 16) & 0xff);
        out[off + 3] = (byte)((v >> 24) & 0xff);
    }

    private void connect() {
        if (ws != null) ws.close();
        if (status != null) status.setText("Verbinde mit " + host + " ...");
        currentConnectionReceivedState = false;
        ws = new WsClient(host, user, pass, new WsClient.Listener() {
            @Override public void onOpen() {
                main.post(() -> { });
            }

            @Override public void onMessage(String text) {
                main.post(() -> applyState(text));
            }

            @Override public void onClosed(String message) {
                main.post(() -> {
                    status.setText(message);
                    if (!hasSuccessfulConnection && !currentConnectionReceivedState) showSettings();
                });
            }
        });
        ws.start();
    }

    private void applyState(String json) {
        try {
            JSONObject o = new JSONObject(json);
            JSONArray outs = o.optJSONArray("outputs");
            JSONArray ns = o.optJSONArray("names");
            JSONArray modes = o.optJSONArray("out_modes");
            for (int i = 0; i < NUM_CH; i++) {
                if (outs != null && outs.length() > i) outputs[i] = outs.optBoolean(i);
                if (ns != null && ns.length() > i) names[i] = ns.optString(i, names[i]);
                if (modes != null && modes.length() > i) outModes[i] = modes.optInt(i, 0);
            }
            currentConnectionReceivedState = true;
            hasSuccessfulConnection = true;
            renderState();
        } catch (Exception e) {
            status.setText("Statusdaten ungueltig");
            if (!hasSuccessfulConnection) showSettings();
        }
    }

    private void sendSet(int ch, boolean val) {
        sendCommand(String.format(Locale.US, "{\"cmd\":\"set\",\"ch\":%d,\"val\":%s}", ch, val ? "true" : "false"));
    }

    private void sendCommand(String json) {
        new Thread(() -> {
            try {
                JSONObject cmd = new JSONObject(json);
                String url = commandUrl(cmd);
                HttpURLConnection con = (HttpURLConnection)new URL(url).openConnection();
                con.setRequestMethod("GET");
                con.setConnectTimeout(5000);
                con.setReadTimeout(5000);
                con.setRequestProperty("Authorization", "Basic " + Base64.encodeToString((user + ":" + pass).getBytes(StandardCharsets.UTF_8), Base64.NO_WRAP));
                int code = con.getResponseCode();
                con.disconnect();
                if (code < 200 || code >= 300) {
                    main.post(() -> Toast.makeText(this, "Schalten fehlgeschlagen: HTTP " + code, Toast.LENGTH_SHORT).show());
                }
            } catch (Exception e) {
                main.post(() -> Toast.makeText(this, "Schalten fehlgeschlagen: " + e.getMessage(), Toast.LENGTH_SHORT).show());
            }
        }).start();
    }

    private String commandUrl(JSONObject cmd) throws Exception {
        URI uri = URI.create(host.contains("://") ? host : "http://" + host);
        String scheme = uri.getScheme() == null ? "http" : uri.getScheme().toLowerCase(Locale.US);
        boolean tls = "https".equals(scheme) || "wss".equals(scheme);
        String h = uri.getHost();
        int port = uri.getPort();
        StringBuilder url = new StringBuilder();
        url.append(tls ? "https" : "http").append("://").append(h);
        if (port > 0) url.append(":").append(port);
        url.append("/api/cmd?cmd=").append(enc(cmd.optString("cmd")));
        if (cmd.has("ch")) url.append("&ch=").append(cmd.optInt("ch"));
        if (cmd.has("val")) url.append("&val=").append(cmd.optBoolean("val") ? "1" : "0");
        return url.toString();
    }

    private static String enc(String value) throws Exception {
        return URLEncoder.encode(value, "UTF-8");
    }

    private void showSettings() {
        if (settingsDialogOpen) return;
        settingsDialogOpen = true;
        LinearLayout form = new LinearLayout(this);
        form.setOrientation(LinearLayout.VERTICAL);
        form.setPadding(dp(18), dp(8), dp(18), 0);
        EditText hostEt = edit("Host/IP", host, InputType.TYPE_CLASS_TEXT);
        EditText userEt = edit("Benutzer", user, InputType.TYPE_CLASS_TEXT);
        EditText passEt = edit("Passwort", pass, InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        form.addView(hostEt);
        form.addView(userEt);
        form.addView(passEt);
        new AlertDialog.Builder(this)
                .setTitle("Verbindung")
                .setView(form)
                .setPositiveButton("Speichern", (d, w) -> {
                    settingsDialogOpen = false;
                    host = hostEt.getText().toString().trim();
                    user = userEt.getText().toString().trim();
                    pass = passEt.getText().toString();
                    hasSuccessfulConnection = false;
                    prefs.edit().putString("host", host).putString("user", user).putString("pass", pass).apply();
                    connect();
                })
                .setNegativeButton("Abbrechen", (d, w) -> settingsDialogOpen = false)
                .setOnCancelListener(d -> settingsDialogOpen = false)
                .show();
    }

    private EditText edit(String hint, String value, int type) {
        EditText e = new EditText(this);
        e.setHint(hint);
        e.setText(value);
        e.setInputType(type);
        return e;
    }

    private GradientDrawable rowBg() {
        GradientDrawable g = new GradientDrawable();
        g.setColor(Color.rgb(30, 41, 59));
        g.setCornerRadius(dp(6));
        g.setStroke(dp(1), Color.rgb(51, 65, 85));
        return g;
    }

    private LayerDrawable ledBg(boolean on) {
        GradientDrawable glow = new GradientDrawable();
        glow.setShape(GradientDrawable.OVAL);
        glow.setColor(on ? Color.argb(95, 34, 197, 94) : Color.argb(0, 0, 0, 0));

        GradientDrawable core = new GradientDrawable();
        core.setShape(GradientDrawable.OVAL);
        core.setColor(on ? Color.rgb(34, 197, 94) : Color.rgb(55, 65, 81));
        core.setStroke(dp(2), on ? Color.rgb(187, 247, 208) : Color.rgb(107, 114, 128));

        LayerDrawable layers = new LayerDrawable(new android.graphics.drawable.Drawable[] { glow, core });
        layers.setLayerInset(1, dp(3), dp(3), dp(3), dp(3));
        return layers;
    }

    private int dp(int v) {
        return (int) (v * getResources().getDisplayMetrics().density + 0.5f);
    }

    private static final class WsClient extends Thread {
        interface Listener {
            void onOpen();
            void onMessage(String text);
            void onClosed(String message);
        }

        private final String hostText;
        private final String user;
        private final String pass;
        private final Listener listener;
        private volatile boolean running = true;
        private volatile boolean open;
        private Socket socket;
        private volatile OutputStream out;
        private long lastPingMs;

        WsClient(String hostText, String user, String pass, Listener listener) {
            this.hostText = hostText;
            this.user = user;
            this.pass = pass;
            this.listener = listener;
        }

        @Override
        public void run() {
            try {
                URI uri = URI.create(hostText.contains("://") ? hostText : "ws://" + hostText + "/ws");
                String scheme = uri.getScheme() == null ? "ws" : uri.getScheme().toLowerCase(Locale.US);
                boolean tls = "wss".equals(scheme) || "https".equals(scheme);
                String host = uri.getHost();
                int port = uri.getPort() > 0 ? uri.getPort() : (tls ? 443 : 80);
                String path = uri.getRawPath();
                if (path == null || path.isEmpty() || "/".equals(path)) path = "/ws";

                socket = tls
                        ? SSLSocketFactory.getDefault().createSocket(host, port)
                        : new Socket(host, port);
                socket.setKeepAlive(true);
                socket.setSoTimeout(1000);
                out = socket.getOutputStream();
                InputStream in = new BufferedInputStream(socket.getInputStream());

                String key = wsKey();
                String auth = Base64.encodeToString((user + ":" + pass).getBytes(StandardCharsets.UTF_8), Base64.NO_WRAP);
                String hostHeader = (uri.getPort() > 0) ? host + ":" + port : host;
                String req = "GET " + path + " HTTP/1.1\r\n"
                        + "Host: " + hostHeader + "\r\n"
                        + "Upgrade: websocket\r\n"
                        + "Connection: Upgrade\r\n"
                        + "Sec-WebSocket-Key: " + key + "\r\n"
                        + "Sec-WebSocket-Version: 13\r\n"
                        + "Authorization: Basic " + auth + "\r\n\r\n";
                out.write(req.getBytes(StandardCharsets.US_ASCII));
                out.flush();

                String status = readHttpLine(in);
                if (status == null || !status.contains("101")) throw new IllegalStateException(status == null ? "Keine Antwort" : status);
                String line;
                while ((line = readHttpLine(in)) != null && line.length() > 0) { }
                open = true;
                lastPingMs = System.currentTimeMillis();
                listener.onOpen();

                while (running) {
                    try {
                        Frame frame = readFrame(in);
                        if (frame == null) break;
                        if (frame.opcode == 1) {
                            listener.onMessage(new String(frame.payload, StandardCharsets.UTF_8));
                        } else if (frame.opcode == 8) {
                            break;
                        } else if (frame.opcode == 9) {
                            sendFrame(10, frame.payload);
                        }
                    } catch (SocketTimeoutException e) {
                        long now = System.currentTimeMillis();
                        if (now - lastPingMs >= 20000) {
                            sendFrame(9, new byte[0]);
                            lastPingMs = now;
                        }
                    }
                }
            } catch (Exception e) {
                listener.onClosed("Getrennt: " + e.getMessage());
            } finally {
                close();
            }
        }

        boolean send(String text) {
            try {
                OutputStream o = out;
                if (!open || o == null) return false;
                sendFrame(1, text.getBytes(StandardCharsets.UTF_8));
                return true;
            } catch (Exception e) {
                open = false;
                return false;
            }
        }

        private synchronized void sendFrame(int opcode, byte[] data) throws Exception {
            OutputStream o = out;
            if (o == null) throw new IllegalStateException("Socket nicht offen");
            ByteArrayOutputStream frame = new ByteArrayOutputStream();
            frame.write(0x80 | (opcode & 0x0f));
            byte[] mask = new byte[4];
            new SecureRandom().nextBytes(mask);
            if (data.length < 126) {
                frame.write(0x80 | data.length);
            } else {
                frame.write(0x80 | 126);
                frame.write((data.length >> 8) & 0xff);
                frame.write(data.length & 0xff);
            }
            frame.write(mask);
            for (int i = 0; i < data.length; i++) frame.write(data[i] ^ mask[i % 4]);
            o.write(frame.toByteArray());
            o.flush();
        }

        void close() {
            running = false;
            open = false;
            try {
                if (socket != null) socket.close();
            } catch (Exception ignored) { }
        }

        private static Frame readFrame(InputStream in) throws Exception {
            int b0 = in.read();
            if (b0 < 0) return null;
            int b1 = in.read();
            if (b1 < 0) return null;
            int opcode = b0 & 0x0f;
            boolean masked = (b1 & 0x80) != 0;
            int len = b1 & 0x7f;
            if (len == 126) len = (in.read() << 8) | in.read();
            if (len == 127) throw new IllegalStateException("Frame zu gross");
            byte[] mask = null;
            if (masked) {
                mask = new byte[4];
                int maskOff = 0;
                while (maskOff < 4) {
                    int n = in.read(mask, maskOff, 4 - maskOff);
                    if (n < 0) return null;
                    maskOff += n;
                }
            }
            byte[] data = new byte[len];
            int off = 0;
            while (off < len) {
                int n = in.read(data, off, len - off);
                if (n < 0) return null;
                off += n;
            }
            if (mask != null) {
                for (int i = 0; i < data.length; i++) data[i] = (byte)(data[i] ^ mask[i % 4]);
            }
            return new Frame(opcode, data);
        }

        private static String readHttpLine(InputStream in) throws Exception {
            ByteArrayOutputStream line = new ByteArrayOutputStream();
            int prev = -1;
            int b;
            while ((b = in.read()) >= 0) {
                if (prev == '\r' && b == '\n') break;
                if (prev >= 0) line.write(prev);
                prev = b;
            }
            if (b < 0 && prev < 0 && line.size() == 0) return null;
            if (b < 0 && prev >= 0) line.write(prev);
            return line.toString("US-ASCII");
        }

        private static String wsKey() {
            byte[] b = new byte[16];
            new SecureRandom().nextBytes(b);
            return Base64.encodeToString(b, Base64.NO_WRAP);
        }

        private static final class Frame {
            final int opcode;
            final byte[] payload;

            Frame(int opcode, byte[] payload) {
                this.opcode = opcode;
                this.payload = payload;
            }
        }
    }
}
