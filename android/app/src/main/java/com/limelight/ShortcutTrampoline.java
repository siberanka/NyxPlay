package com.limelight;

import static com.limelight.utils.ServerHelper.getSecondaryDisplay;

import android.app.AlertDialog;
import android.app.Service;
import android.content.ComponentName;
import android.content.Intent;
import android.content.ServiceConnection;
import android.net.Uri;
import android.os.Bundle;
import android.os.IBinder;
import android.util.Log;

import androidx.appcompat.app.AppCompatActivity;

import com.limelight.computers.ComputerDatabaseManager;
import com.limelight.computers.ComputerManagerListener;
import com.limelight.computers.ComputerManagerService;
import com.limelight.nvstream.http.ComputerDetails;
import com.limelight.nvstream.http.NvApp;
import com.limelight.nvstream.http.NvHTTP;
import com.limelight.nvstream.http.PairingManager;
import com.limelight.nvstream.wol.WakeOnLanSender;
import com.limelight.preferences.PreferenceConfiguration;
import com.limelight.utils.CacheHelper;
import com.limelight.utils.Dialog;
import com.limelight.utils.LaunchToken;
import com.limelight.utils.ServerHelper;
import com.limelight.utils.ShortcutHelper;
import com.limelight.utils.SpinnerDialog;
import com.limelight.utils.UiHelper;

import org.xmlpull.v1.XmlPullParserException;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.StringReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;

public class ShortcutTrampoline extends AppCompatActivity {
    private PreferenceConfiguration prefConfig;
    private String uuidString;
    private NvApp app;
    private ArrayList<Intent> intentStack = new ArrayList<>();

    private int wakeHostTries = 10;
    private ComputerDetails computer;
    private SpinnerDialog blockingLoadSpinner;

    private volatile ComputerManagerService.ComputerManagerBinder managerBinder;
    private final AtomicBoolean serviceBound = new AtomicBoolean(false);
    private volatile boolean stopped;
    private AlertDialog confirmDialog;

    private static final String TAG = "ShortcutTrampoline";

    // Upper bound for a shortcut file; real files are a few hundred bytes
    private static final int MAX_ART_FILE_BYTES = 16 * 1024;

    // Releases the service connection exactly once, no matter which path gets here first
    private void unbindOnce() {
        if (serviceBound.compareAndSet(true, false)) {
            try {
                unbindService(serviceConnection);
            } catch (IllegalArgumentException e) {
                Log.w(TAG, "Service connection was already released", e);
            }
        }
    }

