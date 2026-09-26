#include "usb_midi_link.h"

using namespace daisy;

// CDC_Transmit_FS() returns busy while the previous IN transfer is in flight.
constexpr int      kTxRetries      = 10;
constexpr uint32_t kTxRetryDelayUs = 100;

void UsbMidiLink::Init() {
    MidiUsbTransport::Config config;
    config.periph = MidiUsbTransport::Config::INTERNAL;
    transport_.Init(config);
    transport_.StartRx(rxCallback, this);
}

void UsbMidiLink::Service() {
    // MidiUsbTransport stops receiving when its ring buffer overflows
    // (hid/usb_midi.cpp:187-192); it drains on every packet, so this is a safety net.
    if (!transport_.RxActive())
        transport_.StartRx(rxCallback, this);
}

void UsbMidiLink::rxCallback(uint8_t* data, size_t size, void* context) {
    static_cast<UsbMidiLink*>(context)->assembler_.Feed(data, size);
}

void UsbMidiLink::SendSysEx(const uint8_t* msg, size_t len) {
    const size_t n = PackSysExUsbMidi(msg, len, tx_, sizeof tx_);
    if (n == 0)
        return;
    for (int attempt = 0; attempt <= kTxRetries; ++attempt) {
        if (usb_.TransmitInternal(tx_, n) == UsbHandle::Result::OK)
            return;
        System::DelayUs(kTxRetryDelayUs);
    }
}
