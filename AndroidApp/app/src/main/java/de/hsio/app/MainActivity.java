package de.hsio.app;

import android.animation.ObjectAnimator;
import android.animation.ValueAnimator;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.LayerDrawable;
import android.graphics.drawable.StateListDrawable;
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
import android.view.ViewParent;
import android.view.WindowInsets;
import android.view.animation.LinearInterpolator;
import android.widget.Button;
import android.widget.EditText;
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
    private static final long STATE_POLL_MS = 2000;
    private static final long STATE_STALE_MS = 6000;
    private static final long INITIAL_CONNECT_GRACE_MS = 15000;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final String[] names = new String[NUM_CH];
    private final boolean[] outputs = new boolean[NUM_CH];
    private final int[] remaining = new int[NUM_CH];
    private final int[] outModes = new int[NUM_CH];
    private final int[] outOrder = new int[NUM_CH];
    private final boolean[] outEnabled = new boolean[NUM_CH];
    private final int[] rowChannels = new int[NUM_CH];
    private final View[] rows = new View[NUM_CH];
    private final View[] leds = new View[NUM_CH];
    private final Button[] buttons = new Button[NUM_CH];
    private final TextView[] remainingLabels = new TextView[NUM_CH];
    private final ObjectAnimator[] labelAnims = new ObjectAnimator[NUM_CH];
    private final String[] labelAnimTexts = new String[NUM_CH];
    private final int[] labelAnimWidths = new int[NUM_CH];

    private SharedPreferences prefs;
    private WsClient ws;
    private LinearLayout grid;
    private TextView header;
    private TextView status;
    private String host;
    private String user;
    private String pass;
    private String activeHttpBaseUrl;
    private boolean settingsDialogOpen;
    private boolean errorDialogOpen;
    private String lastErrorDialogMessage = "";
    private boolean currentConnectionReceivedState;
    private boolean hasSuccessfulConnection;
    private boolean connected;
    private int momentaryTouchChannel = -1;
    private long lastStateMs;
    private long connectStartedMs;
    private SoundPool soundPool;
    private int clickOnSoundId;
    private int clickOffSoundId;
    private boolean clickOnSoundLoaded;
    private boolean clickOffSoundLoaded;
    private final Runnable statePollTask = new Runnable() {
        @Override public void run() {
            fetchState();
            main.postDelayed(this, STATE_POLL_MS);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        prefs = getSharedPreferences("hsio", MODE_PRIVATE);
        host = prefs.getString("host", "hs-io.local").trim();
        user = prefs.getString("user", "admin").trim();
        pass = prefs.getString("pass", "admin");
        for (int i = 0; i < NUM_CH; i++) {
            names[i] = "Ausgang " + (i + 1);
            outOrder[i] = i;
            outEnabled[i] = true;
            rowChannels[i] = i;
        }
        setupSound();
        buildUi();
        connect();
    }

    @Override
    protected void onDestroy() {
        main.removeCallbacks(statePollTask);
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
        root.setPadding(0, statusBarHeight(), 0, 0);
        root.setOnApplyWindowInsetsListener((v, insets) -> {
            v.setPadding(0, insets.getSystemWindowInsetTop(), 0, insets.getSystemWindowInsetBottom());
            return insets;
        });

        header = new TextView(this);
        header.setText("HS-IO  v" + appVersion());
        header.setTextColor(Color.WHITE);
        header.setTextSize(14);
        header.setTypeface(Typeface.create(Typeface.SERIF, Typeface.BOLD_ITALIC));
        header.setShadowLayer(dp(4), dp(3), dp(3), Color.rgb(2, 6, 23));
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setPadding(dp(8), dp(6), dp(8), dp(4));
        root.addView(header, new LinearLayout.LayoutParams(-1, -2));

        status = new TextView(this);
        status.setVisibility(View.GONE);
        status.setTextColor(Color.rgb(148, 163, 184));
        status.setTextSize(12);
        status.setGravity(Gravity.CENTER);
        status.setPadding(dp(8), dp(6), dp(8), dp(6));
        root.addView(status, new LinearLayout.LayoutParams(-1, -2));

        grid = new LinearLayout(this);
        grid.setOrientation(LinearLayout.VERTICAL);
        grid.setPadding(dp(4), dp(8), dp(4), dp(8));
        root.addView(grid, new LinearLayout.LayoutParams(-1, 0, 1));
        setContentView(root);

        for (int i = 0; i < NUM_CH; i++) addOutputRow(i);
    }

    private void addOutputRow(int pos) {
        boolean landscape = getResources().getConfiguration().orientation == Configuration.ORIENTATION_LANDSCAPE;
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(landscape ? LinearLayout.VERTICAL : LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(5), dp(2), dp(5), dp(2));
        row.setBackground(rowBg());

        rows[pos] = row;

        View led = new View(this);
        LinearLayout.LayoutParams lpLed = new LinearLayout.LayoutParams(dp(20), dp(20));
        lpLed.setMargins(0, 0, landscape ? 0 : dp(8), landscape ? dp(3) : 0);
        row.addView(led, lpLed);
        leds[pos] = led;

        TextView label = new TextView(this);
        label.setTextColor(Color.WHITE);
        label.setTextSize(landscape ? 11 : 14);
        label.setTypeface(Typeface.create("calibri", Typeface.NORMAL));
        label.setShadowLayer(0, 0, 0, Color.TRANSPARENT);
        label.setIncludeFontPadding(false);
        label.setSingleLine(true);
        label.setEllipsize(null);
        label.setHorizontallyScrolling(true);
        label.setGravity(Gravity.START);

        TextView remain = new TextView(this);
        remain.setTextColor(Color.rgb(148, 163, 184));
        remain.setTextSize(landscape ? 9 : 11);
        remain.setTypeface(Typeface.create("calibri", Typeface.NORMAL));
        remain.setIncludeFontPadding(false);
        remain.setSingleLine(true);
        remain.setGravity(Gravity.START);
        remain.setVisibility(View.GONE);
        remainingLabels[pos] = remain;

        LinearLayout textBox = new LinearLayout(this);
        textBox.setOrientation(LinearLayout.VERTICAL);
        textBox.setGravity(Gravity.CENTER_VERTICAL);
        textBox.addView(label, new LinearLayout.LayoutParams(-1, -2));
        textBox.addView(remain, new LinearLayout.LayoutParams(-1, -2));

        LinearLayout.LayoutParams lpText = new LinearLayout.LayoutParams(landscape ? -1 : 0, -2, landscape ? 0 : 1);
        lpText.setMargins(0, 0, landscape ? 0 : dp(4), 0);
        row.addView(textBox, lpText);

        Button btn = new Button(this);
        btn.setAllCaps(false);
        btn.setTextSize(landscape ? 11 : 13);
        btn.setTypeface(Typeface.create("calibri", Typeface.BOLD));
        btn.setShadowLayer(0, 0, 0, Color.TRANSPARENT);
        btn.setIncludeFontPadding(false);
        btn.setMinWidth(0);
        btn.setMinHeight(0);
        btn.setPadding(dp(2), 0, dp(2), 0);
        row.addView(btn, new LinearLayout.LayoutParams(landscape ? -1 : dp(86), dp(44)));
        buttons[pos] = btn;

        final Runnable[] longPressTask = new Runnable[1];
        final boolean[] longPressDone = new boolean[1];
        btn.setOnTouchListener((v, event) -> {
            int ch = rowChannels[pos];
            if (ch < 0 || ch >= NUM_CH || !connected) return true;
            if (outModes[ch] == 1) {
                if (event.getAction() == MotionEvent.ACTION_DOWN) {
                    momentaryTouchChannel = ch;
                    animateButton(v, true);
                    playClick(true);
                    sendSet(ch, true);
                    return true;
                }
                if (event.getAction() == MotionEvent.ACTION_UP || event.getAction() == MotionEvent.ACTION_CANCEL) {
                    animateButton(v, false);
                    playClick(false);
                    sendSet(ch, false);
                    if (momentaryTouchChannel == ch) momentaryTouchChannel = -1;
                    return true;
                }
                return true;
            }
            if (event.getAction() == MotionEvent.ACTION_DOWN) {
                animateButton(v, true);
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
                animateButton(v, false);
                if (longPressTask[0] != null) main.removeCallbacks(longPressTask[0]);
                if (!longPressDone[0]) {
                    playClick(!outputs[ch]);
                    sendCommand(String.format(Locale.US, "{\"cmd\":\"toggle\",\"ch\":%d}", ch));
                }
                longPressTask[0] = null;
                return true;
            }
            if (event.getAction() == MotionEvent.ACTION_CANCEL) {
                animateButton(v, false);
                if (longPressTask[0] != null) main.removeCallbacks(longPressTask[0]);
                longPressTask[0] = null;
                return true;
            }
            return true;
        });

        row.setTag(label);
        label.setText(labelFor(pos));
        led.setBackground(ledBg(false));
        btn.setText(outModes[pos] == 1 ? "Taster" : "Toggle");
        btn.setTextColor(Color.WHITE);
        btn.setBackground(buttonBg(outModes[pos] == 1));
        btn.setEnabled(false);
        btn.setAlpha(0.45f);

        row.setOnLongClickListener(v -> {
            showSettings();
            return true;
        });
    }

    private void renderState() {
        if (status != null) status.setText("Verbunden mit " + host);
        if (status != null) status.setVisibility(View.GONE);
        boolean landscape = getResources().getConfiguration().orientation == Configuration.ORIENTATION_LANDSCAPE;
        if (momentaryTouchChannel >= 0) {
            updateVisibleRows(landscape);
            return;
        }
        int visibleCount = assignVisibleRows();
        int columns = landscape ? Math.max(1, (visibleCount + 1) / 2) : 1;

        grid.removeAllViews();
        for (int i = 0; i < NUM_CH; i++) {
            View row = rows[i];
            row.setVisibility(View.GONE);
        }

        LinearLayout top = null;
        LinearLayout bottom = null;
        if (landscape) {
            top = outputBand();
            bottom = outputBand();
            grid.addView(top, new LinearLayout.LayoutParams(-1, 0, 1));
            grid.addView(bottom, new LinearLayout.LayoutParams(-1, 0, 1));
        }

        for (int i = 0; i < visibleCount; i++) {
            LinearLayout row = (LinearLayout)rows[i];
            int ch = rowChannels[i];
            if (ch < 0 || ch >= NUM_CH) continue;
            ViewParent parent = row.getParent();
            if (parent instanceof LinearLayout) ((LinearLayout)parent).removeView(row);

            row.setOrientation(landscape ? LinearLayout.VERTICAL : LinearLayout.HORIZONTAL);
            LinearLayout.LayoutParams lp = landscape
                    ? new LinearLayout.LayoutParams(0, -1, 1)
                    : new LinearLayout.LayoutParams(-1, 0, 1);
            lp.setMargins(dp(2), dp(2), dp(2), dp(2));
            if (landscape) {
                (i < columns ? top : bottom).addView(row, lp);
            } else {
                grid.addView(row, lp);
            }
            row.setVisibility(View.VISIBLE);

            updateRowControls(i, ch, landscape);
        }
    }

    private void updateVisibleRows(boolean landscape) {
        for (int i = 0; i < NUM_CH; i++) {
            View row = rows[i];
            if (row == null || row.getVisibility() != View.VISIBLE) continue;
            int ch = rowChannels[i];
            if (ch < 0 || ch >= NUM_CH) continue;
            updateRowControls(i, ch, landscape);
        }
    }

    private void updateRowControls(int pos, int ch, boolean landscape) {
        TextView label = (TextView) rows[pos].getTag();
        label.setText(labelFor(ch));
        label.setTextSize(landscape ? 11 : 14);
        label.setTypeface(Typeface.create("calibri", Typeface.NORMAL));
        label.setShadowLayer(0, 0, 0, Color.TRANSPARENT);
        startLabelScroll(pos, label);
        TextView remain = remainingLabels[pos];
        if (remain != null) {
            remain.setTextSize(landscape ? 9 : 11);
            if (outputs[ch] && remaining[ch] > 0) {
                remain.setText("Restzeit " + formatRemaining(remaining[ch]));
                remain.setVisibility(View.VISIBLE);
            } else {
                remain.setText("");
                remain.setVisibility(View.GONE);
            }
        }
        leds[pos].setBackground(ledBg(outputs[ch]));
        buttons[pos].setText(outModes[ch] == 1 ? "Taster" : "Toggle");
        buttons[pos].setTextSize(landscape ? 11 : 13);
        buttons[pos].setTypeface(Typeface.create("calibri", Typeface.BOLD));
        buttons[pos].setShadowLayer(0, 0, 0, Color.TRANSPARENT);
        buttons[pos].setBackground(buttonBg(outModes[ch] == 1));
        buttons[pos].setTextColor(Color.WHITE);
        buttons[pos].setEnabled(connected);
        buttons[pos].setAlpha(connected ? 1.0f : 0.45f);
    }

    private LinearLayout outputBand() {
        LinearLayout band = new LinearLayout(this);
        band.setOrientation(LinearLayout.HORIZONTAL);
        return band;
    }

    private int assignVisibleRows() {
        boolean[] used = new boolean[NUM_CH];
        int pos = 0;
        for (int i = 0; i < NUM_CH; i++) {
            int ch = outOrder[i];
            if (ch < 0 || ch >= NUM_CH || used[ch] || !outEnabled[ch]) continue;
            rowChannels[pos++] = ch;
            used[ch] = true;
        }
        for (int ch = 0; ch < NUM_CH; ch++) {
            if (!used[ch] && outEnabled[ch]) {
                rowChannels[pos++] = ch;
                used[ch] = true;
            }
        }
        for (int i = pos; i < NUM_CH; i++) rowChannels[i] = -1;
        if (pos == 0) {
            for (int ch = 0; ch < NUM_CH; ch++) rowChannels[ch] = ch;
            return NUM_CH;
        }
        return pos;
    }

    private boolean jsonBool(JSONArray array, int index, boolean fallback) {
        Object value = array.opt(index);
        if (value instanceof Boolean) return (Boolean)value;
        if (value instanceof Number) return ((Number)value).intValue() != 0;
        if (value instanceof String) {
            String s = ((String)value).trim();
            if ("1".equals(s)) return true;
            if ("0".equals(s)) return false;
            if ("true".equalsIgnoreCase(s)) return true;
            if ("false".equalsIgnoreCase(s)) return false;
        }
        return fallback;
    }

    private String labelFor(int ch) {
        String name = cleanName(ch);
        if (hasUniqueConfiguredName(ch, name)) return name;
        return "A" + (ch + 1) + "  " + name;
    }

    private String formatRemaining(int secs) {
        if (secs < 0) secs = 0;
        int h = secs / 3600;
        int m = (secs % 3600) / 60;
        int s = secs % 60;
        if (h > 0) return String.format(Locale.US, "%02d:%02d:%02d", h, m, s);
        return String.format(Locale.US, "%02d:%02d", m, s);
    }

    private boolean hasUniqueConfiguredName(int ch, String name) {
        if (name.length() == 0 || name.equals("Ausgang " + (ch + 1))) return false;
        for (int i = 0; i < NUM_CH; i++) {
            if (i != ch && outEnabled[i] && name.equals(cleanName(i))) return false;
        }
        return true;
    }

    private String cleanName(int ch) {
        String name = names[ch] == null ? "" : names[ch].trim();
        return name.length() == 0 ? "Ausgang " + (ch + 1) : name;
    }

    private void setConnected(boolean value) {
        connected = value;
        for (Button button : buttons) {
            if (button != null) {
                button.setEnabled(value);
                button.setAlpha(value ? 1.0f : 0.45f);
            }
        }
    }

    private void animateButton(View v, boolean pressed) {
        v.animate()
                .scaleX(pressed ? 0.94f : 1.0f)
                .scaleY(pressed ? 0.90f : 1.0f)
                .setDuration(pressed ? 70 : 120)
                .start();
    }

    private void startLabelScroll(int pos, TextView label) {
        label.post(() -> {
            int width = label.getWidth();
            String text = label.getText().toString();
            if (width > 0 && text.equals(labelAnimTexts[pos]) && width == labelAnimWidths[pos] && labelAnims[pos] != null) {
                return;
            }

            if (labelAnims[pos] != null) {
                labelAnims[pos].cancel();
                labelAnims[pos] = null;
            }
            label.setScrollX(0);
            labelAnimTexts[pos] = text;
            labelAnimWidths[pos] = width;

            int textWidth = (int)Math.ceil(label.getPaint().measureText(label.getText().toString()));
            int overflow = Math.max(0, textWidth - width + dp(8));
            if (width <= 0 || overflow <= 0) {
                labelAnimTexts[pos] = null;
                labelAnimWidths[pos] = 0;
                return;
            }

            ObjectAnimator anim = ObjectAnimator.ofInt(label, "scrollX", 0, overflow);
            anim.setDuration(Math.max(4500, overflow * 45L));
            anim.setRepeatCount(ValueAnimator.INFINITE);
            anim.setRepeatMode(ValueAnimator.REVERSE);
            anim.setInterpolator(new LinearInterpolator());
            labelAnims[pos] = anim;
            anim.start();
        });
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
        main.removeCallbacks(statePollTask);
        if (ws != null) ws.close();
        if (status != null) status.setText("Verbinde mit " + host + " ...");
        if (status != null) status.setVisibility(View.VISIBLE);
        setConnected(false);
        lastStateMs = 0;
        connectStartedMs = System.currentTimeMillis();
        activeHttpBaseUrl = null;
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
                    markDisconnectedIfStale(message);
                });
            }
        });
        ws.start();
        main.postDelayed(statePollTask, 3000);
    }

    private void applyState(String json) {
        try {
            JSONObject o = new JSONObject(json);
            JSONArray outs = o.optJSONArray("outputs");
            JSONArray rem = o.optJSONArray("remaining");
            JSONArray ns = o.optJSONArray("names");
            JSONArray modes = o.optJSONArray("out_modes");
            JSONArray order = o.optJSONArray("out_order");
            JSONArray enabled = o.optJSONArray("out_enabled");
            for (int i = 0; i < NUM_CH; i++) {
                if (outs != null && outs.length() > i) outputs[i] = outs.optBoolean(i);
                if (rem != null && rem.length() > i) remaining[i] = Math.max(0, rem.optInt(i, 0));
                if (ns != null && ns.length() > i) names[i] = ns.optString(i, names[i]);
                if (modes != null && modes.length() > i) outModes[i] = modes.optInt(i, 0);
                if (order != null && order.length() > i) outOrder[i] = order.optInt(i, i);
                if (enabled != null && enabled.length() > i) outEnabled[i] = jsonBool(enabled, i, true);
            }
            currentConnectionReceivedState = true;
            hasSuccessfulConnection = true;
            lastStateMs = System.currentTimeMillis();
            setConnected(true);
            renderState();
        } catch (Exception e) {
            setConnected(false);
            String message = "Statusdaten ungueltig: " + e.getMessage();
            if (status != null) {
                status.setVisibility(View.VISIBLE);
                status.setText(message);
            }
            showError(message);
            if (!hasSuccessfulConnection) showSettings();
        }
    }

    private void fetchState() {
        new Thread(() -> {
            String lastError = null;
            String[] bases = httpBaseUrls();
            for (String base : bases) {
                try {
                    HttpURLConnection con = (HttpURLConnection)new URL(base + "/api/state").openConnection();
                    con.setRequestMethod("GET");
                    con.setConnectTimeout(2500);
                    con.setReadTimeout(5000);
                    con.setRequestProperty("Authorization", "Basic " + Base64.encodeToString((user + ":" + pass).getBytes(StandardCharsets.UTF_8), Base64.NO_WRAP));
                    int code = con.getResponseCode();
                    InputStream in = code >= 200 && code < 300 ? con.getInputStream() : con.getErrorStream();
                    String body = in == null ? "" : readAll(in);
                    con.disconnect();
                    if (code >= 200 && code < 300) {
                        activeHttpBaseUrl = base;
                        main.post(() -> applyState(body));
                        return;
                    }
                    lastError = "Status HTTP " + code;
                } catch (Exception e) {
                    lastError = "Getrennt: " + e.getMessage();
                }
            }
            String message = lastError == null ? "Getrennt" : lastError;
            main.post(() -> markDisconnectedIfStale(message));
        }).start();
    }

    private void markDisconnectedIfStale(String message) {
        long last = lastStateMs;
        long now = System.currentTimeMillis();
        if (last == 0 && now - connectStartedMs < INITIAL_CONNECT_GRACE_MS) return;
        if (last == 0 || now - last > STATE_STALE_MS) {
            setConnected(false);
            if (status != null) {
                status.setVisibility(View.VISIBLE);
                status.setText(message);
            }
            showError(message);
            if (!hasSuccessfulConnection && !currentConnectionReceivedState) showSettings();
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
        StringBuilder url = new StringBuilder(activeHttpBaseUrl != null ? activeHttpBaseUrl : httpBaseUrls()[0]);
        url.append("/api/cmd?cmd=").append(enc(cmd.optString("cmd")));
        if (cmd.has("ch")) url.append("&ch=").append(cmd.optInt("ch"));
        if (cmd.has("val")) url.append("&val=").append(cmd.optBoolean("val") ? "1" : "0");
        return url.toString();
    }

    private String[] httpBaseUrls() {
        URI uri = URI.create(host.contains("://") ? host : "http://" + host);
        String scheme = uri.getScheme() == null ? "http" : uri.getScheme().toLowerCase(Locale.US);
        boolean tls = "https".equals(scheme) || "wss".equals(scheme);
        String h = uri.getHost();
        int port = uri.getPort();
        if (host.contains("://")) {
            return new String[] { buildBaseUrl(tls, h, port) };
        }
        return new String[] {
                buildBaseUrl(true, h, port),
                buildBaseUrl(false, h, port)
        };
    }

    private String buildBaseUrl(boolean tls, String h, int port) {
        StringBuilder url = new StringBuilder();
        url.append(tls ? "https" : "http").append("://").append(h);
        if (port > 0) url.append(":").append(port);
        return url.toString();
    }

    private static String enc(String value) throws Exception {
        return URLEncoder.encode(value, "UTF-8");
    }

    private static String readAll(InputStream in) throws Exception {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[1024];
        int n;
        while ((n = in.read(buf)) >= 0) out.write(buf, 0, n);
        return out.toString("UTF-8");
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

    private void showError(String message) {
        if (message == null || message.length() == 0) return;
        if (errorDialogOpen || message.equals(lastErrorDialogMessage)) return;
        lastErrorDialogMessage = message;
        errorDialogOpen = true;

        TextView text = new TextView(this);
        text.setText(message);
        text.setTextIsSelectable(true);
        text.setTextSize(14);
        text.setPadding(dp(18), dp(10), dp(18), 0);

        new AlertDialog.Builder(this)
                .setTitle("Verbindungsfehler")
                .setView(text)
                .setPositiveButton("OK", (d, w) -> errorDialogOpen = false)
                .setNeutralButton("Kopieren", (d, w) -> {
                    ClipboardManager cb = (ClipboardManager)getSystemService(Context.CLIPBOARD_SERVICE);
                    if (cb != null) cb.setPrimaryClip(ClipData.newPlainText("HS-IO Fehler", message));
                    Toast.makeText(this, "Fehler kopiert", Toast.LENGTH_SHORT).show();
                    errorDialogOpen = false;
                })
                .setOnCancelListener(d -> errorDialogOpen = false)
                .show();
    }

    private EditText edit(String hint, String value, int type) {
        EditText e = new EditText(this);
        e.setHint(hint);
        e.setText(value);
        e.setInputType(type);
        return e;
    }

    private String appVersion() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "1.0";
        }
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

    private StateListDrawable buttonBg(boolean taster) {
        int base = taster ? Color.rgb(220, 38, 38) : Color.rgb(14, 165, 233);
        int pressed = taster ? Color.rgb(127, 29, 29) : Color.rgb(3, 105, 161);
        int disabled = Color.rgb(71, 85, 105);

        StateListDrawable states = new StateListDrawable();
        states.addState(new int[] { -android.R.attr.state_enabled }, roundedButton(disabled, Color.rgb(51, 65, 85), 1, 0));
        states.addState(new int[] { android.R.attr.state_pressed }, roundedButton(pressed, Color.rgb(15, 23, 42), 2, 3));
        states.addState(new int[] { }, roundedButton(base, Color.argb(150, 255, 255, 255), 1, 0));
        return states;
    }

    private LayerDrawable roundedButton(int fill, int stroke, int strokeDp, int topInsetDp) {
        GradientDrawable shadow = new GradientDrawable();
        shadow.setColor(Color.argb(90, 0, 0, 0));
        shadow.setCornerRadius(dp(9));

        GradientDrawable face = new GradientDrawable();
        face.setColor(fill);
        face.setCornerRadius(dp(9));
        face.setStroke(dp(strokeDp), stroke);

        LayerDrawable layer = new LayerDrawable(new android.graphics.drawable.Drawable[] { shadow, face });
        layer.setLayerInset(0, 0, dp(topInsetDp + 2), 0, 0);
        layer.setLayerInset(1, 0, dp(topInsetDp), 0, dp(topInsetDp == 0 ? 2 : 0));
        return layer;
    }

    private int dp(int v) {
        return (int) (v * getResources().getDisplayMetrics().density + 0.5f);
    }

    private int statusBarHeight() {
        int id = getResources().getIdentifier("status_bar_height", "dimen", "android");
        return id > 0 ? getResources().getDimensionPixelSize(id) : 0;
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
                URI uri = URI.create(hostText.contains("://") ? hostText : "wss://" + hostText + "/ws");
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
