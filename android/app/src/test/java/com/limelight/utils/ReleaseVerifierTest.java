package com.limelight.utils;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

import com.limelight.utils.ReleaseVerifier.Result;

public class ReleaseVerifierTest {
    private static final String INSTALLER = "NyxPlayHost-2026.11.19.2-windows-x64.exe";
    private static final String LARGE = "NyxPlay-2026.11.19.2-arm64-v8a-release.apk";
    private static final String EMPTY = "NyxPlay-empty.bin";

    // An unrelated public key (RFC 8032 test vector 1)
    private static final byte[] OTHER_KEY = hex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");

    @Rule
    public TemporaryFolder folder = new TemporaryFolder();

    private byte[] sums;
    private byte[] sig;
    private byte[] bare;

    @Before
    public void loadFixtures() throws IOException {
        sums = resource("/release/SHA256SUMS.txt");
        sig = resource("/release/SHA256SUMS.txt.sig");
        assertEquals(89, sig.length);
        bare = Arrays.copyOf(sig, 88);
    }

    private static byte[] resource(String name) throws IOException {
        try (InputStream in = ReleaseVerifierTest.class.getResourceAsStream(name)) {
            assertNotNull(name, in);
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buffer = new byte[4096];
            int got;
            while ((got = in.read(buffer)) != -1) {
                out.write(buffer, 0, got);
            }
            return out.toByteArray();
        }
    }

    private static byte[] hex(String text) {
        byte[] out = new byte[text.length() / 2];
        for (int i = 0; i < out.length; i++) {
            out[i] = (byte) Integer.parseInt(text.substring(i * 2, i * 2 + 2), 16);
        }
        return out;
    }

    private static byte[] ascii(String text) {
        return text.getBytes(StandardCharsets.US_ASCII);
    }

    private static String repeat(char c, int count) {
        char[] chars = new char[count];
        Arrays.fill(chars, c);
        return new String(chars);
    }

    private File write(String name, byte[] content) throws IOException {
        File file = new File(folder.getRoot(), name);
        try (FileOutputStream out = new FileOutputStream(file)) {
            out.write(content);
        }
        return file;
    }

    private static byte[] largeContent() {
        byte[] data = new byte[5 * 1024 * 1024 + 17];
        for (int i = 0; i < data.length; i++) {
            data[i] = (byte) ((i * 31 + 7) & 0xFF);
        }
        return data;
    }

    private static Result sigResult(byte[] sums, byte[] sig) {
        return ReleaseVerifier.verifySignature(sums, sig);
    }

    private static Result parse(String text) {
        return ReleaseVerifier.parseSums(ascii(text), new ArrayList<ReleaseVerifier.SumsEntry>());
    }

    private static Result find(String sums, String name, StringBuilder hex) {
        return ReleaseVerifier.findHash(ascii(sums), name, hex);
    }

    @Test
    public void embeddedKeyFingerprint() {
        List<byte[]> keys = ReleaseVerifier.trustedKeys();
        assertEquals(1, keys.size());
        assertEquals("db9a08158e93637d47eb44baa4e8c8c72973c611f2a115a54367a07b4aa8eca1",
                ReleaseVerifier.keyFingerprint(keys.get(0)));
    }

    @Test
    public void realSignatureVerifies() {
        assertEquals(Result.OK, sigResult(sums, sig));
        assertEquals(Result.OK, sigResult(sums, bare));
    }

    @Test
    public void tamperedSumsAreRejected() {
        byte[] changed = sums.clone();
        changed[0] = changed[0] == 'a' ? (byte) 'b' : (byte) 'a';
        assertEquals(Result.BAD_SIGNATURE, sigResult(changed, sig));
        byte[] longer = Arrays.copyOf(sums, sums.length + 1);
        longer[sums.length] = '\n';
        assertEquals(Result.BAD_SIGNATURE, sigResult(longer, sig));
        assertEquals(Result.BAD_SIGNATURE, sigResult(Arrays.copyOfRange(sums, 1, sums.length), sig));
    }

