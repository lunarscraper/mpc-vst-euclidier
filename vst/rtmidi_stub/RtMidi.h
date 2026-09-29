/* rtmidi_stub/RtMidi.h -- in-process stand-in for RtMidi, used ONLY by the VST build.
 *
 * build.sh copies the vendored engine (../src: euclidier.cpp, eqseq.*, bjlund.*,
 * commontypes.h) into build/eng/ next to this header, so the engine's own
 * `#include "RtMidi.h"` resolves here instead of to the real RtMidi. No ports are
 * opened: everything the engine sends (notes from each lane, clock/CC feedback) goes
 * to euclidier_vst.cpp through g_euclidier_send, which forwards it to the plugin's
 * ALSA seq port. The engine's MIDI *input* is fed directly (handleMidi()), so the
 * RtMidiIn side is inert. */
#ifndef RTMIDI_STUB_H
#define RTMIDI_STUB_H
#include <string>
#include <vector>

extern void (*g_euclidier_send)(const std::vector<unsigned char> &msg);

class RtMidi {
public:
    enum Api { UNSPECIFIED };
    virtual ~RtMidi() {}
    unsigned int getPortCount() { return 0; }
    std::string getPortName(unsigned int = 0) { return ""; }
    void openPort(unsigned int = 0, const std::string & = "") {}
    void openVirtualPort(const std::string & = "") {}
    void closePort() {}
};

class RtMidiIn : public RtMidi {
public:
    typedef void (*RtMidiCallback)(double timeStamp, std::vector<unsigned char> *message, void *userData);
    RtMidiIn(Api = UNSPECIFIED, const std::string & = "", unsigned int = 100) {}
    void setCallback(RtMidiCallback, void * = 0) {}
    void cancelCallback() {}
    void ignoreTypes(bool = true, bool = true, bool = true) {}
};

class RtMidiOut : public RtMidi {
public:
    RtMidiOut(Api = UNSPECIFIED, const std::string & = "") {}
    void sendMessage(std::vector<unsigned char> *message) {
        if (message && !message->empty() && g_euclidier_send) g_euclidier_send(*message);
    }
    void sendMessage(const std::vector<unsigned char> *message) {
        if (message && !message->empty() && g_euclidier_send) g_euclidier_send(*message);
    }
};
#endif
