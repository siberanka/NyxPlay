package com.limelight.binding.input.text;

import android.content.Context;
import android.content.res.Resources;
import android.os.Build;
import android.util.SparseBooleanArray;
import android.util.SparseIntArray;
import android.view.InputDevice;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.inputmethod.InputMethodManager;
import android.view.inputmethod.InputMethodSubtype;

import com.limelight.LimeLog;
import com.limelight.binding.input.KeyboardTranslator;

import java.util.Locale;

/**
 * Android glue for {@link SmartKeyboardTranslator}: extracts the relevant information from
 * {@link KeyEvent}s, works out which layout a hardware keyboard uses and remembers which keys
 * were sent as text so their key-up events can be swallowed.
 */
public final class SmartKeyboard {

    /** Preference value for automatic layout detection. */
    public static final String LAYOUT_AUTO = "auto";

    private final Context context;
    private final KeyboardTranslator keyboardTranslator;
    private final SmartKeyboardTranslator.Mode mode;
    private final String layoutPreference;
    private final boolean rightAltIsShortcut;

    /** Cache: device id -> 1 if the key character map is the generic US map, 0 otherwise. */
    private final SparseIntArray genericMapCache = new SparseIntArray();
    /** Keys (by key code) whose down event was sent as text. */
    private final SparseBooleanArray keysSentAsText = new SparseBooleanArray();

    private String cachedAutoLayoutId;
    private long cachedAutoLayoutTime;

    public SmartKeyboard(Context context, KeyboardTranslator keyboardTranslator, String modePreference,
                         String layoutPreference, boolean rightAltIsShortcut) {
        this.context = context.getApplicationContext() != null ? context.getApplicationContext() : context;
        this.keyboardTranslator = keyboardTranslator;
        this.mode = SmartKeyboardTranslator.Mode.fromPreference(modePreference);
        this.layoutPreference = layoutPreference == null ? LAYOUT_AUTO : layoutPreference;
        this.rightAltIsShortcut = rightAltIsShortcut;
        LimeLog.info("Smart keyboard: mode=" + mode + ", layout=" + this.layoutPreference);
    }

    public SmartKeyboardTranslator.Mode getMode() {
        return mode;
    }

    /**
     * Returns the text to send for a key down event, or {@code null} if the key should be sent
     * as a virtual key code.
     */
    public String onKeyDown(KeyEvent event, SmartKeyboardTranslator.HostFocus hostFocus) {
        if (mode == SmartKeyboardTranslator.Mode.RAW) {
            return null;
        }

        int keyCode = event.getKeyCode();
        int deviceId = event.getDeviceId();
        int meta = event.getMetaState();

        SmartKeyboardTranslator.KeyInput in = new SmartKeyboardTranslator.KeyInput();
        in.effectiveKeyCode = keyboardTranslator.getEffectiveKeyCode(keyCode, deviceId);
        in.positionKeyCode = keyboardTranslator.getQwertyPositionKeyCode(keyCode, deviceId);
        in.scanCode = event.getScanCode();
        in.shift = (meta & KeyEvent.META_SHIFT_ON) != 0;
        in.capsLock = (meta & KeyEvent.META_CAPS_LOCK_ON) != 0;
        boolean rightAlt = (meta & KeyEvent.META_ALT_RIGHT_ON) != 0;
        in.altGr = rightAlt && !rightAltIsShortcut;
        in.shortcutModifier = (meta & (KeyEvent.META_CTRL_ON | KeyEvent.META_META_ON | KeyEvent.META_ALT_LEFT_ON)) != 0
                || (rightAlt && rightAltIsShortcut);
        in.softKeyboard = isSoftKeyboardEvent(event);

        // Character according to Android's own idea of the layout, without shortcut modifiers
        int charMeta = meta & ~(KeyEvent.META_CTRL_MASK | KeyEvent.META_META_MASK);
        in.deviceChar = event.getUnicodeChar(charMeta);
        in.genericKeyCharacterMap = !in.softKeyboard && isGenericKeyCharacterMap(event);

        boolean forceLayout = !LAYOUT_AUTO.equals(layoutPreference);
        KeyboardLayouts.Layout layout = forceLayout ? KeyboardLayouts.get(layoutPreference)
                : (in.genericKeyCharacterMap ? KeyboardLayouts.get(resolveAutoLayoutId(event)) : null);

        String text = SmartKeyboardTranslator.translate(in, mode, layout, forceLayout, hostFocus);
        if (text != null) {
            keysSentAsText.put(keyCode, true);
        }
        return text;
    }

