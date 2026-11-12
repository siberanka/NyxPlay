package com.limelight.binding.input.text;

import org.junit.Test;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

public class ImeTextComposerTest {

    private static void assertEdit(ImeTextComposer.Edit edit, int backspaces, String insert) {
        assertEquals("backspaces", backspaces, edit.backspaces);
        assertEquals("insert", insert, edit.insert);
        assertEquals("forward deletes", 0, edit.forwardDeletes);
    }

    @Test
    public void composingWordIsTypedLive() {
        ImeTextComposer c = new ImeTextComposer();
        assertEdit(c.setComposingText("m"), 0, "m");
        assertEdit(c.setComposingText("me"), 0, "e");
        assertEdit(c.setComposingText("mer"), 0, "r");
        assertEdit(c.setComposingText("merh"), 0, "h");
        assertEquals("merh", c.getComposingText());
    }

    @Test
    public void commitMatchingComposingTextOnlyAddsTheRest() {
        ImeTextComposer c = new ImeTextComposer();
        c.setComposingText("merhab");
        assertEdit(c.commitText("merhaba "), 0, "a ");
        assertEquals("", c.getComposingText());
    }

    @Test
    public void autocorrectionReplacesDivergingSuffix() {
        ImeTextComposer c = new ImeTextComposer();
        c.setComposingText("teh");
        assertEdit(c.commitText("the "), 2, "he ");
    }

    @Test
    public void turkishCharactersSurviveComposition() {
        ImeTextComposer c = new ImeTextComposer();
        assertEdit(c.setComposingText("ğü"), 0, "ğü");
        assertEdit(c.setComposingText("ğüşiöç"), 0, "şiöç");
        assertEdit(c.setComposingText("ğüşıöç"), 3, "ıöç");
        assertEdit(c.commitText("ğüşıöç"), 0, "");
    }

    @Test
    public void surrogatePairsCountAsOneBackspace() {
        ImeTextComposer c = new ImeTextComposer();
        c.setComposingText("a😀");
        assertEdit(c.setComposingText("a"), 1, "");
    }

    @Test
    public void finishComposingKeepsTextOnHost() {
        ImeTextComposer c = new ImeTextComposer();
        c.setComposingText("selam");
        assertTrue(c.finishComposingText().isEmpty());
        // The next word starts fresh
        assertEdit(c.setComposingText("d"), 0, "d");
    }

    @Test
    public void deleteSurroundingTextSendsBackspacesAndShrinksComposing() {
        ImeTextComposer c = new ImeTextComposer();
        c.setComposingText("abc");
        ImeTextComposer.Edit edit = c.deleteSurroundingText(1, 0);
        assertEquals(1, edit.backspaces);
        assertEquals("ab", c.getComposingText());

        ImeTextComposer.Edit forward = c.deleteSurroundingText(0, 2);
        assertEquals(0, forward.backspaces);
        assertEquals(2, forward.forwardDeletes);
    }

    @Test
    public void utf16DeleteCountsAnEmojiAsOneHostCharacter() {
        ImeTextComposer c = new ImeTextComposer();
        c.setComposingText("a😀"); // "a" + grinning face (surrogate pair)
        ImeTextComposer.Edit edit = c.deleteSurroundingTextUtf16(2, 0);
        assertEquals(1, edit.backspaces);
        assertEquals("a", c.getComposingText());
    }

    @Test
    public void utf16DeleteNeverSplitsASurrogatePair() {
        ImeTextComposer c = new ImeTextComposer();
        c.setComposingText("😀");
        // One code unit requested: the whole character goes
        ImeTextComposer.Edit edit = c.deleteSurroundingTextUtf16(1, 0);
        assertEquals(1, edit.backspaces);
        assertEquals("", c.getComposingText());
    }

    @Test
    public void utf16DeleteBeyondComposingCountsUnknownHostText() {
        ImeTextComposer c = new ImeTextComposer();
        c.setComposingText("ab");
        // 2 units inside the composing text + 3 units of committed host text
        assertEquals(5, c.deleteSurroundingTextUtf16(5, 0).backspaces);
        assertEquals("", c.getComposingText());
    }

    @Test
    public void hugeDeleteRequestsAreClamped() {
        ImeTextComposer c = new ImeTextComposer();
        ImeTextComposer.Edit edit = c.deleteSurroundingText(Integer.MAX_VALUE, Integer.MAX_VALUE);
        assertEquals(ImeTextComposer.MAX_DELETE, edit.backspaces);
        assertEquals(ImeTextComposer.MAX_DELETE, edit.forwardDeletes);
        assertEquals(ImeTextComposer.MAX_DELETE, c.deleteSurroundingTextUtf16(Integer.MAX_VALUE, 0).backspaces);
        assertTrue(c.deleteSurroundingText(-5, -1).isEmpty());
    }

    @Test
    public void plainCommitWithoutComposition() {
        ImeTextComposer c = new ImeTextComposer();
        assertEdit(c.commitText("ş"), 0, "ş");
        assertEdit(c.commitText("Ğ"), 0, "Ğ");
    }

    @Test
    public void emptyOperationsProduceNoEdit() {
        ImeTextComposer c = new ImeTextComposer();
        assertTrue(c.setComposingText("").isEmpty());
        assertTrue(c.commitText(null).isEmpty());
        assertTrue(c.deleteSurroundingText(0, 0).isEmpty());
    }
}
