package com.limelight.binding.input.text;

import android.view.KeyEvent;

import org.junit.Test;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;

/**
 * Pure JVM tests for the layout aware keyboard translation (Turkish keyboards in particular).
 */
public class SmartKeyboardTranslatorTest {

    private static final SmartKeyboardTranslator.HostFocus UNKNOWN = SmartKeyboardTranslator.HostFocus.UNKNOWN;
    private static final SmartKeyboardTranslator.HostFocus NO_TEXT = SmartKeyboardTranslator.HostFocus.NO_TEXT_FIELD;
    private static final SmartKeyboardTranslator.HostFocus TEXT = SmartKeyboardTranslator.HostFocus.TEXT_FIELD;

    private static SmartKeyboardTranslator.KeyInput key(int keyCode, int deviceChar) {
        SmartKeyboardTranslator.KeyInput in = new SmartKeyboardTranslator.KeyInput();
        in.effectiveKeyCode = keyCode;
        in.positionKeyCode = keyCode;
        in.deviceChar = deviceChar;
        return in;
    }

    private static String smart(SmartKeyboardTranslator.KeyInput in, SmartKeyboardTranslator.HostFocus focus) {
        return SmartKeyboardTranslator.translate(in, SmartKeyboardTranslator.Mode.SMART, null, false, focus);
    }

    // ---------------------------------------------------------------- Android knows the layout

    @Test
    public void turkishLettersFromAndroidLayoutAreSentAsText() {
        // Android's Turkish layout maps the US bracket/semicolon/comma/period keys to ğ ü ş ö ç
        assertEquals("ğ", smart(key(KeyEvent.KEYCODE_LEFT_BRACKET, 'ğ'), UNKNOWN));
        assertEquals("ü", smart(key(KeyEvent.KEYCODE_RIGHT_BRACKET, 'ü'), UNKNOWN));
        assertEquals("ş", smart(key(KeyEvent.KEYCODE_SEMICOLON, 'ş'), UNKNOWN));
        assertEquals("ö", smart(key(KeyEvent.KEYCODE_COMMA, 'ö'), UNKNOWN));
        assertEquals("ç", smart(key(KeyEvent.KEYCODE_PERIOD, 'ç'), UNKNOWN));
    }

    @Test
    public void dottedAndDotlessIAreSentAsText() {
        // I key types ı, the apostrophe key types i on Turkish Q
        assertEquals("ı", smart(key(KeyEvent.KEYCODE_I, 'ı'), UNKNOWN));
        assertEquals("i", smart(key(KeyEvent.KEYCODE_APOSTROPHE, 'i'), UNKNOWN));
        SmartKeyboardTranslator.KeyInput upper = key(KeyEvent.KEYCODE_APOSTROPHE, 'İ');
        upper.shift = true;
        assertEquals("İ", smart(upper, UNKNOWN));
    }

    @Test
    public void lettersMatchingUsLayoutStayVirtualKeys() {
        assertNull(smart(key(KeyEvent.KEYCODE_A, 'a'), UNKNOWN));
        assertNull(smart(key(KeyEvent.KEYCODE_W, 'w'), UNKNOWN));
        SmartKeyboardTranslator.KeyInput shifted = key(KeyEvent.KEYCODE_S, 'S');
        shifted.shift = true;
        assertNull(smart(shifted, UNKNOWN));
        assertNull(smart(key(KeyEvent.KEYCODE_5, '5'), UNKNOWN));
        assertNull(smart(key(KeyEvent.KEYCODE_SPACE, ' '), UNKNOWN));
    }

    @Test
    public void shiftedDigitSymbolsThatDifferAreSentAsText() {
        SmartKeyboardTranslator.KeyInput quote = key(KeyEvent.KEYCODE_2, '\'');
        quote.shift = true; // US layout would type @
        assertEquals("'", smart(quote, UNKNOWN));
    }

    @Test
    public void altGrCharactersAreSentAsText() {
        SmartKeyboardTranslator.KeyInput at = key(KeyEvent.KEYCODE_Q, '@');
        at.altGr = true;
        assertEquals("@", smart(at, UNKNOWN));
    }

    @Test
    public void shortcutsAlwaysUseVirtualKeys() {
        SmartKeyboardTranslator.KeyInput ctrl = key(KeyEvent.KEYCODE_LEFT_BRACKET, 'ğ');
        ctrl.shortcutModifier = true;
        assertNull(smart(ctrl, UNKNOWN));
    }

    @Test
    public void controlCharactersAndDeadKeysUseVirtualKeys() {
        assertNull(smart(key(KeyEvent.KEYCODE_ENTER, '\n'), UNKNOWN));
        assertNull(smart(key(KeyEvent.KEYCODE_TAB, '\t'), UNKNOWN));
        assertNull(smart(key(KeyEvent.KEYCODE_DEL, 0), UNKNOWN));
        assertNull(smart(key(KeyEvent.KEYCODE_LEFT_BRACKET, android.view.KeyCharacterMap.COMBINING_ACCENT | 0x0308), UNKNOWN));
    }

    @Test
    public void numpadKeysUseVirtualKeys() {
        assertNull(smart(key(KeyEvent.KEYCODE_NUMPAD_7, '7'), UNKNOWN));
        assertNull(smart(key(KeyEvent.KEYCODE_NUMPAD_ADD, '+'), UNKNOWN));
    }

    // ---------------------------------------------------------------- generic map + fallback

