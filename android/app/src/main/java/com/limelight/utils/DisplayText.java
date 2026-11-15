package com.limelight.utils;

/**
 * Helpers for showing text that did not originate in this app.
 */
public final class DisplayText {
    // Longest value shown in a confirmation dialog
    public static final int MAX_LABEL_CHARS = 48;

    private DisplayText() {
    }

    /**
     * Makes a value received from outside safe to show in a dialog: control, formatting
     * (including direction overrides) and unassigned characters are dropped and the length is limited.
     *
     * @return the cleaned text, or null if nothing printable is left
     */
    public static String label(String raw) {
        if (raw == null) {
            return null;
        }

        StringBuilder out = new StringBuilder();
        boolean truncated = false;
        for (int i = 0; i < raw.length(); ) {
            int cp = raw.codePointAt(i);
            i += Character.charCount(cp);

            int type = Character.getType(cp);
            boolean hidden = Character.isISOControl(cp)
                    || type == Character.FORMAT
                    || type == Character.LINE_SEPARATOR
                    || type == Character.PARAGRAPH_SEPARATOR
                    || type == Character.UNASSIGNED
                    || type == Character.PRIVATE_USE
                    || type == Character.SURROGATE;
            if (hidden) {
                continue;
            }
            if (out.codePointCount(0, out.length()) >= MAX_LABEL_CHARS) {
                truncated = true;
                break;
            }
            out.appendCodePoint(cp);
        }

        String result = out.toString().trim();
        if (result.isEmpty()) {
            return null;
        }
        return truncated ? result + "…" : result;
    }
}
