package com.limelight.ui;

import android.text.InputType;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;

import com.limelight.binding.input.text.ImeTextComposer;

/**
 * Input connection between the on-screen keyboard and the remote desktop.
 * <p>
 * Text typed with a soft keyboard is forwarded as exact Unicode characters (independent of the
 * host keyboard layout). Composing text from predictive keyboards is mirrored live on the host
 * via {@link ImeTextComposer}.
 */
public class StreamInputConnection extends BaseInputConnection {

    /** Receives the edits that must be replayed on the host. */
    public interface Sink {
        boolean handleImeEdit(ImeTextComposer.Edit edit);
    }

    private final Sink sink;
    private final ImeTextComposer composer = new ImeTextComposer();

    public StreamInputConnection(View targetView, Sink sink) {
        super(targetView, false);
        this.sink = sink;
    }

    /**
     * Configure the editor attributes announced to the input method.
     *
     * @param predictions allow suggestions, gesture typing and voice input (composing text);
     *                    when false every key press is committed immediately
     */
    public static void configure(EditorInfo outAttrs, boolean predictions) {
        if (predictions) {
            outAttrs.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_AUTO_CORRECT;
        } else {
            // Visible-password style input disables composing and auto-correction on most
            // keyboards (Gboard, SwiftKey, Samsung) so characters are delivered one by one.
            outAttrs.inputType = InputType.TYPE_CLASS_TEXT
                    | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
                    | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
        }
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI
                | EditorInfo.IME_FLAG_NO_FULLSCREEN
                | EditorInfo.IME_FLAG_NO_ENTER_ACTION
                | EditorInfo.IME_ACTION_NONE;
        outAttrs.initialSelStart = 0;
        outAttrs.initialSelEnd = 0;
    }

    private boolean apply(ImeTextComposer.Edit edit) {
        return sink != null && sink.handleImeEdit(edit);
    }

    @Override
    public boolean commitText(CharSequence text, int newCursorPosition) {
        if (text != null && text.length() == 1 && text.charAt(0) == '\n') {
            // Some keyboards commit a newline instead of sending the Enter key
            composer.commitText("");
            return sendEnter();
        }
        return apply(composer.commitText(text)) || super.commitText(text, newCursorPosition);
    }

    @Override
    public boolean setComposingText(CharSequence text, int newCursorPosition) {
        return apply(composer.setComposingText(text)) || super.setComposingText(text, newCursorPosition);
    }

    @Override
    public boolean finishComposingText() {
        composer.finishComposingText();
        return true;
    }

    @Override
    public boolean deleteSurroundingText(int beforeLength, int afterLength) {
        return apply(composer.deleteSurroundingText(beforeLength, afterLength))
                || super.deleteSurroundingText(beforeLength, afterLength);
    }

    @Override
    public boolean deleteSurroundingTextInCodePoints(int beforeLength, int afterLength) {
        return deleteSurroundingText(beforeLength, afterLength);
    }

    @Override
    public boolean performEditorAction(int actionCode) {
        // Treat "Go/Done/Send" style actions as Enter on the host
        return sendEnter();
    }

    @Override
    public CharSequence getTextBeforeCursor(int length, int flags) {
        // The host text is unknown: only expose the composing word so keyboards can refine it
        String composing = composer.getComposingText();
        return composing.length() <= length ? composing : composing.substring(composing.length() - length);
    }

    @Override
    public CharSequence getTextAfterCursor(int length, int flags) {
        return "";
    }

    @Override
    public int getCursorCapsMode(int reqModes) {
        // Never auto-capitalise: we cannot know where sentences start on the host
        return 0;
    }

    @Override
    public void closeConnection() {
        composer.reset();
        super.closeConnection();
    }

    private boolean sendEnter() {
        long now = android.os.SystemClock.uptimeMillis();
        sendKeyEvent(new KeyEvent(now, now, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_ENTER, 0, 0,
                -1, 0, KeyEvent.FLAG_SOFT_KEYBOARD | KeyEvent.FLAG_KEEP_TOUCH_MODE));
        sendKeyEvent(new KeyEvent(now, now, KeyEvent.ACTION_UP, KeyEvent.KEYCODE_ENTER, 0, 0,
                -1, 0, KeyEvent.FLAG_SOFT_KEYBOARD | KeyEvent.FLAG_KEEP_TOUCH_MODE));
        return true;
    }
}