    @Test
    public void genericKeyMapFallsBackToTurkishLayout() {
        KeyboardLayouts.Layout tr = KeyboardLayouts.get(KeyboardLayouts.ID_TR_Q);
        SmartKeyboardTranslator.KeyInput in = key(KeyEvent.KEYCODE_LEFT_BRACKET, '[');
        in.genericKeyCharacterMap = true;
        assertEquals("ğ", SmartKeyboardTranslator.translate(in, SmartKeyboardTranslator.Mode.SMART, tr, false, UNKNOWN));

        SmartKeyboardTranslator.KeyInput dotless = key(KeyEvent.KEYCODE_I, 'i');
        dotless.genericKeyCharacterMap = true;
        assertEquals("ı", SmartKeyboardTranslator.translate(dotless, SmartKeyboardTranslator.Mode.SMART, tr, false, UNKNOWN));

        SmartKeyboardTranslator.KeyInput capsI = key(KeyEvent.KEYCODE_APOSTROPHE, '\'');
        capsI.genericKeyCharacterMap = true;
        capsI.capsLock = true;
        assertEquals("İ", SmartKeyboardTranslator.translate(capsI, SmartKeyboardTranslator.Mode.SMART, tr, false, UNKNOWN));
    }

    @Test
    public void fallbackLayoutIgnoredWhenAndroidKnowsTheLayout() {
        KeyboardLayouts.Layout tr = KeyboardLayouts.get(KeyboardLayouts.ID_TR_Q);
        SmartKeyboardTranslator.KeyInput in = key(KeyEvent.KEYCODE_LEFT_BRACKET, '[');
        in.genericKeyCharacterMap = false; // e.g. a real US keyboard configured in Android
        assertNull(SmartKeyboardTranslator.translate(in, SmartKeyboardTranslator.Mode.SMART, tr, false, UNKNOWN));
    }

    @Test
    public void forcedLayoutOverridesAndroidLayout() {
        KeyboardLayouts.Layout trF = KeyboardLayouts.get(KeyboardLayouts.ID_TR_F);
        SmartKeyboardTranslator.KeyInput in = key(KeyEvent.KEYCODE_Q, 'q');
        assertEquals("f", SmartKeyboardTranslator.translate(in, SmartKeyboardTranslator.Mode.SMART, trF, true, UNKNOWN));
    }

    @Test
    public void iso102ndKeyUsesScanCode() {
        KeyboardLayouts.Layout tr = KeyboardLayouts.get(KeyboardLayouts.ID_TR_Q);
        SmartKeyboardTranslator.KeyInput in = key(KeyEvent.KEYCODE_BACKSLASH, '\\');
        in.scanCode = KeyboardLayouts.SCANCODE_ISO_102ND;
        in.genericKeyCharacterMap = true;
        assertEquals("<", SmartKeyboardTranslator.translate(in, SmartKeyboardTranslator.Mode.SMART, tr, false, UNKNOWN));

        SmartKeyboardTranslator.KeyInput backslashKey = key(KeyEvent.KEYCODE_BACKSLASH, '\\');
        backslashKey.scanCode = 43;
        backslashKey.genericKeyCharacterMap = true;
        assertEquals(",", SmartKeyboardTranslator.translate(backslashKey, SmartKeyboardTranslator.Mode.SMART, tr, false, UNKNOWN));
    }

    // ---------------------------------------------------------------- host focus + soft keyboard

    @Test
    public void letterKeysStayPositionalWhenHostReportsNoTextField() {
        // AZERTY user pressing the key labelled Z (W position) in a game
        SmartKeyboardTranslator.KeyInput in = key(KeyEvent.KEYCODE_W, 'z');
        assertNull(smart(in, NO_TEXT));
        assertEquals("z", smart(in, TEXT));
        assertEquals("z", smart(in, UNKNOWN));
        // Non letter keys still produce correct characters
        assertEquals("ğ", smart(key(KeyEvent.KEYCODE_LEFT_BRACKET, 'ğ'), NO_TEXT));
    }

    @Test
    public void softKeyboardCharactersAreAlwaysText() {
        SmartKeyboardTranslator.KeyInput i = key(KeyEvent.KEYCODE_I, 'i');
        i.softKeyboard = true;
        // A Turkish host would type ı for VK_I, so on-screen keyboard text is sent as characters
        assertEquals("i", smart(i, UNKNOWN));
        assertEquals("i", smart(i, TEXT));
        // ...except for games: positional keys when no text field is focused
        assertNull(smart(i, NO_TEXT));
    }

    // ---------------------------------------------------------------- modes

    @Test
    public void rawModeNeverSendsText() {
        assertNull(SmartKeyboardTranslator.translate(key(KeyEvent.KEYCODE_LEFT_BRACKET, 'ğ'),
                SmartKeyboardTranslator.Mode.RAW, null, false, TEXT));
    }

    @Test
    public void unicodeModeSendsEveryPrintableCharacter() {
        assertEquals("a", SmartKeyboardTranslator.translate(key(KeyEvent.KEYCODE_A, 'a'),
                SmartKeyboardTranslator.Mode.UNICODE, null, false, NO_TEXT));
    }

    @Test
    public void modeFromPreference() {
        assertEquals(SmartKeyboardTranslator.Mode.SMART, SmartKeyboardTranslator.Mode.fromPreference(null));
        assertEquals(SmartKeyboardTranslator.Mode.SMART, SmartKeyboardTranslator.Mode.fromPreference("smart"));
        assertEquals(SmartKeyboardTranslator.Mode.UNICODE, SmartKeyboardTranslator.Mode.fromPreference("unicode"));
        assertEquals(SmartKeyboardTranslator.Mode.RAW, SmartKeyboardTranslator.Mode.fromPreference("raw"));
    }
}
