package com.limelight.binding.video;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/** Builds the CTA-861.3 static metadata block from the host supplied HDR metadata. */
final class HdrStaticInfo {
    /** Number of bytes of host metadata that are consumed (twelve 16-bit values). */
    static final int INPUT_LENGTH = 24;
    static final int OUTPUT_LENGTH = 25;

    private HdrStaticInfo() {}

    /** Returns the rewound 25 byte buffer, or null if the metadata is missing or too short. */
    static ByteBuffer build(byte[] metadata) {
        if (metadata == null || metadata.length < INPUT_LENGTH) {
            return null;
        }

        ByteBuffer out = ByteBuffer.allocate(OUTPUT_LENGTH).order(ByteOrder.LITTLE_ENDIAN);
        ByteBuffer in = ByteBuffer.wrap(metadata).order(ByteOrder.LITTLE_ENDIAN);

        // Metadata type
        out.put((byte) 0);
        // RX, RY, GX, GY, BX, BY, white X/Y, max/min mastering luminance,
        // max content luminance, max frame average luminance
        for (int i = 0; i < INPUT_LENGTH / 2; i++) {
            out.putShort(in.getShort());
        }

        out.rewind();
        return out;
    }
}
