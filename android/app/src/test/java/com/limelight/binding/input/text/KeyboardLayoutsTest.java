package com.limelight.binding.input.text;

import android.view.KeyEvent;

import org.junit.Test;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;

public class KeyboardLayoutsTest {

    private static char ch(String layout, int keyCode, boolean shift) {
        return KeyboardLayouts.get(layout).charFor(keyCode, 0, shift, false, false);
    }

    @Test
    public void allLayoutsAreRegistered() {
        for (String id : new String[]{KeyboardLayouts.ID_US, KeyboardLayouts.ID_TR_Q, KeyboardLayouts.ID_TR_F,
                KeyboardLayouts.ID_DE, KeyboardLayouts.ID_FR, KeyboardLayouts.ID_ES, KeyboardLayouts.ID_IT}) {
            assertNotNull(id, KeyboardLayouts.get(id));
            assertEquals(id, KeyboardLayouts.get(id).getId());
        }
        assertNull(KeyboardLayouts.get("unknown"));
        assertNull(KeyboardLayouts.get(null));
    }

    @Test
    public void turkishQSpecialLetters() {
        assertEquals('ğ', ch(KeyboardLayouts.ID_TR_Q, KeyEvent.KEYCODE_LEFT_BRACKET, false));
        assertEquals('Ğ', ch(KeyboardLayouts.ID_TR_Q, KeyEvent.KEYCODE_LEFT_BRACKET, true));
        assertEquals('ü', ch(KeyboardLayouts.ID_TR_Q, KeyEvent.KEYCODE_RIGHT_BRACKET, false));
        assertEquals('ş', ch(KeyboardLayouts.ID_TR_Q, KeyEvent.KEYCODE_SEMICOLON, false));
        assertEquals('i', ch(KeyboardLayouts.ID_TR_Q, KeyEvent.KEYCODE_APOSTROPHE, false));
        assertEquals('İ', ch(KeyboardLayouts.ID_TR_Q, KeyEvent.KEYCODE_APOSTROPHE, true));
        assertEquals('ı', ch(KeyboardLayouts.ID_TR_Q, KeyEvent.KEYCODE_I, false));
        assertEquals('I', ch(KeyboardLayouts.ID_TR_Q, KeyEvent.KEYCODE_I, true));
        assertEquals('ö', ch(KeyboardLayouts.ID_TR_Q, KeyEvent.KEYCODE_COMMA, false));
        assertEquals('Ç', ch(KeyboardLayouts.ID_TR_Q, KeyEvent.KEYCODE_PERIOD, true));
        assertEquals('.', ch(KeyboardLayouts.ID_TR_Q, KeyEvent.KEYCODE_SLASH, false));
        assertEquals('"', ch(KeyboardLayouts.ID_TR_Q, KeyEvent.KEYCODE_GRAVE, false));
        assertEquals('\'', ch(KeyboardLayouts.ID_TR_Q, KeyEvent.KEYCODE_2, true));
        assertEquals('@', KeyboardLayouts.get(KeyboardLayouts.ID_TR_Q).charFor(KeyEvent.KEYCODE_Q, 0, false, false, true));
    }

    @Test
    public void turkishFRows() {
        assertEquals('f', ch(KeyboardLayouts.ID_TR_F, KeyEvent.KEYCODE_Q, false));
        assertEquals('ğ', ch(KeyboardLayouts.ID_TR_F, KeyEvent.KEYCODE_E, false));
        assertEquals('ı', ch(KeyboardLayouts.ID_TR_F, KeyEvent.KEYCODE_R, false));
        assertEquals('İ', ch(KeyboardLayouts.ID_TR_F, KeyEvent.KEYCODE_S, true));
        assertEquals('ş', ch(KeyboardLayouts.ID_TR_F, KeyEvent.KEYCODE_APOSTROPHE, false));
        assertEquals('ç', ch(KeyboardLayouts.ID_TR_F, KeyEvent.KEYCODE_B, false));
    }

    @Test
    public void capsLockOnlyAffectsLetters() {
        KeyboardLayouts.Layout tr = KeyboardLayouts.get(KeyboardLayouts.ID_TR_Q);
        assertEquals('Ş', tr.charFor(KeyEvent.KEYCODE_SEMICOLON, 0, false, true, false));
        assertEquals('ş', tr.charFor(KeyEvent.KEYCODE_SEMICOLON, 0, true, true, false));
        assertEquals('1', tr.charFor(KeyEvent.KEYCODE_1, 0, false, true, false));
    }

    @Test
    public void germanAndFrenchSwaps() {
        assertEquals('z', ch(KeyboardLayouts.ID_DE, KeyEvent.KEYCODE_Y, false));
        assertEquals('y', ch(KeyboardLayouts.ID_DE, KeyEvent.KEYCODE_Z, false));
        assertEquals('ß', ch(KeyboardLayouts.ID_DE, KeyEvent.KEYCODE_MINUS, false));
        assertEquals('a', ch(KeyboardLayouts.ID_FR, KeyEvent.KEYCODE_Q, false));
        assertEquals('z', ch(KeyboardLayouts.ID_FR, KeyEvent.KEYCODE_W, false));
        assertEquals('m', ch(KeyboardLayouts.ID_FR, KeyEvent.KEYCODE_SEMICOLON, false));
        assertEquals('é', ch(KeyboardLayouts.ID_FR, KeyEvent.KEYCODE_2, false));
        assertEquals('ñ', ch(KeyboardLayouts.ID_ES, KeyEvent.KEYCODE_SEMICOLON, false));
    }

    @Test
    public void languageMapping() {
        assertEquals(KeyboardLayouts.ID_TR_Q, KeyboardLayouts.layoutIdForLanguage("tr-TR"));
        assertEquals(KeyboardLayouts.ID_TR_Q, KeyboardLayouts.layoutIdForLanguage("tr_TR"));
        assertEquals(KeyboardLayouts.ID_TR_Q, KeyboardLayouts.layoutIdForLanguage("tr"));
        assertEquals(KeyboardLayouts.ID_DE, KeyboardLayouts.layoutIdForLanguage("de-AT"));
        assertEquals(KeyboardLayouts.ID_US, KeyboardLayouts.layoutIdForLanguage("en-GB"));
        assertNull(KeyboardLayouts.layoutIdForLanguage("ja-JP"));
        assertNull(KeyboardLayouts.layoutIdForLanguage(""));
        assertNull(KeyboardLayouts.layoutIdForLanguage(null));
    }
}
