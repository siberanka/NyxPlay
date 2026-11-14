package com.limelight;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import android.content.Context;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ImageButton;

import androidx.test.core.app.ApplicationProvider;

import org.junit.BeforeClass;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.util.ArrayList;
import java.util.List;

@Config(sdk = {33})
@RunWith(RobolectricTestRunner.class)
public class AccessibilityLabelsTest {
    @BeforeClass
    public static void suppressInvalidIdLogs() {
        TestLogSuppressor.install();
    }

    private static void collectImageButtons(View view, List<ImageButton> out) {
        if (view instanceof ImageButton) {
            out.add((ImageButton) view);
        }
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) {
                collectImageButtons(group.getChildAt(i), out);
            }
        }
    }

    private void assertAllImageButtonsLabelled(int layoutId, int minimumButtons) {
        Context base = ApplicationProvider.getApplicationContext();
        Context context = new androidx.appcompat.view.ContextThemeWrapper(base, R.style.AppTheme);
        View root = LayoutInflater.from(context).inflate(layoutId, null);

        List<ImageButton> buttons = new ArrayList<>();
        collectImageButtons(root, buttons);
        assertTrue(buttons.size() >= minimumButtons);
        for (ImageButton button : buttons) {
            CharSequence description = button.getContentDescription();
            assertNotNull("ImageButton without description in layout " + layoutId, description);
            assertFalse(description.toString().trim().isEmpty());
        }
    }

    @Test
    public void pcViewButtonsHaveDescriptions() {
        assertAllImageButtonsLabelled(R.layout.activity_pc_view, 3);
    }

    @Test
    public void profileRowButtonsHaveDescriptions() {
        assertAllImageButtonsLabelled(R.layout.row_profile, 2);
    }
}
