/*
 * moiren_link.h — framing and dispatch for protocol.h, shared by all three boards.
 *
 * Transport-agnostic above an Arduino Stream: USB CDC today, a hardware UART
 * between boards once the Teensy control card gains inter-board ports, and CAN
 * later by swapping the Stream for a different byte source. Nothing in
 * protocol.h changes when that happens.
 *
 * Receive is a byte-at-a-time state machine with no dynamic allocation — safe
 * on the Mega's 8 KB of RAM, and it resynchronises after a corrupt packet
 * instead of stalling.
 */

#ifndef MOIREN_LINK_H
#define MOIREN_LINK_H

#include <Arduino.h>
#include <stdint.h>

#include "protocol.h"

/* A received packet. `payload` points into the link's receive buffer and is
 * only valid for the duration of the callback — copy it if you need to keep
 * it past the handler returning. */
struct packet_t {
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
    typedef void (*PacketHandler)(const packet_t &packet, void *ctx);

    MoirenLink(Stream &port, uint8_t self_node)
        : port_(port), self_node_(self_node), handler_(0), handler_ctx_(0),
          state_(WAIT_SOF0), idx_(0), len_(0), tx_seq_(0),
          packets_rx_(0), crc_errors_(0), proto_errors_(0) {}

    void onPacket(PacketHandler cb, void *ctx = 0)
    {
        handler_ = cb;
        handler_ctx_ = ctx;
    }

    /* Drain the port and dispatch every complete packet. Call from loop().
     * Bounded per call so a flooded port cannot starve the rest of the loop. */
    void poll(uint8_t max_bytes = 128)
    {
        while (max_bytes-- && port_.available() > 0) {
            feedByte((uint8_t)port_.read());
        }
    }

    /* Send a packet. Returns false if the payload does not fit. */
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

    /* Reply to `packet` with a sys_ack_t. */
    bool sendAck(const packet_t &packet, uint8_t status);

    /* Convenience: MSG_LOG with a NUL-padded message. */
    bool sendLog(uint8_t dst, uint8_t level, const char *text);

    uint8_t  node() const { return self_node_; }
    uint8_t  nextSeq() { return tx_seq_; }
    uint16_t packetsRx() const { return packets_rx_; }
    uint16_t crcErrors() const { return crc_errors_; }
    uint16_t protoErrors() const { return proto_errors_; }

private:
    enum RxState { WAIT_SOF0, WAIT_SOF1, IN_HEADER, IN_PAYLOAD, IN_CRC };

    void feedByte(uint8_t b);
    void dispatch();

    Stream      &port_;
    uint8_t      self_node_;
    PacketHandler handler_;
    void        *handler_ctx_;

    RxState  state_;
    uint16_t idx_;
    uint8_t  len_;
    /* VER SRC DST MSG FLAGS SEQ LEN + payload + CRC lo/hi */
    uint8_t  rx_[PACKET_HEADER_LEN - 2 + PACKET_MAX_PAYLOAD + PACKET_CRC_LEN];
    uint8_t  tx_[PACKET_MAX_LEN];

    uint8_t  tx_seq_;
    uint16_t packets_rx_;
    uint16_t crc_errors_;
    uint16_t proto_errors_;
};

#endif /* MOIREN_LINK_H */