    /** Returns true if the key-up event belongs to a key that was sent as text. */
    public boolean onKeyUp(KeyEvent event) {
        int keyCode = event.getKeyCode();
        if (keysSentAsText.get(keyCode, false)) {
            keysSentAsText.delete(keyCode);
            return true;
        }
        return false;
    }

    public void onInputDeviceChanged(int deviceId) {
        genericMapCache.delete(deviceId);
    }

    static boolean isSoftKeyboardEvent(KeyEvent event) {
        if ((event.getFlags() & KeyEvent.FLAG_SOFT_KEYBOARD) != 0) {
            return true;
        }
        if (event.getDeviceId() == KeyCharacterMap.VIRTUAL_KEYBOARD) {
            return true;
        }
        InputDevice device = event.getDevice();
        return device == null || device.isVirtual();
    }

    private boolean isGenericKeyCharacterMap(KeyEvent event) {
        int deviceId = event.getDeviceId();
        int cached = genericMapCache.get(deviceId, -1);
        if (cached >= 0) {
            return cached == 1;
        }

        boolean generic = false;
        KeyCharacterMap kcm = event.getKeyCharacterMap();
        if (kcm != null) {
            generic = kcm.get(KeyEvent.KEYCODE_LEFT_BRACKET, 0) == '['
                    && kcm.get(KeyEvent.KEYCODE_RIGHT_BRACKET, 0) == ']'
                    && kcm.get(KeyEvent.KEYCODE_SEMICOLON, 0) == ';'
                    && kcm.get(KeyEvent.KEYCODE_APOSTROPHE, 0) == '\''
                    && kcm.get(KeyEvent.KEYCODE_COMMA, 0) == ','
                    && kcm.get(KeyEvent.KEYCODE_I, 0) == 'i'
                    && kcm.get(KeyEvent.KEYCODE_Y, 0) == 'y'
                    && kcm.get(KeyEvent.KEYCODE_Q, 0) == 'q';
        }
        genericMapCache.put(deviceId, generic ? 1 : 0);
        if (generic) {
            LimeLog.info("Smart keyboard: device " + deviceId + " uses the generic US key map, layout will be inferred");
        }
        return generic;
    }

    /**
     * Layout for a keyboard whose layout Android does not know: the language of the active
     * input method, then the system language.
     */
    private String resolveAutoLayoutId(KeyEvent event) {
        long now = android.os.SystemClock.uptimeMillis();
        if (cachedAutoLayoutId != null && now - cachedAutoLayoutTime < 5000) {
            return cachedAutoLayoutId;
        }

        // An English input method is often only a secondary/default subtype, so it does not
        // override a non-US system language (e.g. a Turkish keyboard on a Turkish device).
        String id = KeyboardLayouts.layoutIdForLanguage(currentImeLanguage());
        if (KeyboardLayouts.ID_US.equals(id)) {
            id = null;
        }

        if (id == null) {
            Locale system = Build.VERSION.SDK_INT >= Build.VERSION_CODES.N
                    ? Resources.getSystem().getConfiguration().getLocales().get(0)
                    : Resources.getSystem().getConfiguration().locale;
            id = KeyboardLayouts.layoutIdForLanguage(system != null ? system.toLanguageTag() : null);
        }

        if (id == null) {
            id = KeyboardLayouts.ID_US;
        }

        if (!id.equals(cachedAutoLayoutId)) {
            LimeLog.info("Smart keyboard: inferred physical layout " + id);
        }
        cachedAutoLayoutId = id;
        cachedAutoLayoutTime = now;
        return id;
    }

    private String currentImeLanguage() {
        try {
            InputMethodManager imm = (InputMethodManager) context.getSystemService(Context.INPUT_METHOD_SERVICE);
            if (imm == null) {
                return null;
            }
            InputMethodSubtype subtype = imm.getCurrentInputMethodSubtype();
            if (subtype == null) {
                return null;
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
                String tag = subtype.getLanguageTag();
                if (tag != null && !tag.isEmpty()) {
                    return tag;
                }
            }
            //noinspection deprecation
            return subtype.getLocale();
        } catch (RuntimeException e) {
            return null;
        }
    }
}