    @Test
    public void tamperedSignatureIsRejected() {
        byte[] changed = sig.clone();
        changed[0] = changed[0] == 'A' ? (byte) 'B' : (byte) 'A';
        assertEquals(Result.BAD_SIGNATURE, sigResult(sums, changed));
        changed = sig.clone();
        changed[40] = changed[40] == 'A' ? (byte) 'B' : (byte) 'A';
        assertEquals(Result.BAD_SIGNATURE, sigResult(sums, changed));
    }

    @Test
    public void wrongKeyIsRejected() {
        assertEquals(Result.BAD_SIGNATURE,
                ReleaseVerifier.verifySignature(sums, sig, Collections.singletonList(OTHER_KEY)));
        assertTrue(ReleaseVerifier.verifySignature(sums, sig, Collections.singletonList(new byte[32])) != Result.OK);
        assertEquals(Result.BAD_SIGNATURE,
                ReleaseVerifier.verifySignature(sums, sig, Collections.singletonList(new byte[31])));
        assertEquals(Result.BAD_SIGNATURE,
                ReleaseVerifier.verifySignature(sums, sig, Collections.<byte[]>emptyList()));
        // A key list accepts the signature when any key matches (key rotation)
        List<byte[]> rotated = new ArrayList<>();
        rotated.add(OTHER_KEY);
        rotated.addAll(ReleaseVerifier.trustedKeys());
        assertEquals(Result.OK, ReleaseVerifier.verifySignature(sums, sig, rotated));
    }

    @Test
    public void malformedSignatureText() {
        assertEquals(Result.MALFORMED, sigResult(sums, ascii("")));
        assertEquals(Result.MALFORMED, sigResult(sums, ascii("\n")));
        assertEquals(Result.MALFORMED, sigResult(sums, Arrays.copyOf(bare, 87)));
        assertEquals(Result.MALFORMED, sigResult(sums, Arrays.copyOf(bare, 86)));
        assertEquals(Result.MALFORMED, sigResult(sums, concat(bare, ascii("A"))));
        assertEquals(Result.MALFORMED, sigResult(sums, concat(bare, ascii("\n\n"))));
        assertEquals(Result.MALFORMED, sigResult(sums, concat(bare, ascii("\r\n"))));
        assertEquals(Result.MALFORMED, sigResult(sums, concat(ascii(" "), bare)));

        byte[] badChar = bare.clone();
        badChar[10] = '*';
        assertEquals(Result.MALFORMED, sigResult(sums, badChar));
        badChar[10] = '-';   // URL-safe alphabet is not accepted
        assertEquals(Result.MALFORMED, sigResult(sums, badChar));

        // Non-zero padding bits in the last character
        byte[] bits = bare.clone();
        bits[85] = 'B';
        assertEquals(Result.MALFORMED, sigResult(sums, bits));

        // 63 and 65 bytes of valid Base64, and no padding
        assertEquals(Result.MALFORMED, sigResult(sums, ascii(repeat('A', 84) + "A=")));
        assertEquals(Result.MALFORMED, sigResult(sums, ascii(repeat('A', 86))));
        assertEquals(Result.MALFORMED, sigResult(sums, ascii(repeat('A', 88))));
        assertEquals(Result.MALFORMED, ReleaseVerifier.verifySignature(null, sig));
        assertEquals(Result.MALFORMED, ReleaseVerifier.verifySignature(sums, null));
    }

    private static byte[] concat(byte[] a, byte[] b) {
        byte[] out = Arrays.copyOf(a, a.length + b.length);
        System.arraycopy(b, 0, out, a.length, b.length);
        return out;
    }

    @Test
    public void rfc8032VectorThroughTheKeyList() {
        // Empty message, RFC 8032 section 7.1 test 1
        byte[] signature = ascii("5VZDAMNgrHKQhuLMgG6CioSHfx645dl02HPgZSJJAVVfuIIVkKM7rMYeOXAc+bRr0lv18FlbviRlUUFDjnoQCw==");
        assertEquals(Result.OK,
                ReleaseVerifier.verifySignature(new byte[0], signature, Collections.singletonList(OTHER_KEY)));
    }

