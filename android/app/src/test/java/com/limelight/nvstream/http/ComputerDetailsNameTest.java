package com.limelight.nvstream.http;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;

import org.junit.Test;

public class ComputerDetailsNameTest {
    @Test
    public void ordinaryNamesAreKept() {
        assertEquals("Living Room PC", ComputerDetails.sanitizeName("Living Room PC"));
        assertEquals("Oyun Bilgisayar\u0131", ComputerDetails.sanitizeName("Oyun Bilgisayar\u0131"));
        assertNull(ComputerDetails.sanitizeName(null));
    }

    @Test
    public void controlAndFormattingCharactersAreRemoved() {
        // A right-to-left override would make the following text read backwards
        assertEquals("PCexe.txt", ComputerDetails.sanitizeName("PC\u202Eexe.txt"));
        assertEquals("AB", ComputerDetails.sanitizeName("A\r\n\u0000\u2028B"));
        assertEquals("Host", ComputerDetails.sanitizeName("  Host\t"));
    }

    @Test
    public void longNamesAreCut() {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < 2 * 1024 * 1024; i++) {
            sb.append('x');
        }
        assertEquals(ComputerDetails.MAX_NAME_LENGTH, ComputerDetails.sanitizeName(sb.toString()).length());

        // Characters outside the BMP count once and are never split
        StringBuilder emoji = new StringBuilder();
        for (int i = 0; i < 100; i++) {
            emoji.appendCodePoint(0x1F3AE);
        }
        String cut = ComputerDetails.sanitizeName(emoji.toString());
        assertEquals(ComputerDetails.MAX_NAME_LENGTH, cut.codePointCount(0, cut.length()));
    }
}