    private final ServiceConnection serviceConnection = new ServiceConnection() {
        public void onServiceConnected(ComponentName className, IBinder binder) {
            if (binder == null) {
                return;
            }

            final ComputerManagerService.ComputerManagerBinder localBinder =
                    ((ComputerManagerService.ComputerManagerBinder)binder);

            // Wait in a separate thread to avoid stalling the UI
            new Thread() {
                @Override
                public void run() {
                    // Wait for the binder to be ready
                    localBinder.waitForReady();

                    // The activity may have been left while we were waiting
                    if (stopped) {
                        return;
                    }

                    // Now make the binder visible
                    managerBinder = localBinder;

                    // Get the computer object
                    computer = managerBinder.getComputer(uuidString);

                    if (computer == null) {
                        Dialog.displayDialog(ShortcutTrampoline.this,
                                getResources().getString(R.string.conn_error_title),
                                getResources().getString(R.string.scut_pc_not_found),
                                true);

                        if (blockingLoadSpinner != null) {
                            blockingLoadSpinner.dismiss();
                            blockingLoadSpinner = null;
                        }

                        unbindOnce();
                        managerBinder = null;

                        return;
                    }

                    // Force CMS to repoll this machine
                    managerBinder.invalidateStateForComputer(computer.uuid);

                    // Start polling
                    managerBinder.startPolling(new ComputerManagerListener() {
                        @Override
                        public void notifyComputerUpdated(final ComputerDetails details) {
                            // Don't care about other computers
                            if (!details.uuid.equalsIgnoreCase(uuidString)) {
                                return;
                            }

                            // Try to wake the target PC if it's not online (up to some retry limit)
                            if (details.state != ComputerDetails.State.ONLINE && details.macAddress != null && --wakeHostTries >= 0) {
                                try {
                                    // Make a best effort attempt to wake the target PC
                                    WakeOnLanSender.sendWolPacket(computer);

                                    // If we sent at least one WoL packet, reset the computer state
                                    // to force ComputerManager to poll it again.
                                    managerBinder.invalidateStateForComputer(computer.uuid);
                                    return;
                                } catch (IOException e) {
                                    // If we got an exception, we couldn't send a single WoL packet,
                                    // so fallthrough into the offline error path.
                                    e.printStackTrace();
                                }
                            }

                            if (details.state != ComputerDetails.State.UNKNOWN) {
                                runOnUiThread(new Runnable() {
                                    @Override
                                    public void run() {
                                        // Stop showing the spinner
                                        if (blockingLoadSpinner != null) {
                                            blockingLoadSpinner.dismiss();
                                            blockingLoadSpinner = null;
                                        }

                                        // If the managerBinder was destroyed before this callback,
                                        // just finish the activity.
                                        if (managerBinder == null) {
                                            finish();
                                            return;
                                        }

                                        if (details.state == ComputerDetails.State.ONLINE && details.pairState == PairingManager.PairState.PAIRED) {
                                            
                                            // Launch game if provided app ID, otherwise launch app view
                                            if (app != null) {
                                                if (details.runningGameId == 0 || details.runningGameId == app.getAppId() || Objects.equals(details.runningGameUUID, app.getAppUUID())) {
                                                    intentStack.add(ServerHelper.createStartIntent(ShortcutTrampoline.this, app, details, managerBinder, prefConfig.useVirtualDisplay));

                                                    // Close this activity
                                                    finish();

                                                    // Now start the activities
                                                    startActivities(intentStack.toArray(new Intent[]{}));
                                                } else {
                                                    // Create the start intent immediately, so we can safely unbind the managerBinder
                                                    // below before we return.
                                                    final Intent startIntent = ServerHelper.createStartIntent(ShortcutTrampoline.this, app, details, managerBinder, prefConfig.useVirtualDisplay);

                                                    UiHelper.displayQuitConfirmationDialog(ShortcutTrampoline.this, new Runnable() {
                                                        @Override
                                                        public void run() {
                                                            intentStack.add(startIntent);

                                                            // Close this activity
                                                            finish();

                                                            // Now start the activities
                                                            startActivities(intentStack.toArray(new Intent[]{}));
                                                        }
                                                    }, new Runnable() {
                                                        @Override
                                                        public void run() {
                                                            // Close this activity
                                                            finish();
                                                        }
                                                    });
                                                }
                                            } else {
                                                // Close this activity
                                                finish();

                                                // Add the PC view at the back (and clear the task)
                                                Intent i;
                                                i = new Intent(ShortcutTrampoline.this, PcView.class);
                                                i.setAction(Intent.ACTION_MAIN);
                                                i.setFlags(Intent.FLAG_ACTIVITY_CLEAR_TASK | Intent.FLAG_ACTIVITY_NEW_TASK);
                                                intentStack.add(i);

                                                // Take this intent's data and create an intent to start the app view
                                                i = new Intent(getIntent());
                                                i.setClass(ShortcutTrampoline.this, AppView.class);
                                                intentStack.add(i);

                                                // If a game is running, we'll make the stream the top level activity
                                                if (details.runningGameId != 0) {
                                                    intentStack.add(ServerHelper.createStartIntent(ShortcutTrampoline.this,
                                                            new NvApp(null, null, details.runningGameId, false), details, managerBinder, prefConfig.useVirtualDisplay));
                                                }

                                                // Now start the activities
                                                startActivities(intentStack.toArray(new Intent[]{}));
                                            }
                                            
                                        }
                                        else if (details.state == ComputerDetails.State.OFFLINE) {
                                            // Computer offline - display an error dialog
                                            Dialog.displayDialog(ShortcutTrampoline.this,
                                                    getResources().getString(R.string.conn_error_title),
                                                    getResources().getString(R.string.error_pc_offline),
                                                    true);
                                        } else if (details.pairState != PairingManager.PairState.PAIRED) {
                                            // Computer not paired - display an error dialog
                                            Dialog.displayDialog(ShortcutTrampoline.this,
                                                    getResources().getString(R.string.conn_error_title),
                                                    getResources().getString(R.string.scut_not_paired),
                                                    true);
                                        }

                                        // We don't want any more callbacks from now on, so go ahead
                                        // and unbind from the service
                                        final ComputerManagerService.ComputerManagerBinder binderToRelease = managerBinder;
                                        if (binderToRelease != null) {
                                            binderToRelease.stopPolling();
                                            managerBinder = null;
                                        }
                                        unbindOnce();
                                    }
                                });
                            }
                        }
                    });
                }
            }.start();
        }

        public void onServiceDisconnected(ComponentName className) {
            managerBinder = null;
        }
    };

