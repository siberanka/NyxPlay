package com.limelight;

/** Canned host replies shared by the networking tests. */
public final class TestHostResponses {
    private TestHostResponses() {}

    public static String serverInfo(String uniqueId) {
        return "<?xml version=\"1.0\" encoding=\"utf-8\"?>" +
                "<root status_code=\"200\">" +
                "<hostname>TestHost</hostname>" +
                "<appversion>7.1.431.-1</appversion>" +
                "<uniqueid>" + uniqueId + "</uniqueid>" +
                "<HttpsPort>47984</HttpsPort>" +
                "<mac>00:00:00:00:00:00</mac>" +
                "<state>SUNSHINE_SERVER_FREE</state>" +
                "<PairStatus>0</PairStatus>" +
                "<currentgame>0</currentgame>" +
                "</root>";
    }

    /** A valid serverinfo document padded with a trailing XML comment to reach the requested size. */
    public static String serverInfoOfSize(String uniqueId, int totalBytes) {
        String base = serverInfo(uniqueId);
        String close = "</root>";
        String head = base.substring(0, base.length() - close.length());
        int fill = totalBytes - head.length() - close.length() - "<!---->".length();
        StringBuilder sb = new StringBuilder(totalBytes);
        sb.append(head).append("<!--");
        for (int i = 0; i < fill; i++) {
            sb.append('a');
        }
        sb.append("-->").append(close);
        return sb.toString();
    }
}
