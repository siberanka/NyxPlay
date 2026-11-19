package com.limelight.utils;

import org.bouncycastle.crypto.params.Ed25519PublicKeyParameters;
import org.bouncycastle.crypto.signers.Ed25519Signer;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * Verification of signed release checksums (SHA256SUMS.txt plus a detached Ed25519 signature).
 * No method throws: every failure is reported through {@link Result}.
 * <p>
 * Installing an APK update is additionally protected by the platform's same-signer rule;
 * this check is defence in depth.
 */
public final class ReleaseVerifier {

    public enum Result {
        /** Everything checked out. */
        OK,
        /** Well formed signature that no trusted key accepts. */
        BAD_SIGNATURE,
        /** Signature text or checksum list not in the strict format. */
        MALFORMED,
        /** The file name has no entry in the checksum list. */
        NOT_LISTED,
        /** The file does not match the signed hash. */
        HASH_MISMATCH,
        /** The file could not be read. */
        IO_ERROR
    }

    public static final class SumsEntry {
        public final String name;
        /** 64 lowercase hex characters. */
        public final String sha256Hex;

        SumsEntry(String name, String sha256Hex) {
            this.name = name;
            this.sha256Hex = sha256Hex;
        }
    }

    public static final int SIGNATURE_SIZE = 64;
    public static final int PUBLIC_KEY_SIZE = 32;
    public static final int MAX_SUMS_SIZE = 64 * 1024;

    // SHA-256 fingerprint of this key:
    // db9a08158e93637d47eb44baa4e8c8c72973c611f2a115a54367a07b4aa8eca1
    // A later key rotation adds the new key to this list one release before it is used.
    private static final byte[][] TRUSTED_KEYS = {
            hex("942ba346020ed33fcf76feb170e056c16af2a2864bffaa6fd6ff4224af8e9326")
    };

    private static final char[] HEX_DIGITS = "0123456789abcdef".toCharArray();

    private ReleaseVerifier() {
    }

    /** The release signing keys embedded in this build (raw 32 byte keys). */
    public static List<byte[]> trustedKeys() {
        List<byte[]> copy = new ArrayList<>();
        for (byte[] key : TRUSTED_KEYS) {
            copy.add(key.clone());
        }
        return Collections.unmodifiableList(copy);
    }

    /** Lowercase hex SHA-256 of a raw public key. Safe to log. */
    public static String keyFingerprint(byte[] rawKey) {
        try {
            return toHex(MessageDigest.getInstance("SHA-256").digest(rawKey));
        } catch (Exception e) {
            return "";
        }
    }

    /**
     * Strict Base64: 88 characters ending in "==", canonical, 64 bytes. One trailing LF is accepted.
     *
     * @return the 64 signature bytes, or null if the text is not in that exact form
     */
    public static byte[] decodeSignature(byte[] textBytes) {
        if (textBytes == null) {
            return null;
        }
        int length = textBytes.length;
        if (length > 0 && textBytes[length - 1] == '\n') {
            length--;
        }
        // 64 bytes = 21 groups of 4 characters + one group with two characters and "=="
        if (length != 88 || textBytes[86] != '=' || textBytes[87] != '=') {
            return null;
        }
        byte[] out = new byte[SIGNATURE_SIZE];
        int pos = 0;
        for (int group = 0; group < 22; group++) {
            int[] v = new int[4];
            int chars = group == 21 ? 2 : 4;
            for (int i = 0; i < chars; i++) {
                v[i] = base64Value(textBytes[group * 4 + i]);
                if (v[i] < 0) {
                    return null;
                }
            }
            out[pos++] = (byte) ((v[0] << 2) | (v[1] >> 4));
            if (group == 21) {
                // the unused low four bits of the last character must be zero
                if ((v[1] & 0x0F) != 0) {
                    return null;
                }
            } else {
                out[pos++] = (byte) (((v[1] & 0x0F) << 4) | (v[2] >> 2));
                out[pos++] = (byte) (((v[2] & 0x03) << 6) | v[3]);
            }
        }
        return pos == SIGNATURE_SIZE ? out : null;
    }

    /** Verifies the signature over the exact bytes of the checksum list against the embedded keys. */
    public static Result verifySignature(byte[] sums, byte[] signatureBase64) {
        return verifySignature(sums, signatureBase64, Arrays.asList(TRUSTED_KEYS));
    }

    /** Verifies the signature against any key of the given list. */
    public static Result verifySignature(byte[] sums, byte[] signatureBase64, List<byte[]> keys) {
        try {
            if (sums == null || signatureBase64 == null || keys == null || sums.length > MAX_SUMS_SIZE) {
                return Result.MALFORMED;
            }
            byte[] signature = decodeSignature(signatureBase64);
            if (signature == null) {
                return Result.MALFORMED;
            }
            for (byte[] key : keys) {
                if (verifyOne(key, sums, signature)) {
                    return Result.OK;
                }
            }
            return Result.BAD_SIGNATURE;
        } catch (RuntimeException e) {
            return Result.MALFORMED;
        }
    }