    protected boolean validateHostInput(String hostUUID, String hostName) {
        // Validate PC UUID/Name
        if (hostUUID == null && hostName == null) {
            Dialog.displayDialog(ShortcutTrampoline.this,
                    getResources().getString(R.string.conn_error_title),
                    getResources().getString(R.string.scut_invalid_uuid),
                    true);
            return false;
        }

        if (hostUUID != null && !hostUUID.isEmpty()) {
            if (!ComputerDetails.isValidUuid(hostUUID)) {
                Dialog.displayDialog(ShortcutTrampoline.this,
                        getResources().getString(R.string.conn_error_title),
                        getResources().getString(R.string.scut_invalid_uuid),
                        true);
                return false;
            }
        } else {
            // UUID is null, so fallback to Name
            if (hostName == null || hostName.isEmpty()) {
                Dialog.displayDialog(ShortcutTrampoline.this,
                        getResources().getString(R.string.conn_error_title),
                        getResources().getString(R.string.scut_invalid_uuid),
                        true);
                return false;
            }
        }

        return true;
    }


    protected boolean validateAppInput(String appUUID, String appIDStr, String appName) {
        if (appUUID == null && appIDStr == null && appName == null) {
            // We're just going to the AppView, so there is nothing to check
            return true;
        }

        if (appUUID != null && !appUUID.isEmpty()) {
            try {
                UUID.fromString(appUUID);
            } catch (IllegalArgumentException ex) {
                Dialog.displayDialog(ShortcutTrampoline.this,
                        getResources().getString(R.string.conn_error_title),
                        getResources().getString(R.string.scut_invalid_app_id),
                        true);
                return false;
            }
        } else {
            // Validate App ID (if provided)
            if (appIDStr != null && !appIDStr.isEmpty()) {
                try {
                    Integer.parseInt(appIDStr);
                } catch (NumberFormatException ex) {
                    Dialog.displayDialog(ShortcutTrampoline.this,
                            getResources().getString(R.string.conn_error_title),
                            getResources().getString(R.string.scut_invalid_app_id),
                            true);
                    return false;
                }
            }
        }

        return true;
    }

