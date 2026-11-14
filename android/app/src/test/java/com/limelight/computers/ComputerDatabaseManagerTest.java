package com.limelight.computers;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;

import android.content.ContentValues;
import android.content.Context;
import android.database.sqlite.SQLiteDatabase;

import androidx.test.core.app.ApplicationProvider;

import com.limelight.binding.crypto.AndroidCryptoProvider;
import com.limelight.nvstream.http.ComputerDetails;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.util.List;

@Config(sdk = {33}, shadows = {com.limelight.shadows.ShadowMoonBridge.class, com.limelight.shadows.ShadowGameManager.class})
@RunWith(RobolectricTestRunner.class)
public class ComputerDatabaseManagerTest {
    private static final String VALID_ID = "3f2b1c4e-9a7d-4e21-8c55-0a1b2c3d4e5f";
    private static final String ADDRESSES = "{\"local\":{\"address\":\"10.0.0.5\",\"port\":47989}}";

    private Context context;

    @Before
    public void setUp() {
        context = ApplicationProvider.getApplicationContext();
        context.deleteDatabase("computers4.db");
    }

    private void insertRawRow(String uuid, String name, String addresses) {
        SQLiteDatabase db = context.openOrCreateDatabase("computers4.db", 0, null);
        try {
            db.execSQL("CREATE TABLE IF NOT EXISTS Computers(UUID TEXT PRIMARY KEY, ComputerName TEXT NOT NULL, " +
                    "Addresses TEXT NOT NULL, MacAddress TEXT, ServerCert TEXT)");
            ContentValues values = new ContentValues();
            values.put("UUID", uuid);
            values.put("ComputerName", name);
            values.put("Addresses", addresses);
            db.insert("Computers", null, values);
        } finally {
            db.close();
        }
    }

    @Test
    public void getAllComputers_skipsInvalidUuidRows() {
        insertRawRow("../x", "bad", ADDRESSES);
        insertRawRow(VALID_ID, "good", ADDRESSES);

        ComputerDatabaseManager manager = new ComputerDatabaseManager(context);
        try {
            List<ComputerDetails> all = manager.getAllComputers();
            assertEquals(1, all.size());
            assertEquals(VALID_ID, all.get(0).uuid);
        } finally {
            manager.close();
        }
    }

    @Test
    public void initializeDb_removesInvalidUuidRows_andReportsCount() {
        insertRawRow("../x", "bad one", ADDRESSES);
        insertRawRow("host_1", "bad two", ADDRESSES);
        insertRawRow(VALID_ID, "good", ADDRESSES);

        ComputerDatabaseManager manager = new ComputerDatabaseManager(context);
        manager.close();
        assertEquals(2, manager.getDroppedHostCount());

        // The second open finds nothing left to remove and the valid row survives
        ComputerDatabaseManager again = new ComputerDatabaseManager(context);
        try {
            assertEquals(0, again.getDroppedHostCount());
            assertEquals(1, again.getAllComputers().size());
            assertNotNull(again.getComputerByUUID(VALID_ID));
        } finally {
            again.close();
        }
    }

    @Test
    public void getAllComputers_corruptAddressJson_doesNotThrow() {
        insertRawRow("aaaa-bbbb", "corrupt", "{not json");
        insertRawRow(VALID_ID, "good", ADDRESSES);

        ComputerDatabaseManager manager = new ComputerDatabaseManager(context);
        try {
            List<ComputerDetails> all = manager.getAllComputers();
            assertEquals(1, all.size());
            assertEquals(VALID_ID, all.get(0).uuid);
        } finally {
            manager.close();
        }
    }

    @Test
    public void updateComputer_roundTrip_preservesCertAndAddresses() {
        ComputerDetails details = new ComputerDetails();
        details.uuid = VALID_ID;
        details.name = "Round trip";
        details.localAddress = new ComputerDetails.AddressTuple("10.0.0.5", 47989);
        details.remoteAddress = new ComputerDetails.AddressTuple("203.0.113.9", 47990);
        details.manualAddress = new ComputerDetails.AddressTuple("host.example", 47989);
        details.macAddress = "00:11:22:33:44:55";
        details.serverCert = new AndroidCryptoProvider(context).getClientCertificate();

        ComputerDatabaseManager manager = new ComputerDatabaseManager(context);
        try {
            manager.updateComputer(details);
            ComputerDetails stored = manager.getComputerByUUID(VALID_ID);
            assertNotNull(stored);
            assertEquals("Round trip", stored.name);
            assertEquals("10.0.0.5", stored.localAddress.address);
            assertEquals(47990, stored.remoteAddress.port);
            assertEquals("host.example", stored.manualAddress.address);
            assertNull(stored.ipv6Address);
            assertEquals("00:11:22:33:44:55", stored.macAddress);
            assertEquals(details.serverCert, stored.serverCert);
        } finally {
            manager.close();
        }
    }

    @Test
    public void getComputerByUUID_invalidId_returnsNull() {
        ComputerDatabaseManager manager = new ComputerDatabaseManager(context);
        try {
            assertNull(manager.getComputerByUUID("../../x"));
            assertNull(manager.getComputerByUUID(null));
        } finally {
            manager.close();
        }
    }
}
