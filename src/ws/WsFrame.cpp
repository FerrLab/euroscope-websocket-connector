#include "WsFrame.h"

namespace Ws
{
    std::vector<uint8_t> EncodeFrame(Opcode opcode,
                                     const uint8_t* payload, size_t len,
                                     const uint8_t mask[4],
                                     bool fin)
    {
        std::vector<uint8_t> out;
        out.reserve(len + 14);

        out.push_back(static_cast<uint8_t>((fin ? 0x80 : 0x00) |
                                           (static_cast<uint8_t>(opcode) & 0x0F)));

        // MASK bit always set: client->server frames must be masked.
        if (len < 126)
        {
            out.push_back(static_cast<uint8_t>(0x80 | len));
        }
        else if (len <= 0xFFFF)
        {
            out.push_back(0x80 | 126);
            out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>(len & 0xFF));
        }
        else
        {
            out.push_back(0x80 | 127);
            const uint64_t len64 = len;
            for (int i = 7; i >= 0; --i)
                out.push_back(static_cast<uint8_t>((len64 >> (i * 8)) & 0xFF));
        }

        out.push_back(mask[0]);
        out.push_back(mask[1]);
        out.push_back(mask[2]);
        out.push_back(mask[3]);

        for (size_t i = 0; i < len; ++i)
            out.push_back(static_cast<uint8_t>(payload[i] ^ mask[i % 4]));

        return out;
    }

    std::vector<uint8_t> EncodeText(const std::string& text, const uint8_t mask[4])
    {
        return EncodeFrame(Opcode::Text,
                           reinterpret_cast<const uint8_t*>(text.data()),
                           text.size(), mask, true);
    }

    DecodeResult TryDecodeFrame(const uint8_t* data, size_t len,
                                Frame& out, size_t& consumed,
                                std::string& error)
    {
        if (len < 2)
            return DecodeResult::NeedMore;

        const uint8_t b0 = data[0];
        const uint8_t b1 = data[1];

        out.fin = (b0 & 0x80) != 0;
        if ((b0 & 0x70) != 0)
        {
            error = "reserved bits set (extensions are not negotiated)";
            return DecodeResult::Error;
        }
        out.opcode = static_cast<Opcode>(b0 & 0x0F);
        switch (out.opcode)
        {
            case Opcode::Continuation:
            case Opcode::Text:
            case Opcode::Binary:
            case Opcode::Close:
            case Opcode::Ping:
            case Opcode::Pong:
                break;
            default:
                error = "unknown opcode";
                return DecodeResult::Error;
        }

        const bool masked = (b1 & 0x80) != 0;
        uint64_t payloadLen = b1 & 0x7F;
        size_t pos = 2;

        if (payloadLen == 126)
        {
            if (len < pos + 2)
                return DecodeResult::NeedMore;
            payloadLen = (static_cast<uint64_t>(data[pos]) << 8) | data[pos + 1];
            pos += 2;
        }
        else if (payloadLen == 127)
        {
            if (len < pos + 8)
                return DecodeResult::NeedMore;
            payloadLen = 0;
            for (int i = 0; i < 8; ++i)
                payloadLen = (payloadLen << 8) | data[pos + i];
            pos += 8;
        }

        if (payloadLen > kMaxPayload)
        {
            error = "frame payload exceeds limit";
            return DecodeResult::Error;
        }

        uint8_t mask[4] = { 0, 0, 0, 0 };
        if (masked)
        {
            if (len < pos + 4)
                return DecodeResult::NeedMore;
            mask[0] = data[pos];
            mask[1] = data[pos + 1];
            mask[2] = data[pos + 2];
            mask[3] = data[pos + 3];
            pos += 4;
        }

        if (len < pos + payloadLen)
            return DecodeResult::NeedMore;

        out.payload.resize(static_cast<size_t>(payloadLen));
        for (size_t i = 0; i < payloadLen; ++i)
            out.payload[i] = masked ? static_cast<uint8_t>(data[pos + i] ^ mask[i % 4])
                                    : data[pos + i];

        consumed = pos + static_cast<size_t>(payloadLen);
        return DecodeResult::Ok;
    }

    MessageAssembler::Event MessageAssembler::Feed(const Frame& frame)
    {
        switch (frame.opcode)
        {
            case Opcode::Ping:
                control = frame.payload;
                return Event::Ping;
            case Opcode::Pong:
                control = frame.payload;
                return Event::Pong;
            case Opcode::Close:
                control = frame.payload;
                return Event::Close;

            case Opcode::Text:
            case Opcode::Binary:
                if (m_assembling)
                {
                    error = "new data frame while a fragmented message is in progress";
                    return Event::Error;
                }
                if (frame.fin)
                {
                    message.assign(frame.payload.begin(), frame.payload.end());
                    return Event::Message;
                }
                m_buffer = frame.payload;
                m_assembling = true;
                return Event::None;

            case Opcode::Continuation:
                if (!m_assembling)
                {
                    error = "continuation frame without a message in progress";
                    return Event::Error;
                }
                if (m_buffer.size() + frame.payload.size() > kMaxPayload)
                {
                    error = "fragmented message exceeds limit";
                    m_assembling = false;
                    m_buffer.clear();
                    return Event::Error;
                }
                m_buffer.insert(m_buffer.end(), frame.payload.begin(),
                                frame.payload.end());
                if (frame.fin)
                {
                    message.assign(m_buffer.begin(), m_buffer.end());
                    m_buffer.clear();
                    m_assembling = false;
                    return Event::Message;
                }
                return Event::None;
        }
        error = "unhandled opcode";
        return Event::Error;
    }
}
