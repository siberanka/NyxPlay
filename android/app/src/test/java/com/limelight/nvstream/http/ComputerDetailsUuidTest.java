package com.limelight.nvstream.http;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public class ComputerDetailsUuidTest {
    @Test
    public void validUuids_accepted() {
        assertTrue(ComputerDetails.isValidUuid("3f2b1c4e-9a7d-4e21-8c55-0a1b2c3d4e5f"));
        assertTrue(ComputerDetails.isValidUuid("3F2B1C4E-9A7D-4E21-8C55-0A1B2C3D4E5F"));
        assertTrue(ComputerDetails.isValidUuid("0123456789ABCDEF"));
        assertTrue(ComputerDetails.isValidUuid(repeat('a', 64)));
        assertTrue(ComputerDetails.isValidUuid("x"));
    }

    @Test
    public void invalidUuids_rejected() {
        assertFalse(ComputerDetails.isValidUuid(null));
        assertFalse(ComputerDetails.isValidUuid(""));
        assertFalse(ComputerDetails.isValidUuid(repeat('a', 65)));
        assertFalse(ComputerDetails.isValidUuid(".."));
        assertFalse(ComputerDetails.isValidUuid("."));
        assertFalse(ComputerDetails.isValidUuid("a/b"));
        assertFalse(ComputerDetails.isValidUuid("a\\b"));
        assertFalse(ComputerDetails.isValidUuid("a b"));
        assertFalse(ComputerDetails.isValidUuid("çağ"));
        assertFalse(ComputerDetails.isValidUuid("ab\u0000cd"));
        assertFalse(ComputerDetails.isValidUuid("../../x"));
        assertFalse(ComputerDetails.isValidUuid("host_1"));
    }

    private static String repeat(char c, int n) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < n; i++) {
            sb.append(c);
        }
        return sb.toString();
    }
}
