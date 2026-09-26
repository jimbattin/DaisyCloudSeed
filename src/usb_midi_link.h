#ifndef USB_MIDI_LINK_H
#define USB_MIDI_LINK_H

#include <stddef.h>
#include <stdint.h>

#include "daisy_seed.h"
#include "preset_protocol.h"

// The Seed's micro-USB port as a class-compliant USB-MIDI device carrying the preset
// protocol (preset_protocol.h). Receive uses libdaisy's MidiUsbTransport directly:
// MidiHandler truncates SysEx at SYSEX_BUFFER_LEN = 128 (hid/MidiEvent.h:2). Replies
// bypass MidiUsbTransport::Tx(), which splits at every status byte and so sends the
// closing F7 in a packet of its own (hid/usb_midi.cpp:294-318); they are packed by
// PackSysExUsbMidi() and sent with UsbHandle::TransmitInternal(), the same CDC path.
//
// The USB OTG FS interrupt shares priority 0 with the audio DMA (usbd/usbd_conf.c:102-107,
// sys/dma.c:17-50), so the receive callback does nothing but SysExAssembler::Feed().
class UsbMidiLink
{
public:
    void Init();     // boot, before hw.StartAudio(); blocks ~10 ms (usb_midi.cpp:125)
    void Service();  // main loop: restarts reception after a ring-buffer overflow
    bool FrameReady() const { return assembler_.Ready(); }
    const uint8_t* Frame() const { return assembler_.Frame(); }
    size_t FrameLength() const { return assembler_.Length(); }
    void ReleaseFrame() { assembler_.Release(); }
    // Main loop. `msg` is one complete SysEx message, F0 ... F7, at most
    // PresetProtocol::kMaxReply bytes. Dropped if the CDC endpoint stays busy.
    void SendSysEx(const uint8_t* msg, size_t len);

private:
    static void rxCallback(uint8_t* data, size_t size, void* context);  // USB ISR

    daisy::MidiUsbTransport transport_;
    daisy::UsbHandle        usb_;  // stateless; TransmitInternal() only
    SysExAssembler          assembler_;
    // Must outlive the asynchronous CDC IN transfer: the next SendSysEx() overwrites it
    // only after a new frame has been received and handled.
    uint8_t tx_[(PresetProtocol::kMaxReply + 2) / 3 * 4];
};

#endif
