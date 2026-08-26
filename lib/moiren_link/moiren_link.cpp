#include "moiren_link.h"

#include <string.h>

/* Offsets within rx_, which starts at VER (the two SOF bytes are not stored). */
#define RX_VER   0
#define RX_SRC   1
#define RX_DST   2
#define RX_MSG   3
#define RX_FLAGS 4
#define RX_SEQ   5
#define RX_LEN   6
#define RX_HDR   7  /* PACKET_HEADER_LEN - 2 */

void MoirenLink::feedByte(uint8_t b)
{
    switch (state_) {
    case WAIT_SOF0:
        if (b == PACKET_SOF0) {
            state_ = WAIT_SOF1;
        }
        break;

    case WAIT_SOF1:
        if (b == PACKET_SOF1) {
            idx_ = 0;
            state_ = IN_HEADER;
        } else if (b != PACKET_SOF0) {
            /* Stay in WAIT_SOF1 on a repeated SOF0: A5 A5 5A is a valid start. */
            state_ = WAIT_SOF0;
        }
        break;

    case IN_HEADER:
        rx_[idx_++] = b;
        if (idx_ == RX_HDR) {
            len_ = rx_[RX_LEN];
            if (len_ > PACKET_MAX_PAYLOAD) {
                proto_errors_++;
                state_ = WAIT_SOF0;  /* bogus length: not a real packet start */
            } else {
                state_ = len_ ? IN_PAYLOAD : IN_CRC;
            }
        }
        break;

    case IN_PAYLOAD:
        rx_[idx_++] = b;
        if (idx_ == (uint16_t)(RX_HDR + len_)) {
            state_ = IN_CRC;
        }
        break;

    case IN_CRC:
        rx_[idx_++] = b;
        if (idx_ == (uint16_t)(RX_HDR + len_ + PACKET_CRC_LEN)) {
            const uint16_t want = crc16_ccitt(rx_, (uint16_t)(RX_HDR + len_));
            const uint16_t got  = (uint16_t)rx_[RX_HDR + len_] |
                                  ((uint16_t)rx_[RX_HDR + len_ + 1] << 8);
            if (want == got) {
                packets_rx_++;
                dispatch();
            } else {
                crc_errors_++;
            }
            state_ = WAIT_SOF0;
        }
        break;
    }
}

void MoirenLink::dispatch()
{
    /* Not for us and not a broadcast: drop it. Once the Teensy routes for the
     * other boards it will forward here instead, keyed on packet.dst. */
    if (rx_[RX_DST] != self_node_ && rx_[RX_DST] != NODE_BROADCAST) {
        return;
    }

    if (rx_[RX_VER] != PROTOCOL_VERSION) {
        proto_errors_++;
        fault_report_t fault;
        fault.code   = FAULT_VERSION_MISMATCH;
        fault.node   = self_node_;
        fault.detail = rx_[RX_VER];
        sendStruct(rx_[RX_SRC], MSG_FAULT, fault, FLAG_IS_ERROR);
        return;
    }

    if (!handler_) {
        return;
    }

    packet_t packet;
    packet.version = rx_[RX_VER];
    packet.src     = rx_[RX_SRC];
    packet.dst     = rx_[RX_DST];
    packet.msg     = rx_[RX_MSG];
    packet.flags   = rx_[RX_FLAGS];
    packet.seq     = rx_[RX_SEQ];
    packet.len     = len_;
    packet.payload = &rx_[RX_HDR];
    handler_(packet, handler_ctx_);
}

bool MoirenLink::send(uint8_t dst, uint8_t msg, const void *payload, uint8_t len,
                      uint8_t flags)
{
    if (len > PACKET_MAX_PAYLOAD) {
        return false;
    }

    tx_[0] = PACKET_SOF0;
    tx_[1] = PACKET_SOF1;
    tx_[2] = PROTOCOL_VERSION;
    tx_[3] = self_node_;
    tx_[4] = dst;
    tx_[5] = msg;
    tx_[6] = flags;
    tx_[7] = tx_seq_++;
    tx_[8] = len;
    if (len && payload) {
        memcpy(&tx_[PACKET_HEADER_LEN], payload, len);
    }

    /* CRC covers VER..payload, i.e. everything after the two SOF bytes. */
    const uint16_t crc = crc16_ccitt(&tx_[2], (uint16_t)(RX_HDR + len));
    tx_[PACKET_HEADER_LEN + len]     = (uint8_t)(crc & 0xFF);
    tx_[PACKET_HEADER_LEN + len + 1] = (uint8_t)(crc >> 8);

    const size_t total = (size_t)PACKET_HEADER_LEN + len + PACKET_CRC_LEN;
    return port_.write(tx_, total) == total;
}

bool MoirenLink::sendAck(const packet_t &packet, uint8_t status)
{
    sys_ack_t ack;
    ack.ack_msg = packet.msg;
    ack.ack_seq = packet.seq;
    ack.status  = status;
    return sendStruct(packet.src, MSG_ACK, ack, FLAG_IS_RESPONSE);
}

bool MoirenLink::sendLog(uint8_t dst, uint8_t level, const char *text)
{
    sys_log_t entry;
    entry.level = level;
    memset(entry.text, 0, sizeof(entry.text));
    /* text is NUL-padded, not necessarily NUL-terminated — the length is
     * carried by the struct, so a full 48-character message is legal. */
    size_t n = strlen(text);
    if (n > sizeof(entry.text)) {
        n = sizeof(entry.text);
    }
    memcpy(entry.text, text, n);
    return sendStruct(dst, MSG_LOG, entry);
}
