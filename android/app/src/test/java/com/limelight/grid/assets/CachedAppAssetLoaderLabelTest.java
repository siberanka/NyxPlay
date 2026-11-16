package com.limelight.grid.assets;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;
import static org.mockito.ArgumentMatchers.any;
import static org.mockito.ArgumentMatchers.anyInt;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.when;

import android.content.Context;
import android.graphics.Bitmap;
import android.widget.ImageView;
import android.widget.TextView;

import androidx.test.core.app.ApplicationProvider;

import com.limelight.nvstream.http.ComputerDetails;
import com.limelight.nvstream.http.NvApp;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

@Config(sdk = {33}, shadows = {com.limelight.shadows.ShadowMoonBridge.class, com.limelight.shadows.ShadowGameManager.class})
@RunWith(RobolectricTestRunner.class)
public class CachedAppAssetLoaderLabelTest {
    @Test
    public void renamedAppUpdatesTheLabelWhileItsArtIsStillLoading() throws Exception {
        Context context = ApplicationProvider.getApplicationContext();

        ComputerDetails computer = new ComputerDetails();
        computer.uuid = "3f2b1c4e-9a7d-4e21-8c55-0a1b2c3d4e5f";

        // Keep the first art load busy so a second request for the same app finds it still pending
        final CountDownLatch loadStarted = new CountDownLatch(1);
        final CountDownLatch releaseLoad = new CountDownLatch(1);
        DiskAssetLoader disk = mock(DiskAssetLoader.class);
        when(disk.loadBitmapFromCache(any(CachedAppAssetLoader.LoaderTuple.class), anyInt())).thenAnswer(invocation -> {
            loadStarted.countDown();
            releaseLoad.await(10, TimeUnit.SECONDS);
            return null;
        });

        CachedAppAssetLoader loader = new CachedAppAssetLoader(computer, 1.0, mock(NetworkAssetLoader.class),
                new MemoryAssetLoader(), disk, Bitmap.createBitmap(1, 1, Bitmap.Config.ARGB_8888));

        ImageView image = new ImageView(context);
        TextView label = new TextView(context);
        NvApp app = new NvApp("Old Name", null, 7, false);

        try {
            loader.populateImageView(app, image, label);
            assertEquals("Old Name", label.getText().toString());
            assertTrue(loadStarted.await(5, TimeUnit.SECONDS));

            app.setAppName("New Name");
            loader.populateImageView(app, image, label);

            assertEquals("New Name", label.getText().toString());
        } finally {
            releaseLoad.countDown();
            loader.cancelForegroundLoads();
            loader.cancelBackgroundLoads();
        }
    }
}
