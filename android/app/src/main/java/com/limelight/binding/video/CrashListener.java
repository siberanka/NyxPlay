package com.limelight.binding.video;

public interface CrashListener {
    void notifyCrash(Exception e);

    /**
     * Called when the decoder could not be recovered and has stopped rendering, for example
     * because its Surface became invalid. May be called from a non-UI thread.
     */
    default void notifyDecoderUnrecoverable() {}
}
