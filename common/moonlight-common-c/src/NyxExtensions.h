#pragma once

// Control stream protocol extensions that are not part of the GameStream protocol.
// This header is plain C and C++ safe: the library, the NyxPlay Host and the clients
// all take their wire constants from here so that the tables cannot drift apart.

// Sunshine protocol extensions (encrypted control stream only)
#define LI_PACKET_TYPE_RUMBLE_TRIGGERS     0x5500
#define LI_PACKET_TYPE_SET_MOTION_EVENT    0x5501
#define LI_PACKET_TYPE_SET_RGB_LED         0x5502
#define LI_PACKET_TYPE_SET_ADAPTIVE_TRIGGERS 0x5503

// Apollo protocol extensions
#define LI_PACKET_TYPE_EXEC_SERVER_CMD     0x3000
#define LI_PACKET_TYPE_SET_CLIPBOARD       0x3001
#define LI_PACKET_TYPE_FILE_TRANSFER_NONCE_REQUEST 0x3002

// NyxPlay protocol extension: host text input focus state. The value lies outside of
// the Sunshine (0x55xx) and Apollo (0x30xx) ranges. Only sent to clients that opted in
// with the "nyxTextInput=1" launch parameter.
#define LI_PACKET_TYPE_SET_TEXT_INPUT_STATE 0x4E01

// Payload of LI_PACKET_TYPE_SET_TEXT_INPUT_STATE: version, active, flags, reserved
#define LI_TEXT_INPUT_PAYLOAD_VERSION 1
#define LI_TEXT_INPUT_PAYLOAD_SIZE    4

#define LI_TEXT_INPUT_FLAG_PASSWORD  0x01
#define LI_TEXT_INPUT_FLAG_MULTILINE 0x02
