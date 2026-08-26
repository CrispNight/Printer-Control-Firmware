/*
 * moiren_link.h — framing and dispatch for protocol.h, shared by all three boards.
 *
 * Transport-agnostic above an Arduino Stream: USB CDC today, a hardware UART
 * between boards once the Teensy control card gains inter-board ports, and CAN
 * later by swapping the Stream for a different byte source. Nothing in
 * protocol.h changes when that happens.
 *
 * Receive is a byte-at-a-time state machine with no dynamic allocation — safe
 * on the Mega's 8 KB of RAM, and it resynchronises after a corrupt frame
 * instead of stalling.
 */

#ifndef MOIREN_LINK_H
#define MOIREN_LINK_H

#include <Arduino.h>
#include <stdint.h>

#include "protocol.h"

/* A received frame. `payload` points into the link's receive buffer and is
 * only valid for the duration of the callback — copy it if you need to keep
 * it past the handler returning. */
struct MoirenFrame {
    uint8_t        version;
    uint8_t        src;
    uint8_t        dst;
    uint8_t        msg;
    uint8_t        flags;
    uint8_t        seq;
    uint8_t        len;
    const uint8_t *payload;
};

class MoirenLink {
public:
    typedef void (*FrameHandler)(const MoirenFrame &frame, void *ctx);

    MoirenLink(Stream &port, uint8_t self_node)
        : port_(port), self_node_(self_node), handler_(0), handler_ctx_(0),
          state_(WAIT_SOF0), idx_(0), len_(0), tx_seq_(0),
          frames_rx_(0), crc_errors_(0), proto_errors_(0) {}

    void onFrame(FrameHandler cb, void *ctx = 0)
    {
        handler_ = cb;
        handler_ctx_ = ctx;
    }

    /* Drain the port and dispatch every complete frame. Call from loop().
     * Bounded per call so a flooded port cannot starve the rest of the loop. */
    void poll(uint8_t max_bytes = 128)
    {
        while (max_bytes-- && port_.available() > 0) {
            feedByte((uint8_t)port_.read());
        }
    }

    /* Send a frame. Returns false if the payload does not fit. */
    bool send(uint8_t dst, uint8_t msg, const void *payload, uint8_t len,
              uint8_t flags = 0);

    bool sendEmpty(uint8_t dst, uint8_t msg, uint8_t flags = 0)
    {
        return send(dst, msg, 0, 0, flags);
    }

    /* Typed convenience: link.sendStruct(NODE_PC, MSG_STATE, state); */
    template <typename T>
    bool sendStruct(uint8_t dst, uint8_t msg, const T &payload, uint8_t flags = 0)
    {
        return send(dst, msg, &payload, (uint8_t)sizeof(T), flags);
    }

    /* Reply to `frame` with a sys_ack_t. */
    bool sendAck(const MoirenFrame &frame, uint8_t status);

    /* Convenience: MSG_LOG with a NUL-padded message. */
    bool sendLog(uint8_t dst, uint8_t level, const char *text);

    uint8_t  node() const { return self_node_; }
    uint8_t  nextSeq() { return tx_seq_; }
    uint16_t framesRx() const { return frames_rx_; }
    uint16_t crcErrors() const { return crc_errors_; }
    uint16_t protoErrors() const { return proto_errors_; }

private:
    enum RxState { WAIT_SOF0, WAIT_SOF1, IN_HEADER, IN_PAYLOAD, IN_CRC };

    void feedByte(uint8_t b);
    void dispatch();

    Stream      &port_;
    uint8_t      self_node_;
    FrameHandler handler_;
    void        *handler_ctx_;

    RxState  state_;
    uint16_t idx_;
    uint8_t  len_;
    /* VER SRC DST MSG FLAGS SEQ LEN + payload + CRC lo/hi */
    uint8_t  rx_[FRAME_HEADER_LEN - 2 + FRAME_MAX_PAYLOAD + FRAME_CRC_LEN];
    uint8_t  tx_[FRAME_MAX_LEN];

    uint8_t  tx_seq_;
    uint16_t frames_rx_;
    uint16_t crc_errors_;
    uint16_t proto_errors_;
};

#endif /* MOIREN_LINK_H */