    // Returns the parsed entries, or null (after showing an error) if the file cannot be used
    private Map<String, String> parseArtFileData(Uri fileUri) {
        if (fileUri == null) {
            return null;
        }

        Map<String, String> artData = new HashMap<>();

        try (InputStream inputStream = getContentResolver().openInputStream(fileUri)) {
            if (inputStream == null) {
                throw new IOException("Unable to open .art file");
            }

            // Read a bounded amount so a hostile file cannot exhaust memory
            ByteArrayOutputStream content = new ByteArrayOutputStream();
            byte[] buffer = new byte[1024];
            int read;
            while ((read = inputStream.read(buffer)) != -1) {
                if (content.size() + read > MAX_ART_FILE_BYTES) {
                    throw new IOException(".art file is too large");
                }
                content.write(buffer, 0, read);
            }

            BufferedReader reader = new BufferedReader(new StringReader(content.toString(StandardCharsets.UTF_8.name())));
            String line;
            while ((line = reader.readLine()) != null) {
                line = line.trim();
                if (line.startsWith("#") || line.isEmpty()) {
                    continue; // Skip comments and empty lines
                }

                if (!line.startsWith("[")) {
                    throw new IOException("Invalid .art file format");
                }

                int separatorIndex = line.indexOf(' ');
                if (separatorIndex > 0 && separatorIndex < line.length() - 1) {
                    String key = line.substring(0, separatorIndex).trim();
                    String value = line.substring(separatorIndex + 1).trim();
                    if (key.endsWith("]")) {
                        key = key.substring(1, key.length() - 1);
                        artData.put(key, value);
                    } else {
                        throw new IOException("Invalid .art file format");
                    }
                }
            }
        } catch (IOException | RuntimeException e) {
            Log.e(TAG, "Error reading .art file", e);
            Dialog.displayDialog(ShortcutTrampoline.this,
                    getResources().getString(R.string.conn_error_title),
                    getResources().getString(R.string.scut_art_read_error),
                    true);
            return null;
        }
        return artData;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        prefConfig = PreferenceConfiguration.readPreferences(this);

        UiHelper.notifyNewRootView(this);
        ComputerDatabaseManager dbManager = new ComputerDatabaseManager(this);
        ComputerDetails _computer = null;

        Intent intent = getIntent();
        String action = intent.getAction();
        Uri dataUri = intent.getData();

        // Intents built by this app carry a per-install token. Anything else (a shared shortcut file,
        // an intent from another app, a shortcut pinned by an older version) has to be confirmed first.
        final boolean viewLaunch = Intent.ACTION_VIEW.equals(action) && dataUri != null;
        final boolean externalLaunch = viewLaunch
                || !LaunchToken.matches(this, intent.getStringExtra(LaunchToken.EXTRA));

        String hostUUID = null;
        String hostName = null;
        String appName = null;
        String appUUID = null;
        String appIDStr = null;

        if (viewLaunch) {
            Map<String, String> artData = parseArtFileData(dataUri);
            if (artData == null) {
                // The error has been shown already
                return;
            }

            hostUUID = artData.get(ShortcutHelper.KEY_HOST_UUID);
            hostName = artData.get(ShortcutHelper.KEY_HOST_NAME);
            appName = artData.get(ShortcutHelper.KEY_APP_NAME);
            appUUID = artData.get(ShortcutHelper.KEY_APP_UUID);
            appIDStr = artData.get(ShortcutHelper.KEY_APP_ID);
        }

        {
            // PC arguments, both are optional, but at least one must be provided
            if (hostUUID == null) {
                hostUUID = getIntent().getStringExtra(AppView.UUID_EXTRA);
            }
            if (hostName == null) {
                hostName = getIntent().getStringExtra(AppView.NAME_EXTRA);
            }

            // App arguments, all optional, but one must be provided in order to start an app
            if (appUUID == null) {
                appUUID = getIntent().getStringExtra(Game.EXTRA_APP_UUID);
            }
            if (appIDStr == null) {
                appIDStr = getIntent().getStringExtra(Game.EXTRA_APP_ID);
            }
            if (appName == null) {
                appName = getIntent().getStringExtra(Game.EXTRA_APP_NAME);
            }
        }

        if (!validateHostInput(hostUUID, hostName)) {
            // Invalid input, so just return
//            finish();
            return;
        }

        if (hostUUID == null || hostUUID.isEmpty()) {
            // Use hostName to find the corresponding UUID
            _computer = dbManager.getComputerByName(hostName);

            if (_computer == null) {
                Dialog.displayDialog(ShortcutTrampoline.this,
                        getResources().getString(R.string.conn_error_title),
                        getResources().getString(R.string.scut_pc_not_found),
                        true);
//                    finish();
                return;
            }

            hostUUID = _computer.uuid;
        }

        uuidString = hostUUID;

        // Set the AppView UUID intent
        setIntent(new Intent(getIntent()).putExtra(AppView.UUID_EXTRA, uuidString));

        if (!validateAppInput(appUUID, appIDStr, appName)) {
            // Invalid input, so just return
            return;
        }

        if (appUUID != null || appIDStr != null || appName != null) {
            // If app data came from .art file or was determined by appNameString from extras
            if (appUUID != null && !appUUID.isEmpty()) {
                app = new NvApp(appName, // appName can be null if only UUID is provided
                        appUUID,
                        -1, // App ID is not strictly needed if UUID is present
                        getIntent().getBooleanExtra(Game.EXTRA_APP_HDR, false)); // HDR info still from intent
            } else if (appIDStr != null && !appIDStr.isEmpty()) {
                int appID = Integer.parseInt(appIDStr);
                app = new NvApp(appName, // appName can be null if only App ID is provided
                        null,
                        appID,
                        getIntent().getBooleanExtra(Game.EXTRA_APP_HDR, false)); // HDR info still from intent
            } else if (appName != null && !appName.isEmpty()) {
                // Use appNameString (from .art file or intent extra) to find the corresponding AppId and AppUUID
                try {
                    int appID = -1;
                    String appUuidFromFile = null;
                    String rawAppList = CacheHelper.readInputStreamToString(CacheHelper.openCacheFileForInput(getCacheDir(), "applist", uuidString));

                    if (rawAppList.isEmpty()) {
                        Dialog.displayDialog(ShortcutTrampoline.this,
                                getResources().getString(R.string.conn_error_title),
                                getResources().getString(R.string.scut_invalid_app_id) + " (applist cache empty or unreadable)",
                                true);
//                    finish();
                        return;
                    }
                    List<NvApp> applist = NvHTTP.getAppListByReader(new StringReader(rawAppList));

                    for (NvApp _app : applist) {
                        if (_app.getAppName().equalsIgnoreCase(appName)) {
                            appID = _app.getAppId();
                            appUuidFromFile = _app.getAppUUID();
                            break;
                        }
                    }
                    if (appID < 0 && appUuidFromFile == null) { // Need at least one
                        Dialog.displayDialog(ShortcutTrampoline.this,
                                getResources().getString(R.string.conn_error_title),
                                getResources().getString(R.string.scut_invalid_app_id) + " (app not found in cache)",
                                true);
//                    finish();
                        return;
                    }
                    // Update intent with found app ID and UUID if they weren't originally there
                    Intent currentIntent = getIntent();
                    if (currentIntent.getStringExtra(Game.EXTRA_APP_ID) == null && appID != -1) {
                        currentIntent.putExtra(Game.EXTRA_APP_ID, String.valueOf(appID));
                    }
                    if (currentIntent.getStringExtra(Game.EXTRA_APP_UUID) == null && appUuidFromFile != null) {
                        currentIntent.putExtra(Game.EXTRA_APP_UUID, appUuidFromFile);
                    }
                    app = new NvApp(
                            appName,
                            appUuidFromFile,
                            appID,
                            getIntent().getBooleanExtra(Game.EXTRA_APP_HDR, false));
                } catch (IOException | XmlPullParserException e) {
                    Log.e(TAG, "Error processing app list from cache", e);
                    Dialog.displayDialog(ShortcutTrampoline.this,
                            getResources().getString(R.string.conn_error_title),
                            getResources().getString(R.string.scut_invalid_app_id) + " (error parsing applist cache)",
                            true);
//                finish();
                    return;
                }
            }
        }

        final Runnable startLaunch = () -> {
            if (externalLaunch && !viewLaunch) {
                // A shortcut from an older version has no token: refresh the app's own shortcuts
                // so that they start directly next time.
                new ShortcutHelper(this).refreshShortcutTokens();
            }

            // Bind to the computer manager service
            serviceBound.set(bindService(new Intent(this, ComputerManagerService.class), serviceConnection,
                    Service.BIND_AUTO_CREATE));

            blockingLoadSpinner = SpinnerDialog.displayDialog(this, getResources().getString(R.string.conn_establishing_title),
                    getResources().getString(R.string.applist_connect_msg), true);
        };

        if (externalLaunch) {
            // The shortcut file came from another app, so ask before starting a stream
            String pcLabel = hostName != null && !hostName.isEmpty() ? hostName : (_computer != null ? _computer.name : null);
            String message;
            if (pcLabel != null && appName != null && !appName.isEmpty()) {
                message = getString(R.string.launch_confirm_message_named, appName, pcLabel);
            } else {
                message = getString(R.string.launch_confirm_message);
            }

            confirmDialog = new AlertDialog.Builder(this)
                    .setTitle(R.string.launch_confirm_title)
                    .setMessage(message)
                    .setCancelable(false)
                    .setPositiveButton(R.string.proceed, (dialog, which) -> startLaunch.run())
                    .setNegativeButton(R.string.cancel, (dialog, which) -> finish())
                    .show();
        }
        else {
            startLaunch.run();
        }
    }

    @Override
    protected void onStop() {
        super.onStop();

        stopped = true;

        if (blockingLoadSpinner != null) {
            blockingLoadSpinner.dismiss();
            blockingLoadSpinner = null;
        }

        if (confirmDialog != null) {
            confirmDialog.dismiss();
            confirmDialog = null;
        }

        Dialog.closeDialogs();

        final ComputerManagerService.ComputerManagerBinder binderToRelease = managerBinder;
        if (binderToRelease != null) {
            binderToRelease.stopPolling();
            managerBinder = null;
        }
        unbindOnce();

        finish();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();

        if (confirmDialog != null) {
            confirmDialog.dismiss();
            confirmDialog = null;
        }

        unbindOnce();
    }
}
