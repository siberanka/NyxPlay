package com.limelight.nvstream;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.io.IOException;
import java.io.InterruptedIOException;
import java.net.ConnectException;
import java.net.SocketTimeoutException;

public class NvConnectionTimeoutTest {
    @Test
    public void callTimeoutIsATimeout() {
        assertTrue(NvConnection.isTimeout(new InterruptedIOException("timeout")));
    }

    @Test
    public void socketTimeoutIsATimeout() {
        assertTrue(NvConnection.isTimeout(new SocketTimeoutException("Read timed out")));
    }

    @Test
    public void wrappedTimeoutIsATimeout() {
        assertTrue(NvConnection.isTimeout(new IOException("failed", new SocketTimeoutException("timeout"))));
    }

    @Test
    public void refusedConnectionIsNotATimeout() {
        assertFalse(NvConnection.isTimeout(new ConnectException("refused")));
        assertFalse(NvConnection.isTimeout(new IOException("closed")));
        assertFalse(NvConnection.isTimeout(null));
    }
}