    @Test
    public void parsesTheFixture() {
        List<ReleaseVerifier.SumsEntry> entries = new ArrayList<>();
        assertEquals(Result.OK, ReleaseVerifier.parseSums(sums, entries));
        assertEquals(3, entries.size());
        assertEquals(EMPTY, entries.get(1).name);
        assertEquals("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", entries.get(1).sha256Hex);
    }

    @Test
    public void strictSumsFormat() {
        String hash = repeat('a', 64);
        assertEquals(Result.OK, parse(hash + "  file.bin\n"));
        assertEquals(Result.OK, parse(hash + "  file.bin"));   // missing final LF is tolerated
        assertEquals(Result.MALFORMED, parse(""));
        assertEquals(Result.MALFORMED, parse("\n"));
        assertEquals(Result.MALFORMED, parse(hash + "  file.bin\n\n"));   // empty line
        assertEquals(Result.MALFORMED, parse(hash + " *file.bin\n"));   // binary mode marker
        assertEquals(Result.MALFORMED, parse(hash + " file.bin\n"));
        assertEquals(Result.MALFORMED, parse(hash + "   file.bin\n"));
        assertEquals(Result.MALFORMED, parse(repeat('A', 64) + "  file.bin\n"));   // upper case hex
        assertEquals(Result.MALFORMED, parse(repeat('a', 63) + "  file.bin\n"));
        assertEquals(Result.MALFORMED, parse(repeat('a', 65) + "  file.bin\n"));
        assertEquals(Result.MALFORMED, parse(repeat('a', 63) + "g  file.bin\n"));
        assertEquals(Result.MALFORMED, parse(hash + "  file.bin\r\n"));
        assertEquals(Result.MALFORMED, parse(hash + "  \n"));   // no name
        assertEquals(Result.MALFORMED, parse(hash + "  file name.bin\n"));
        assertEquals(Result.MALFORMED, parse(hash + "  " + repeat('x', 256) + "\n"));
        assertEquals(Result.OK, parse(hash + "  " + repeat('x', 255) + "\n"));
        assertEquals(Result.MALFORMED, ReleaseVerifier.parseSums(new byte[ReleaseVerifier.MAX_SUMS_SIZE + 1],
                new ArrayList<ReleaseVerifier.SumsEntry>()));
        assertEquals(Result.MALFORMED, ReleaseVerifier.parseSums(null, new ArrayList<ReleaseVerifier.SumsEntry>()));
    }

    @Test
    public void pathLikeNamesAreMalformed() {
        String hash = repeat('b', 64);
        String[] names = {"../evil.exe", "dir/file.bin", "dir\\file.bin", "/abs.bin", "C:file.bin",
                "..", ".", "a%2Fb", "file.bin:stream", "naïve"};
        for (String name : names) {
            byte[] line = (hash + "  " + name + "\n").getBytes(StandardCharsets.UTF_8);
            assertEquals(name, Result.MALFORMED, ReleaseVerifier.parseSums(line, new ArrayList<ReleaseVerifier.SumsEntry>()));
        }
        assertEquals(Result.OK, parse(hash + "  ...\n"));
        StringBuilder hex = new StringBuilder();
        assertEquals(Result.NOT_LISTED, ReleaseVerifier.findHash(sums, "../" + INSTALLER, hex));
        assertEquals(Result.NOT_LISTED, ReleaseVerifier.findHash(sums, "x/" + INSTALLER, hex));
    }

    @Test
    public void duplicateAndMissingEntries() {
        String a = repeat('a', 64);
        String b = repeat('b', 64);
        StringBuilder hex = new StringBuilder();
        assertEquals(Result.MALFORMED, find(a + "  one.bin\n" + b + "  one.bin\n", "one.bin", hex));
        assertEquals(Result.MALFORMED, find(a + "  one.bin\n" + a + "  one.bin\n", "one.bin", hex));
        assertEquals(Result.OK, find(a + "  one.bin\n" + b + "  two.bin\n", "two.bin", hex));
        assertEquals(b, hex.toString());
        assertEquals(Result.NOT_LISTED, find(a + "  one.bin\n" + b + "  two.bin\n", "three.bin", hex));
        assertEquals(0, hex.length());
        // Exact match only: no case folding, no prefix or suffix match
        assertEquals(Result.NOT_LISTED, find(a + "  one.bin\n", "One.bin", hex));
        assertEquals(Result.NOT_LISTED, find(a + "  one.bin\n", "one.bi", hex));
        assertEquals(Result.NOT_LISTED, find(a + "  one.bin\n", "one.bin.", hex));
        assertEquals(Result.NOT_LISTED, find(a + "  one.bin\n", "", hex));
        assertEquals(Result.OK, ReleaseVerifier.findHash(sums, INSTALLER, hex));
        assertEquals("fb5c85338c609e0c9284185f942c7683275e2248086e5824611ef30f5ecd6a58", hex.toString());
    }

