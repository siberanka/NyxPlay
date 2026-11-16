package com.limelight.preferences;

import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.util.Collections;
import java.util.Objects;
import java.util.concurrent.LinkedBlockingQueue;

import com.limelight.AppView;
import com.limelight.Game;
import com.limelight.PcView;
import com.limelight.ShortcutTrampoline;
import com.limelight.computers.ComputerManagerService;
import com.limelight.R;
import com.limelight.nvstream.http.ComputerDetails;
import com.limelight.nvstream.http.NvHTTP;
import com.limelight.nvstream.jni.MoonBridge;
import com.limelight.utils.Dialog;
import com.limelight.utils.DisplayText;
import com.limelight.utils.LaunchToken;
import com.limelight.utils.ServerHelper;
import com.limelight.utils.SpinnerDialog;
import com.limelight.utils.UiHelper;

import android.app.AlertDialog;
import android.app.Service;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.net.Uri;
import android.os.Bundle;
import android.os.IBinder;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;

public class AddComputerManually extends AppCompatActivity {
    private TextView hostText;
    private ComputerManagerService.ComputerManagerBinder managerBinder;
    private final LinkedBlockingQueue<String> computersToAdd = new LinkedBlockingQueue<>();
    private Thread addThread;
    private volatile boolean destroyed;

    // Set once a launch link has been passed on; the next screen owns the dialogs from then on
    private boolean launchHandedOff;
    private boolean serviceBound;
    private final ServiceConnection serviceConnection = new ServiceConnection() {
        public void onServiceConnected(ComponentName className, final IBinder binder) {
            managerBinder = ((ComputerManagerService.ComputerManagerBinder)binder);
            startAddThread();
        }

        public void onServiceDisconnected(ComponentName className) {
            joinAddThread();
            managerBinder = null;
        }
    };

    private boolean isWrongSubnetSiteLocalAddress(String address) {
        try {
            InetAddress targetAddress = InetAddress.getByName(address);
            if (!(targetAddress instanceof Inet4Address) || !targetAddress.isSiteLocalAddress()) {
                return false;
            }

            // We have a site-local address. Look for a matching local interface.
            for (NetworkInterface iface : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                for (InterfaceAddress addr : iface.getInterfaceAddresses()) {
                    if (!(addr.getAddress() instanceof Inet4Address) || !addr.getAddress().isSiteLocalAddress()) {
                        // Skip non-site-local or non-IPv4 addresses
                        continue;
                    }

                    byte[] targetAddrBytes = targetAddress.getAddress();
                    byte[] ifaceAddrBytes = addr.getAddress().getAddress();

                    // Compare prefix to ensure it's the same
                    boolean addressMatches = true;
                    for (int i = 0; i < addr.getNetworkPrefixLength(); i++) {
                        if ((ifaceAddrBytes[i / 8] & (1 << (i % 8))) != (targetAddrBytes[i / 8] & (1 << (i % 8)))) {
                            addressMatches = false;
                            break;
                        }
                    }

                    if (addressMatches) {
                        return false;
                    }
                }
            }

            // Couldn't find a matching interface
            return true;
        } catch (Exception e) {
            // Catch all exceptions because some broken Android devices
            // will throw an NPE from inside getNetworkInterfaces().
            e.printStackTrace();
            return false;
        }
    }

    private Uri parseRawUserInputToUri(String rawUserInput) {
        // Try adding a scheme and parsing the remaining input.
        // This handles input like 127.0.0.1:47989, [::1], [::1]:47989, and 127.0.0.1.
        Uri uri = Uri.parse("art://" + rawUserInput);
        if (uri.getHost() != null && !uri.getHost().isEmpty()) {
            return uri;
        }

        // Attempt to escape the input as an IPv6 literal.
        // This handles input like ::1.
        uri = Uri.parse("art://[" + rawUserInput + "]");
        if (uri.getHost() != null && !uri.getHost().isEmpty()) {
            return uri;
        }

        return null;
    }

