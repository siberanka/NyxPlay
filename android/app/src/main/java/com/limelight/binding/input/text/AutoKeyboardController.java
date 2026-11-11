package com.limelight.binding.input.text;

/**
 * Shows the on-screen keyboard automatically when the user taps a text field on the host.
 * <p>
 * NyxPlay Host reports whenever an editable text field gains or loses keyboard focus. The
 * keyboard is only opened when that focus change follows a recent tap of this client, so
 * applications that move focus by themselves (or other users) do not pop up the keyboard. Tapping
 * an already focused text field again re-opens the keyboard if the user dismissed it.
 * <p>
 * All timing and UI work is delegated to {@link Host}, which keeps this class unit testable.
 */
public final class AutoKeyboardController {

    /** How long after a tap a host focus change is attributed to that tap. */
    public static final long TAP_ATTRIBUTION_WINDOW_MS = 1500;
    /** Delay before re-opening the keyboard after tapping an already focused field. */
    public static final long REFOCUS_SHOW_DELAY_MS = 300;

    public interface Host {
        long uptimeMillis();

        void showKeyboard();

        void hideKeyboard();

        boolean isKeyboardVisible();

        /** True if a hardware keyboard is attached (the soft keyboard is not needed then). */
        boolean hasHardwareKeyboard();

        void postDelayed(Runnable runnable, long delayMs);

        void removeCallbacks(Runnable runnable);
    }

    private final Host host;

    private boolean enabled;
    private boolean hostTextFocus;
    private int hostFlags;
    private boolean hostReportsFocus;
    private long lastTapUptime = Long.MIN_VALUE / 2;
    private boolean shownAutomatically;

    private final Runnable pendingRefocusShow = new Runnable() {
        @Override
        public void run() {
            if (enabled && hostTextFocus && !host.isKeyboardVisible() && !host.hasHardwareKeyboard()) {
                show();
            }
        }
    };

    public AutoKeyboardController(Host host, boolean enabled) {
        this.host = host;
        this.enabled = enabled;
    }

    public void setEnabled(boolean enabled) {
        this.enabled = enabled;
        if (!enabled) {
            host.removeCallbacks(pendingRefocusShow);
        }
    }

    public boolean isEnabled() {
        return enabled;
    }

    /** True once NyxPlay Host reported at least one text focus state. */
    public boolean hostReportsFocus() {
        return hostReportsFocus;
    }

    public boolean isHostTextFocused() {
        return hostTextFocus;
    }

    public int getHostFlags() {
        return hostFlags;
    }

    /** Current host focus as seen by the keyboard translator. */
    public SmartKeyboardTranslator.HostFocus getHostFocus() {
        if (!hostReportsFocus) {
            return SmartKeyboardTranslator.HostFocus.UNKNOWN;
        }
        return hostTextFocus ? SmartKeyboardTranslator.HostFocus.TEXT_FIELD
                : SmartKeyboardTranslator.HostFocus.NO_TEXT_FIELD;
    }

    /** Called (on the UI thread) when the host reports a new text focus state. */
    public void onHostTextInputState(boolean active, int flags) {
        hostReportsFocus = true;
        boolean wasActive = hostTextFocus;
        hostTextFocus = active;
        hostFlags = active ? flags : 0;

        if (active) {
            boolean recentTap = host.uptimeMillis() - lastTapUptime <= TAP_ATTRIBUTION_WINDOW_MS;
            if (enabled && recentTap && !host.hasHardwareKeyboard() && !host.isKeyboardVisible()) {
                host.removeCallbacks(pendingRefocusShow);
                show();
            }
        } else {
            host.removeCallbacks(pendingRefocusShow);
            if (wasActive && shownAutomatically && host.isKeyboardVisible()) {
                host.hideKeyboard();
            }
            shownAutomatically = false;
        }
    }

    /** Called when the user taps / clicks on the stream. */
    public void onUserTap() {
        lastTapUptime = host.uptimeMillis();
        if (enabled && hostTextFocus && !host.isKeyboardVisible() && !host.hasHardwareKeyboard()) {
            // Tapping an already focused field does not change the host focus, so re-open the
            // keyboard ourselves. Delay it a little: if the tap moved focus away from the field
            // the host reports that first and the pending show is cancelled.
            host.removeCallbacks(pendingRefocusShow);
            host.postDelayed(pendingRefocusShow, REFOCUS_SHOW_DELAY_MS);
        }
    }

    /** Called when the keyboard visibility changes (any reason). */
    public void onKeyboardVisibilityChanged(boolean visible) {
        if (!visible) {
            shownAutomatically = false;
            host.removeCallbacks(pendingRefocusShow);
        }
    }

    /** Called when the user toggles the keyboard manually. */
    public void onManualToggle() {
        shownAutomatically = false;
        host.removeCallbacks(pendingRefocusShow);
    }

    /** Called when the stream stops. */
    public void reset() {
        host.removeCallbacks(pendingRefocusShow);
        hostReportsFocus = false;
        hostTextFocus = false;
        hostFlags = 0;
        shownAutomatically = false;
    }

    private void show() {
        shownAutomatically = true;
        host.showKeyboard();
    }
}
