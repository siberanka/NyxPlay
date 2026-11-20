package com.limelight;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.widget.Toast;

import com.limelight.utils.UpdateNotifier;
import com.limelight.utils.UpdateShortcuts;

/**
 * Target of the launcher shortcuts "Check for updates" and "Install update". It is not exported and has
 * no intent filter: only this app's own shortcuts reach it. It validates the request, hands it to the
 * host list in-process and opens the host list. Nothing in the intent is trusted beyond two fixed actions.
 */
public class UpdateShortcutActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        UpdateShortcuts.Action action = UpdateShortcuts.parse(getIntent(), UpdateNotifier.pendingVersion(this));
        if (action != UpdateShortcuts.Action.NONE) {
            if (Game.instance != null) {
                // Never disturb a running stream
                Toast.makeText(this, R.string.update_not_during_stream, Toast.LENGTH_LONG).show();
            } else {
                UpdateNotifier.requestShortcutAction(action);
                startActivity(new Intent(this, PcView.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP));
            }
        }
        finish();
    }
}
