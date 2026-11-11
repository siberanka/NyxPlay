package com.limelight.binding.input.text;

import org.junit.Before;
import org.junit.Test;

import java.util.ArrayList;
import java.util.List;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

public class AutoKeyboardControllerTest {

    /** Deterministic host: manual clock, synchronous scheduler. */
    private static final class FakeHost implements AutoKeyboardController.Host {
        long now = 10_000;
        boolean visible;
        boolean hardwareKeyboard;
        int shows;
        int hides;
        final List<Runnable> pending = new ArrayList<>();
        final List<Long> pendingAt = new ArrayList<>();

        @Override
        public long uptimeMillis() {
            return now;
        }

        @Override
        public void showKeyboard() {
            shows++;
            visible = true;
        }

        @Override
        public void hideKeyboard() {
            hides++;
            visible = false;
        }

        @Override
        public boolean isKeyboardVisible() {
            return visible;
        }

        @Override
        public boolean hasHardwareKeyboard() {
            return hardwareKeyboard;
        }

        @Override
        public void postDelayed(Runnable runnable, long delayMs) {
            pending.add(runnable);
            pendingAt.add(now + delayMs);
        }

        @Override
        public void removeCallbacks(Runnable runnable) {
            int i;
            while ((i = pending.indexOf(runnable)) >= 0) {
                pending.remove(i);
                pendingAt.remove(i);
            }
        }

        void advance(long ms) {
            now += ms;
            for (int i = 0; i < pending.size(); ) {
                if (pendingAt.get(i) <= now) {
                    Runnable r = pending.remove(i);
                    pendingAt.remove(i);
                    r.run();
                } else {
                    i++;
                }
            }
        }
    }

    private FakeHost host;
    private AutoKeyboardController controller;

    @Before
    public void setUp() {
        host = new FakeHost();
        controller = new AutoKeyboardController(host, true);
    }

    @Test
    public void tapOnTextFieldOpensKeyboard() {
        controller.onUserTap();
        host.advance(120);
        controller.onHostTextInputState(true, 0);
        assertEquals(1, host.shows);
        assertTrue(host.visible);
    }

    @Test
    public void focusChangeWithoutRecentTapDoesNotOpenKeyboard() {
        controller.onUserTap();
        host.advance(AutoKeyboardController.TAP_ATTRIBUTION_WINDOW_MS + 1);
        controller.onHostTextInputState(true, 0);
        assertEquals(0, host.shows);
    }

    @Test
    public void keyboardClosesWhenFieldLosesFocus() {
        controller.onUserTap();
        controller.onHostTextInputState(true, 0);
        controller.onHostTextInputState(false, 0);
        assertEquals(1, host.hides);
        assertFalse(host.visible);
    }

    @Test
    public void manuallyOpenedKeyboardIsNotClosedAutomatically() {
        host.visible = true; // opened by the user
        controller.onManualToggle();
        controller.onHostTextInputState(true, 0);
        controller.onHostTextInputState(false, 0);
        assertEquals(0, host.hides);
        assertTrue(host.visible);
    }

    @Test
    public void tappingFocusedFieldAgainReopensKeyboard() {
        controller.onUserTap();
        controller.onHostTextInputState(true, 0);
        // User dismisses the keyboard with the back button
        host.visible = false;
        controller.onKeyboardVisibilityChanged(false);

        controller.onUserTap();
        host.advance(AutoKeyboardController.REFOCUS_SHOW_DELAY_MS - 1);
        assertEquals(1, host.shows);
        host.advance(1);
        assertEquals(2, host.shows);
    }

    @Test
    public void tapThatMovesFocusAwayCancelsPendingReopen() {
        controller.onUserTap();
        controller.onHostTextInputState(true, 0);
        host.visible = false;
        controller.onKeyboardVisibilityChanged(false);

        controller.onUserTap();
        host.advance(100);
        controller.onHostTextInputState(false, 0); // the tap hit a button
        host.advance(1000);
        assertEquals(1, host.shows);
    }

    @Test
    public void hardwareKeyboardSuppressesAutoShow() {
        host.hardwareKeyboard = true;
        controller.onUserTap();
        controller.onHostTextInputState(true, 0);
        assertEquals(0, host.shows);
    }

    @Test
    public void disabledControllerNeverShows() {
        controller.setEnabled(false);
        controller.onUserTap();
        controller.onHostTextInputState(true, 0);
        assertEquals(0, host.shows);
    }

    @Test
    public void hostFocusIsExposedForTheTranslator() {
        assertEquals(SmartKeyboardTranslator.HostFocus.UNKNOWN, controller.getHostFocus());
        controller.onHostTextInputState(true, 1);
        assertEquals(SmartKeyboardTranslator.HostFocus.TEXT_FIELD, controller.getHostFocus());
        assertEquals(1, controller.getHostFlags());
        controller.onHostTextInputState(false, 1);
        assertEquals(SmartKeyboardTranslator.HostFocus.NO_TEXT_FIELD, controller.getHostFocus());
        assertEquals(0, controller.getHostFlags());
        controller.reset();
        assertEquals(SmartKeyboardTranslator.HostFocus.UNKNOWN, controller.getHostFocus());
    }
}
