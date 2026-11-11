package com.limelight.nvstream;

public interface NvConnectionListener {
    void stageStarting(String stage);
    void stageComplete(String stage);
    boolean stageFailed(String stage, int portFlags, int errorCode);
    
    void connectionStarted();
    void connectionTerminated(int errorCode);
    void connectionStatusUpdate(int connectionStatus);
    
    void displayMessage(String message);
    void displayTransientMessage(String message);

    void rumble(short controllerNumber, short lowFreqMotor, short highFreqMotor);
    void rumbleTriggers(short controllerNumber, short leftTrigger, short rightTrigger);

    void setHdrMode(boolean enabled, byte[] hdrMetadata);

    void setMotionEventState(short controllerNumber, byte motionType, short reportRateHz);

    void setControllerLED(short controllerNumber, byte r, byte g, byte b);

    /**
     * NyxPlay protocol extension: invoked when an editable text field on the host gains
     * ({@code active == true}) or loses keyboard focus.
     *
     * @param active true while a text field is focused on the host
     * @param flags  combination of MoonBridge.LI_TEXT_INPUT_FLAG_* values
     */
    default void setTextInputState(boolean active, int flags) {}
}
