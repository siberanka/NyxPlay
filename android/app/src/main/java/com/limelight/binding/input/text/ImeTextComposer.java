package com.limelight.binding.input.text;

/**
 * Converts input method editor (IME) operations into edits that can be replayed on the host.
 * <p>
 * Soft keyboards with predictions or gesture typing first show a word as "composing text" and
 * update it while the user types. The host has no editable model of our text, so every update is
 * translated into a number of backspaces followed by the newly inserted characters. This keeps the
 * remote text field in sync while the user is still typing instead of waiting for the word to be
 * committed.
 */
public final class ImeTextComposer {

    /** An edit to replay on the host: delete characters, then insert text. */
    public static final class Edit {
        public final int backspaces;
        public final int forwardDeletes;
        public final String insert;

        Edit(int backspaces, int forwardDeletes, String insert) {
            this.backspaces = backspaces;
            this.forwardDeletes = forwardDeletes;
            this.insert = insert;
        }

        public boolean isEmpty() {
            return backspaces == 0 && forwardDeletes == 0 && insert.isEmpty();
        }

        @Override
        public String toString() {
            return "Edit{backspaces=" + backspaces + ", forwardDeletes=" + forwardDeletes + ", insert='" + insert + "'}";
        }
    }

    private static final Edit NONE = new Edit(0, 0, "");

    /** Upper bound for a single IME delete request (each unit becomes a key press on the host). */
    public static final int MAX_DELETE = 256;

    private String composing = "";

    private static int clamp(int length) {
        return Math.max(0, Math.min(length, MAX_DELETE));
    }

    /** Text currently shown as composing text on the host. */
    public String getComposingText() {
        return composing;
    }

    public Edit setComposingText(CharSequence text) {
        String next = text == null ? "" : text.toString();
        Edit edit = diff(composing, next);
        composing = next;
        return edit;
    }

    public Edit commitText(CharSequence text) {
        String committed = text == null ? "" : text.toString();
        Edit edit = diff(composing, committed);
        composing = "";
        return edit;
    }

    /** The composing text becomes final; it was already sent so nothing changes on the host. */
    public Edit finishComposingText() {
        composing = "";
        return NONE;
    }

    /**
     * {@link android.view.inputmethod.InputConnection#deleteSurroundingText} variant: the lengths are
     * UTF-16 code units. Units inside the composing text are converted to code points (an emoji is
     * one host backspace); units before it are unknown host text and count as one character each.
     */
    public Edit deleteSurroundingTextUtf16(int beforeLength, int afterLength) {
        int before = clamp(beforeLength);
        if (before > 0 && !composing.isEmpty()) {
            int inComposing = Math.min(before, composing.length());
            int start = composing.length() - inComposing;
            if (start > 0 && start < composing.length() && Character.isLowSurrogate(composing.charAt(start))
                    && Character.isHighSurrogate(composing.charAt(start - 1))) {
                // Never split a surrogate pair: include the whole character
                start--;
                inComposing++;
            }
            int codePoints = composing.codePointCount(start, composing.length());
            before = codePoints + Math.max(0, before - inComposing);
        }
        return deleteSurroundingText(before, afterLength);
    }

    /**
     * {@link android.view.inputmethod.InputConnection#deleteSurroundingTextInCodePoints} variant.
     * Lengths are clamped to {@link #MAX_DELETE} (some keyboards ask to delete everything).
     */
    public Edit deleteSurroundingText(int beforeLength, int afterLength) {
        int before = clamp(beforeLength);
        int after = clamp(afterLength);
        if (!composing.isEmpty()) {
            // Deleting inside the composing region shortens it locally as well
            int composingCodePoints = composing.codePointCount(0, composing.length());
            int removed = Math.min(before, composingCodePoints);
            int keep = composingCodePoints - removed;
            composing = composing.substring(0, composing.offsetByCodePoints(0, keep));
        }
        return before == 0 && after == 0 ? NONE : new Edit(before, after, "");
    }

    /** Forget the composing state without touching the host (e.g. when the IME restarts). */
    public void reset() {
        composing = "";
    }

    /**
     * Minimal edit turning {@code from} into {@code to}: keep the common prefix (by code point),
     * delete the rest of {@code from} and insert the rest of {@code to}.
     */
    static Edit diff(String from, String to) {
        if (from.equals(to)) {
            return NONE;
        }
        int i = 0;
        int j = 0;
        while (i < from.length() && j < to.length()) {
            int a = from.codePointAt(i);
            int b = to.codePointAt(j);
            if (a != b) {
                break;
            }
            i += Character.charCount(a);
            j += Character.charCount(b);
        }
        int backspaces = from.codePointCount(i, from.length());
        return new Edit(backspaces, 0, to.substring(j));
    }
}
