package com.limelight.utils;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public class DisplayTextTest {
    @Test
    public void nullAndBlankGiveNull() {
        assertNull(DisplayText.label(null));
        assertNull(DisplayText.label("   "));
        assertNull(DisplayText.label("\u0000\n\t"));
    }

    @Test
    public void plainNamesAreKept() {
        assertEquals("Living Room PC", DisplayText.label("  Living Room PC "));
        assertEquals("Oyun Bilgisayar\u0131", DisplayText.label("Oyun Bilgisayar\u0131"));
    }

    @Test
    public void controlAndDirectionCharactersAreDropped() {
        assertEquals("AB", DisplayText.label("A\nB"));
        assertEquals("PCgnp", DisplayText.label("PC\u202Egnp"));
        assertEquals("ab", DisplayText.label("a\u200Bb"));
    }

    @Test
    public void longValuesAreLimited() {
        StringBuilder in = new StringBuilder();
        for (int i = 0; i < 500; i++) {
            in.append('x');
        }
        String out = DisplayText.label(in.toString());
        assertEquals(DisplayText.MAX_LABEL_CHARS + 1, out.length());
        assertTrue(out.endsWith("\u2026"));
    }
}