    @Test
    public void fileMatchesSignedHash() throws IOException {
        File file = write(INSTALLER, ascii("nyxplay fixture installer\n"));
        assertEquals(Result.OK, ReleaseVerifier.verifyFile(sums, INSTALLER, file));
        file = write(EMPTY, new byte[0]);
        assertEquals(Result.OK, ReleaseVerifier.verifyFile(sums, EMPTY, file));
    }

    @Test
    public void hashMismatch() throws IOException {
        File file = write(INSTALLER, ascii("nyxplay fixture installer\n\n"));
        assertEquals(Result.HASH_MISMATCH, ReleaseVerifier.verifyFile(sums, INSTALLER, file));
        file = write(INSTALLER, ascii("Nyxplay fixture installer\n"));
        assertEquals(Result.HASH_MISMATCH, ReleaseVerifier.verifyFile(sums, INSTALLER, file));
        file = write(INSTALLER, new byte[0]);
        assertEquals(Result.HASH_MISMATCH, ReleaseVerifier.verifyFile(sums, INSTALLER, file));
    }

    @Test
    public void notListedAndIoError() throws IOException {
        File other = write("other.bin", ascii("x"));
        assertEquals(Result.NOT_LISTED, ReleaseVerifier.verifyFile(sums, "other.bin", other));
        assertEquals(Result.IO_ERROR,
                ReleaseVerifier.verifyFile(sums, INSTALLER, new File(folder.getRoot(), "does-not-exist.bin")));
        assertEquals(Result.IO_ERROR, ReleaseVerifier.verifyFile(sums, INSTALLER, folder.getRoot()));   // a directory
        assertEquals(Result.MALFORMED, ReleaseVerifier.verifyFile(ascii("garbage"), INSTALLER, other));
        StringBuilder hex = new StringBuilder("stale");
        assertEquals(Result.IO_ERROR, ReleaseVerifier.hashFile(new File(folder.getRoot(), "does-not-exist.bin"), hex));
        assertEquals(0, hex.length());
    }

    @Test
    public void largeFileIsStreamedInChunks() throws IOException {
        byte[] data = largeContent();
        File file = write(LARGE, data);
        assertEquals(Result.OK, ReleaseVerifier.verifyFile(sums, LARGE, file));
        // One changed byte near the end is detected
        data[data.length - 5] ^= 0x01;
        file = write(LARGE, data);
        assertEquals(Result.HASH_MISMATCH, ReleaseVerifier.verifyFile(sums, LARGE, file));
    }

    @Test
    public void constantTimeCompare() {
        assertTrue(ReleaseVerifier.constantTimeEqual(ascii("abc"), ascii("abc")));
        assertTrue(ReleaseVerifier.constantTimeEqual(ascii(""), ascii("")));
        assertFalse(ReleaseVerifier.constantTimeEqual(ascii("abc"), ascii("abd")));
        assertFalse(ReleaseVerifier.constantTimeEqual(ascii("abc"), ascii("ab")));
        assertFalse(ReleaseVerifier.constantTimeEqual(ascii("abc"), ascii("abcd")));
        assertFalse(ReleaseVerifier.constantTimeEqual(null, ascii("abc")));
    }

    @Test
    public void decodeSignatureReturnsExactBytes() {
        byte[] decoded = ReleaseVerifier.decodeSignature(bare);
        assertNotNull(decoded);
        assertEquals(64, decoded.length);
        assertArrayEquals(decoded, ReleaseVerifier.decodeSignature(sig));
    }
}
