package de.hsio.app;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.media.AudioAttributes;
import android.media.AudioManager;
import android.media.SoundPool;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.webkit.HttpAuthHandler;
import android.webkit.JavascriptInterface;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.net.URI;
import java.util.Locale;

public class MainActivity extends Activity {
    private final Handler main = new Handler(Looper.getMainLooper());

    private SharedPreferences prefs;
    private WebView webView;
    private TextView status;
    private TextView settings;
    private String host;
    private String user;
    private String pass;
    private boolean settingsDialogOpen;
    private SoundPool soundPool;
    private int clickOnSoundId;
    private int clickOffSoundId;
    private boolean clickOnSoundLoaded;
    private boolean clickOffSoundLoaded;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        prefs = getSharedPreferences("hsio", MODE_PRIVATE);
        host = prefs.getString("host", "hs-io.local").trim();
        user = prefs.getString("user", "admin").trim();
        pass = prefs.getString("pass", "admin");
        setupSound();
        buildUi();
        loadWeb();
    }

    @Override
    protected void onDestroy() {
        if (webView != null) {
            webView.destroy();
            webView = null;
        }
        if (soundPool != null) {
            soundPool.release();
            soundPool = null;
        }
        super.onDestroy();
    }

    @Override
    public void onBackPressed() {
        if (webView != null && webView.canGoBack()) {
            webView.goBack();
            return;
        }
        super.onBackPressed();
    }

    @SuppressLint({"SetJavaScriptEnabled", "AddJavascriptInterface"})
    private void buildUi() {
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.rgb(15, 23, 42));

        webView = new WebView(this);
        WebSettings ws = webView.getSettings();
        ws.setJavaScriptEnabled(true);
        ws.setDomStorageEnabled(true);
        ws.setDatabaseEnabled(true);
        ws.setMediaPlaybackRequiresUserGesture(false);
        ws.setCacheMode(WebSettings.LOAD_DEFAULT);
        webView.addJavascriptInterface(new AndroidBridge(), "HSIOAndroid");
        webView.setWebViewClient(new HsioWebViewClient());
        root.addView(webView, new FrameLayout.LayoutParams(-1, -1));

        status = new TextView(this);
        status.setTextColor(Color.WHITE);
        status.setTextSize(13);
        status.setGravity(Gravity.CENTER);
        status.setBackgroundColor(Color.rgb(30, 41, 59));
        status.setPadding(dp(12), dp(8), dp(12), dp(8));
        status.setVisibility(View.GONE);
        FrameLayout.LayoutParams statusLp = new FrameLayout.LayoutParams(-1, -2, Gravity.BOTTOM);
        root.addView(status, statusLp);

        settings = new TextView(this);
        settings.setText("\u2699");
        settings.setTextColor(Color.WHITE);
        settings.setTextSize(18);
        settings.setGravity(Gravity.CENTER);
        settings.setBackgroundColor(Color.argb(170, 15, 23, 42));
        settings.setOnClickListener(v -> showSettings());
        FrameLayout.LayoutParams settingsLp = new FrameLayout.LayoutParams(dp(44), dp(44), Gravity.TOP | Gravity.END);
        settingsLp.setMargins(0, statusBarHeight() + dp(6), dp(6), 0);
        root.addView(settings, settingsLp);

        setContentView(root);
    }

    private void loadWeb() {
        if (webView == null) return;
        String url = normalizeUrl(host);
        showStatus("Lade " + url);
        webView.loadUrl(url);
    }

    private String normalizeUrl(String value) {
        String text = value == null ? "" : value.trim();
        if (text.length() == 0) text = "hs-io.local";
        URI uri = URI.create(text.contains("://") ? text : "http://" + text);
        String scheme = uri.getScheme() == null ? "http" : uri.getScheme().toLowerCase(Locale.US);
        String hostPart = uri.getHost();
        if (hostPart == null || hostPart.length() == 0) return "http://hs-io.local/";
        StringBuilder out = new StringBuilder();
        out.append(scheme).append("://").append(hostPart);
        if (uri.getPort() > 0) out.append(':').append(uri.getPort());
        String path = uri.getRawPath();
        out.append(path == null || path.length() == 0 ? "/" : path);
        return out.toString();
    }

    private void showStatus(String text) {
        if (status == null) return;
        status.setText(text);
        status.setVisibility(View.VISIBLE);
    }

    private void hideStatus() {
        if (status != null) status.setVisibility(View.GONE);
    }

    private void showSettings() {
        if (settingsDialogOpen) return;
        settingsDialogOpen = true;

        LinearLayout form = new LinearLayout(this);
        form.setOrientation(LinearLayout.VERTICAL);
        form.setPadding(dp(18), dp(8), dp(18), 0);
        EditText hostEt = edit("Host/IP oder URL", host, InputType.TYPE_CLASS_TEXT);
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
                    prefs.edit().putString("host", host).putString("user", user).putString("pass", pass).apply();
                    loadWeb();
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

    private String appVersion() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "2.0";
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
        clickOnSoundId = loadClickSound("click-on.wav", 1900.0);
        clickOffSoundId = loadClickSound("click-off.wav", 850.0);
    }

    private int loadClickSound(String name, double frequency) {
        try {
            File file = new File(getCacheDir(), name);
            byte[] wav = createClickWav(frequency);
            FileOutputStream out = new FileOutputStream(file);
            out.write(wav);
            out.close();
            return soundPool.load(file.getAbsolutePath(), 1);
        } catch (Exception e) {
            return 0;
        }
    }

    private void playClick(boolean on) {
        if (soundPool == null) return;
        int id = on ? clickOnSoundId : clickOffSoundId;
        boolean loaded = on ? clickOnSoundLoaded : clickOffSoundLoaded;
        if (id != 0 && loaded) {
            soundPool.play(id, 0.65f, 0.65f, 1, 0, 1.0f);
            return;
        }
        AudioManager audio = (AudioManager)getSystemService(Context.AUDIO_SERVICE);
        if (audio != null) audio.playSoundEffect(on ? AudioManager.FX_KEY_CLICK : AudioManager.FX_KEYPRESS_DELETE);
    }

    private byte[] createClickWav(double frequency) throws Exception {
        int sampleRate = 44100;
        int samples = (int)(sampleRate * 0.055);
        ByteArrayOutputStream pcm = new ByteArrayOutputStream();
        for (int i = 0; i < samples; i++) {
            double t = i / (double)sampleRate;
            double envelope = Math.exp(-t * 55.0);
            short value = (short)(Math.sin(2.0 * Math.PI * frequency * t) * envelope * 28000.0);
            write16(pcm, value);
        }

        byte[] data = pcm.toByteArray();
        ByteArrayOutputStream wav = new ByteArrayOutputStream();
        writeAscii(wav, "RIFF");
        write32(wav, 36 + data.length);
        writeAscii(wav, "WAVEfmt ");
        write32(wav, 16);
        write16(wav, (short)1);
        write16(wav, (short)1);
        write32(wav, sampleRate);
        write32(wav, sampleRate * 2);
        write16(wav, (short)2);
        write16(wav, (short)16);
        writeAscii(wav, "data");
        write32(wav, data.length);
        wav.write(data);
        return wav.toByteArray();
    }

    private static void writeAscii(ByteArrayOutputStream out, String s) {
        for (int i = 0; i < s.length(); i++) out.write((byte)s.charAt(i));
    }

    private static void write16(ByteArrayOutputStream out, short value) {
        out.write(value & 0xff);
        out.write((value >> 8) & 0xff);
    }

    private static void write32(ByteArrayOutputStream out, int value) {
        out.write(value & 0xff);
        out.write((value >> 8) & 0xff);
        out.write((value >> 16) & 0xff);
        out.write((value >> 24) & 0xff);
    }

    private int dp(int v) {
        return (int)(v * getResources().getDisplayMetrics().density + 0.5f);
    }

    private int statusBarHeight() {
        int id = getResources().getIdentifier("status_bar_height", "dimen", "android");
        return id > 0 ? getResources().getDimensionPixelSize(id) : 0;
    }

    private final class AndroidBridge {
        @JavascriptInterface
        public void click(boolean on) {
            main.post(() -> playClick(on));
        }

        @JavascriptInterface
        public String appVersion() {
            return MainActivity.this.appVersion();
        }

        @JavascriptInterface
        public void settings() {
            main.post(() -> showSettings());
        }
    }

    private final class HsioWebViewClient extends WebViewClient {
        @Override
        public void onPageFinished(WebView view, String url) {
            hideStatus();
        }

        @Override
        public void onReceivedHttpAuthRequest(WebView view, HttpAuthHandler handler, String hostName, String realm) {
            handler.proceed(user, pass);
        }

        @Override
        public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
            if (request != null && request.isForMainFrame()) {
                showStatus("Verbindung fehlgeschlagen. Oben rechts die Verbindung einstellen.");
            }
        }
    }
}