    /**
     * Parses lines of "&lt;64 lowercase hex&gt;&lt;two spaces&gt;&lt;name&gt;", names only [A-Za-z0-9._-]
     * (not "." or ".."), no duplicate names. On any problem the result is MALFORMED and the list is empty.
     */
    public static Result parseSums(byte[] sums, List<SumsEntry> entries) {
        entries.clear();
        if (sums == null || sums.length == 0 || sums.length > MAX_SUMS_SIZE) {
            return Result.MALFORMED;
        }
        List<SumsEntry> parsed = new ArrayList<>();
        Set<String> seen = new HashSet<>();
        int pos = 0;
        while (pos < sums.length) {
            int end = pos;
            while (end < sums.length && sums[end] != '\n') {
                end++;
            }
            int lineLength = end - pos;
            // 64 hex + two spaces + 1..255 name characters
            if (lineLength < 67 || lineLength > 66 + 255 || sums[pos + 64] != ' ' || sums[pos + 65] != ' ') {
                return Result.MALFORMED;
            }
            for (int i = 0; i < 64; i++) {
                if (!isLowerHex(sums[pos + i])) {
                    return Result.MALFORMED;
                }
            }
            for (int i = 66; i < lineLength; i++) {
                if (!isNameChar(sums[pos + i])) {
                    return Result.MALFORMED;
                }
            }
            String hash = new String(sums, pos, 64, StandardCharsets.US_ASCII);
            String name = new String(sums, pos + 66, lineLength - 66, StandardCharsets.US_ASCII);
            if (name.equals(".") || name.equals("..") || !seen.add(name)) {
                return Result.MALFORMED;
            }
            parsed.add(new SumsEntry(name, hash));
            pos = end + 1;
        }
        if (parsed.isEmpty()) {
            return Result.MALFORMED;
        }
        entries.addAll(parsed);
        return Result.OK;
    }

    /** Exact name lookup. The returned hash is only meaningful when the result is OK. */
    public static Result findHash(byte[] sums, String fileName, StringBuilder sha256Hex) {
        sha256Hex.setLength(0);
        List<SumsEntry> entries = new ArrayList<>();
        Result parsed = parseSums(sums, entries);
        if (parsed != Result.OK) {
            return parsed;
        }
        for (SumsEntry entry : entries) {
            if (entry.name.equals(fileName)) {
                sha256Hex.append(entry.sha256Hex);
                return Result.OK;
            }
        }
        return Result.NOT_LISTED;
    }

    /** Streaming SHA-256 of a local file as lowercase hex. */
    public static Result hashFile(File file, StringBuilder sha256Hex) {
        sha256Hex.setLength(0);
        try (InputStream in = new FileInputStream(file)) {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            byte[] buffer = new byte[64 * 1024];
            int got;
            while ((got = in.read(buffer)) != -1) {
                digest.update(buffer, 0, got);
            }
            sha256Hex.append(toHex(digest.digest()));
            return Result.OK;
        } catch (IOException | RuntimeException | java.security.GeneralSecurityException e) {
            return Result.IO_ERROR;
        }
    }

    /** Compares two byte strings without an early exit on the first difference. */
    public static boolean constantTimeEqual(byte[] a, byte[] b) {
        return a != null && b != null && MessageDigest.isEqual(a, b);
    }

    /** Checks a local file against its entry in the (already signature checked) checksum list. */
    public static Result verifyFile(byte[] sums, String fileName, File file) {
        StringBuilder expected = new StringBuilder();
        Result found = findHash(sums, fileName, expected);
        if (found != Result.OK) {
            return found;
        }
        StringBuilder actual = new StringBuilder();
        Result hashed = hashFile(file, actual);
        if (hashed != Result.OK) {
            return hashed;
        }
        return constantTimeEqual(expected.toString().getBytes(StandardCharsets.US_ASCII),
                actual.toString().getBytes(StandardCharsets.US_ASCII)) ? Result.OK : Result.HASH_MISMATCH;
    }

    private static boolean verifyOne(byte[] key, byte[] sums, byte[] signature) {
        if (key == null || key.length != PUBLIC_KEY_SIZE) {
            return false;
        }
        try {
            Ed25519Signer signer = new Ed25519Signer();
            signer.init(false, new Ed25519PublicKeyParameters(key, 0));
            signer.update(sums, 0, sums.length);
            return signer.verifySignature(signature);
        } catch (RuntimeException e) {
            return false;
        }
    }

    private static int base64Value(byte c) {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    }

    private static boolean isNameChar(byte c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                || c == '.' || c == '_' || c == '-';
    }

    private static boolean isLowerHex(byte c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }

    private static String toHex(byte[] data) {
        char[] out = new char[data.length * 2];
        for (int i = 0; i < data.length; i++) {
            out[i * 2] = HEX_DIGITS[(data[i] >> 4) & 0x0F];
            out[i * 2 + 1] = HEX_DIGITS[data[i] & 0x0F];
        }
        return new String(out);
    }

    private static byte[] hex(String text) {
        byte[] out = new byte[text.length() / 2];
        for (int i = 0; i < out.length; i++) {
            out[i] = (byte) Integer.parseInt(text.substring(i * 2, i * 2 + 2), 16);
        }
        return out;
    }
}
