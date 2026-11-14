package com.limelight.nvstream.mdns;

public interface MdnsDiscoveryListener {
    /**
     * Called when a new host was announced.
     * @return true if the host was handled, false if it should be reported again on a later announce
     */
    boolean notifyComputerAdded(MdnsComputer computer);
    void notifyDiscoveryFailure(Exception e);
}