    private void doAddPc(String rawUserInput) throws InterruptedException {
        boolean wrongSiteLocal = false;
        boolean invalidInput = false;
        boolean success;
        boolean invalidReply = false;
        int portTestResult;

        // Nothing is shown for an activity that is going away
        if (destroyed || isFinishing()) {
            return;
        }

        SpinnerDialog dialog = SpinnerDialog.displayDialog(this, getResources().getString(R.string.title_add_pc),
            getResources().getString(R.string.msg_add_pc), false);

        Uri uri = parseRawUserInputToUri(rawUserInput);
        try {
            ComputerDetails details = new ComputerDetails();

            // Check if we parsed a host address successfully
            if (uri != null && uri.getHost() != null && !uri.getHost().isEmpty()) {
                String host = uri.getHost();
                int port = uri.getPort();

                // If a port was not specified, use the default
                if (port == -1) {
                    port = NvHTTP.DEFAULT_HTTP_PORT;
                }

                details.manualAddress = new ComputerDetails.AddressTuple(host, port);
                ComputerManagerService.AddResult result = managerBinder.addComputerWithResult(details);
                success = result == ComputerManagerService.AddResult.ADDED;
                invalidReply = result == ComputerManagerService.AddResult.INVALID_REPLY;
                if (!success && !invalidReply) {
                    wrongSiteLocal = isWrongSubnetSiteLocalAddress(host);
                }
            } else {
                // Invalid user input
                success = false;
                invalidInput = true;
            }
        } catch (InterruptedException e) {
            // Propagate the InterruptedException to the caller for proper handling
            dialog.dismiss();
            throw e;
        } catch (IllegalArgumentException e) {
            // This can be thrown from OkHttp if the host fails to canonicalize to a valid name.
            // https://github.com/square/okhttp/blob/okhttp_27/okhttp/src/main/java/com/squareup/okhttp/HttpUrl.java#L705
            e.printStackTrace();
            success = false;
            invalidInput = true;
        }

        // Keep the SpinnerDialog open while testing connectivity
        if (!success && !wrongSiteLocal && !invalidInput && !invalidReply) {
            // Run the test before dismissing the spinner because it can take a few seconds.
            portTestResult = MoonBridge.testClientConnectivity(ServerHelper.CONNECTION_TEST_SERVER, 443,
                    MoonBridge.ML_PORT_FLAG_TCP_47984 | MoonBridge.ML_PORT_FLAG_TCP_47989);
        } else {
            // Don't bother with the test if we succeeded, the IP address was bogus or the host answered
            portTestResult = MoonBridge.ML_TEST_RESULT_INCONCLUSIVE;
        }

        dialog.dismiss();

        // The activity may have been closed while the request was running; its result is dropped
        if (destroyed || isFinishing()) {
            return;
        }

        if (invalidInput) {
            Dialog.displayDialog(this, getResources().getString(R.string.conn_error_title), getResources().getString(R.string.addpc_unknown_host), false);
        }
        else if (invalidReply) {
            Dialog.displayDialog(this, getResources().getString(R.string.conn_error_title), getResources().getString(R.string.addpc_invalid_reply), false);
        }
        else if (wrongSiteLocal) {
            Dialog.displayDialog(this, getResources().getString(R.string.conn_error_title), getResources().getString(R.string.addpc_wrong_sitelocal), false);
        }
        else if (!success) {
            String dialogText;
            if (portTestResult != MoonBridge.ML_TEST_RESULT_INCONCLUSIVE && portTestResult != 0)  {
                dialogText = getResources().getString(R.string.nettest_text_blocked);
            }
            else {
                dialogText = getResources().getString(R.string.addpc_fail);
            }
            Dialog.displayDialog(this, getResources().getString(R.string.conn_error_title), dialogText, false);
        }
        else {
            AddComputerManually.this.runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    if (destroyed) {
                        return;
                    }

                    Toast.makeText(AddComputerManually.this, getResources().getString(R.string.addpc_success), Toast.LENGTH_LONG).show();

                    if (!isFinishing()) {
                        // Close the activity
                        AddComputerManually.this.finish();
                    }

                    String pin = uri.getQueryParameter("pin");
                    String passphrase = uri.getQueryParameter("passphrase");
                    if (pin != null && passphrase != null) {
                        Intent intent = new Intent(AddComputerManually.this, PcView.class);
                        intent.setFlags(Intent.FLAG_ACTIVITY_CLEAR_TASK | Intent.FLAG_ACTIVITY_NEW_TASK);
                        intent.putExtra("hostname", uri.getHost());
                        intent.putExtra("port", uri.getPort());
                        intent.putExtra("pin", pin);
                        intent.putExtra("passphrase", passphrase);
                        // Marks this hand-off as coming from the app itself; the user has confirmed the link already
                        intent.putExtra(LaunchToken.EXTRA, LaunchToken.get(AddComputerManually.this));

                        startActivity(intent);
                    }

                }
            });
        }

    }

    private void startAddThread() {
        if (destroyed) {
            return;
        }

        addThread = new Thread() {
            @Override
            public void run() {
                while (!isInterrupted()) {
                    try {
                        String computer = computersToAdd.take();
                        doAddPc(computer);
                    } catch (InterruptedException e) {
                        return;
                    }
                }
            }
        };
        addThread.setName("UI - AddComputerManually");
        addThread.start();
    }

    private void joinAddThread() {
        if (addThread != null) {
            addThread.interrupt();

            try {
                addThread.join();
            } catch (InterruptedException e) {
                e.printStackTrace();

                // InterruptedException clears the thread's interrupt status. Since we can't
                // handle that here, we will re-interrupt the thread to set the interrupt
                // status back to true.
                Thread.currentThread().interrupt();
            }

            addThread = null;
        }
    }

    @Override
    protected void onStop() {
        super.onStop();

        if (!launchHandedOff) {
            Dialog.closeDialogs();
        }
        SpinnerDialog.closeDialogs(this);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();

        destroyed = true;
        joinAddThread();
        if (!launchHandedOff) {
            Dialog.closeDialogs();
        }
        SpinnerDialog.closeDialogs(this);

        if (serviceBound) {
            serviceBound = false;
            unbindService(serviceConnection);
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        UiHelper.setLocale(this);

        String action = getIntent().getAction();

        String server;
        String query;
        Uri data = getIntent().getData();
        if (Intent.ACTION_VIEW.equals(action) && data != null) {
            int port = data.getPort();

            if (port == -1) {
                String urlAction = data.getHost();
                if (Objects.equals(urlAction, "launch")) {
                    String hostUUID = data.getQueryParameter("host_uuid");
                    String hostName = data.getQueryParameter("host_name");
                    String appUUID = data.getQueryParameter("app_uuid");
                    String appName = data.getQueryParameter("app_name");
                    String appID = data.getQueryParameter("app_id");

                    Intent intent = new Intent(AddComputerManually.this, ShortcutTrampoline.class);
                    intent.setFlags(Intent.FLAG_ACTIVITY_CLEAR_TASK | Intent.FLAG_ACTIVITY_NEW_TASK);
                    intent.putExtra(AppView.UUID_EXTRA, hostUUID);
                    intent.putExtra(AppView.NAME_EXTRA, hostName);
                    intent.putExtra(Game.EXTRA_APP_UUID, appUUID);
                    intent.putExtra(Game.EXTRA_APP_NAME, appName);
                    intent.putExtra(Game.EXTRA_APP_ID, appID);

                    // This link can be opened by any app or web page. It carries no launch token, so
                    // the trampoline asks first and names the host and app from this app's own records.
                    launchHandedOff = true;
                    startActivity(intent);
                    finish();

                    return;
                }
            }

            server = data.getAuthority();
            query = data.getQuery();
        } else {
            query = null;
            server = null;
        }

        setContentView(R.layout.activity_add_computer_manually);

        UiHelper.notifyNewRootView(this);

        this.hostText = findViewById(R.id.hostTextView);
        hostText.setImeOptions(EditorInfo.IME_ACTION_DONE);
        hostText.setOnEditorActionListener(new TextView.OnEditorActionListener() {
            @Override
            public boolean onEditorAction(TextView textView, int actionId, KeyEvent keyEvent) {
                if (actionId == EditorInfo.IME_ACTION_DONE ||
                        (keyEvent != null &&
                                keyEvent.getAction() == KeyEvent.ACTION_DOWN &&
                                keyEvent.getKeyCode() == KeyEvent.KEYCODE_ENTER)) {
                    return handleDoneEvent();
                }
                else if (actionId == EditorInfo.IME_ACTION_PREVIOUS) {
                    // This is how the Fire TV dismisses the keyboard
                    InputMethodManager imm = (InputMethodManager)getSystemService(Context.INPUT_METHOD_SERVICE);
                    imm.hideSoftInputFromWindow(hostText.getWindowToken(), 0);
                    return false;
                }

                return false;
            }
        });

        findViewById(R.id.addPcButton).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                handleDoneEvent();
            }
        });

        // Bind to the ComputerManager service
        serviceBound = bindService(new Intent(AddComputerManually.this,
                    ComputerManagerService.class), serviceConnection, Service.BIND_AUTO_CREATE);


        // Check if we have been called from deep link
        if (data == null || server == null) {
            return;
        }

        hostText.setText(server);

        if (query != null && !query.isEmpty()) {
            // Only the address that will be contacted is shown; a name supplied by the link
            // could be made to look like something else.
            String hostName = DisplayText.label(server);
            if (hostName == null) {
                hostName = "?";
            }

            AlertDialog.Builder builder = new AlertDialog.Builder(this);
            builder.setTitle(R.string.pair_pc_confirm_title);
            builder.setMessage(getString(R.string.pair_pc_confirm_message, hostName));

            builder.setPositiveButton(getString(R.string.proceed), (dialog, which) -> {
                dialog.dismiss();
                // The activity closes itself once the computer has been added
                computersToAdd.add(server + '?' + query);
            });

            builder.setNegativeButton(getString(R.string.cancel), (dialog, which) -> dialog.dismiss());

            AlertDialog dialog = builder.create();
            dialog.show();
        }
    }

    // Returns true if the event should be eaten
    private boolean handleDoneEvent() {
        String hostAddress = hostText.getText().toString().trim();

        if (hostAddress.length() == 0) {
            Toast.makeText(AddComputerManually.this, getResources().getString(R.string.addpc_enter_ip), Toast.LENGTH_LONG).show();
            return true;
        }

        computersToAdd.add(hostAddress);
        return false;
    }
}
