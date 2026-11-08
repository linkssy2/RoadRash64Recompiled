package io.github.rayishere0.rr64probe;

import android.os.Bundle;
import android.text.method.ScrollingMovementMethod;
import android.util.Log;
import android.view.WindowManager;
import android.widget.ScrollView;
import android.widget.TextView;
import org.libsdl.app.SDLActivity;

/**
 * RT64 probe activity. The native side (SDL_main in probe.cpp) runs the
 * whole test and calls appendProbeLine() for every result line; this class
 * only shows those lines on screen in large readable text (Ray debugs via
 * screenshots) and keeps the screen on while the probe runs.
 *
 * No ROM, no game code, no touch controls: the probe is a diagnostics
 * program, not a game shell.
 */
public final class ProbeActivity extends SDLActivity {
    private static final String TAG = "RR64Probe";
    private TextView reportView;
    private ScrollView scroller;

    @Override protected String[] getLibraries() {
        return new String[]{"c++_shared", "SDL2", "main"};
    }

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (mLayout == null) return;
        reportView = new TextView(this);
        reportView.setTextSize(17);
        reportView.setTextColor(0xFFFFFFFF);
        reportView.setBackgroundColor(0xCC000000);
        reportView.setTypeface(android.graphics.Typeface.MONOSPACE);
        reportView.setMovementMethod(new ScrollingMovementMethod());
        reportView.setPadding(24, 24, 24, 24);
        reportView.setText("RR64 RT64 PROBE\nStarting...\n");
        scroller = new ScrollView(this);
        scroller.addView(reportView);
        mLayout.addView(scroller, new android.widget.RelativeLayout.LayoutParams(-1, -1));
        immersive();
    }

    private void immersive() {
        getWindow().getDecorView().setSystemUiVisibility(
            android.view.View.SYSTEM_UI_FLAG_FULLSCREEN
            | android.view.View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
            | android.view.View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
            | android.view.View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
            | android.view.View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
            | android.view.View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }

    @Override public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus) immersive();
    }

    /** Called from native code (probe.cpp) for every report line. */
    public void appendProbeLine(String line) {
        Log.i(TAG, line);
        runOnUiThread(() -> {
            if (reportView == null) return;
            if (reportView.length() > 20000) reportView.setText("");
            reportView.append(line + "\n");
            scroller.post(() -> scroller.fullScroll(android.view.View.FOCUS_DOWN));
        });
    }

    @Override public void onBackPressed() {
        // Back exits the probe; the native loop sees SDL_QUIT and shuts down.
        finish();
    }
}
