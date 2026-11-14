package com.limelight.utils;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;

public class CacheHelperTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    @Test
    public void openPath_rejectsUnsafeComponents() throws IOException {
        File root = tmp.newFolder();
        String[] bad = { "", ".", "..", "a/b", "a\\b", "a\u0000b", null };
        for (String component : bad) {
            try {
                CacheHelper.openPath(false, root, "boxart", component);
                fail("Accepted unsafe component: " + component);
            } catch (IllegalArgumentException expected) {
                // expected
            }
        }
    }

    @Test
    public void openPath_staysInsideRoot() throws IOException {
        File root = tmp.newFolder();
        File f = CacheHelper.openPath(true, root, "boxart", "3f2b1c4e-9a7d", "12.png");
        assertTrue(f.getCanonicalPath().startsWith(root.getCanonicalPath()));
        assertTrue(f.getParentFile().isDirectory());
    }

    @Test
    public void writeInputStreamToOutputStream_overLimit_throws() {
        byte[] data = new byte[10000];
        try {
            CacheHelper.writeInputStreamToOutputStream(new ByteArrayInputStream(data), new ByteArrayOutputStream(), 9999);
            fail("Expected IOException");
        } catch (IOException expected) {
            // expected
        }
    }

    @Test
    public void writeInputStreamToOutputStream_belowLimit_copiesAll() throws IOException {
        byte[] data = new byte[5000];
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        CacheHelper.writeInputStreamToOutputStream(new ByteArrayInputStream(data), out, 10000);
        assertEquals(5000, out.size());
    }

    @Test
    public void deleteCacheFile_missing_returnsFalse() throws IOException {
        File root = tmp.newFolder();
        assertFalse(CacheHelper.deleteCacheFile(root, "applist", "missing"));
    }
}
