#ifndef PEDAL_STORAGE_H
#define PEDAL_STORAGE_H

#include <stdint.h>
#include <string.h>

#include "daisy_seed.h"
#include "preset_bank.h"

constexpr uint32_t QSPI_SECTOR_BYTES = 4096;
constexpr uint32_t USER_PRESET_VALID = 1u;  // erased flash reads 0xFFFFFFFF

// Persistent Settings
struct Settings {
    int version;        // Version of the settings struct
    int currentPreset;  // 0 .. gPresets.count-1
    bool bypass;         // Persisted bypass state (true = pedal was bypassed at last save)

    // Overloading the != operator
    // This is necessary as this operator is used in the PersistentStorage source code
    bool operator!=(const Settings& a) const {
        return !(
            a.version == version &&
            a.currentPreset == currentPreset &&
            a.bypass == bypass
        );
    }
};

// One preset's saved edit. Every field is 4 bytes, so there is no padding and the
// memcmp below is exact. `valid` is last: QSPI pages are programmed in ascending
// order, so a slot whose `valid` reads USER_PRESET_VALID was written completely.
struct UserPreset {
    float    params[(int)::Parameter::Count];  // reverb->GetAllParameters() at save time
    float    reverseDelay;                     // state.reverseDelayNorm at save time
    float    delayLinesMax;                    // state.delayLinesMax at save time, 0.0 or 1.0
    float    reverseEnabled;                   // state.reverseEnabled at save time, 0.0 or 1.0
    float    reverseDirectMix;                 // state.reverseDirectMix at save time, 0.0 or 1.0
    uint32_t valid;                            // USER_PRESET_VALID, else load from presets.toml
};

struct UserPresets {
    uint32_t   firmwareHash;                   // firmwareImageHash() of the image that saved these
    UserPreset presets[kMaxPresets];

    // Required by PersistentStorage: decides whether Save() erases and writes.
    bool operator!=(const UserPresets& a) const { return memcmp(this, &a, sizeof *this) != 0; }
};

// +4: PersistentStorage prefixes its State word.
static_assert(sizeof(UserPresets) + 4 <= QSPI_SECTOR_BYTES,
              "user presets must fit one QSPI sector");

// Preset index + bypass (QSPI offset 0) and per-preset user edits (offset 0x1000).
class PedalStorage
{
public:
    explicit PedalStorage(daisy::QSPIHandle& qspi) : settings_(qspi), userPresets_(qspi) {}

    // Boot, before any other call: loads Settings (defaults on a SETTINGS_VERSION
    // mismatch) and the user presets (wiped when a different firmware image saved them).
    void Init();
    // The restored preset index, 0 if outside 0..presetCount-1. The corrected value
    // reaches flash through ServiceSettingsSave().
    int  RestoredPreset(int presetCount);
    bool RestoredBypass();
    // Main loop only. Mirrors pedal state into the RAM copy and writes flash
    // SETTINGS_SAVE_DELAY_MS after the last change.
    void ServiceSettingsSave(int currentPreset, bool bypass);
    // One preset's saved edit; valid != USER_PRESET_VALID means "use presets.toml".
    UserPreset& UserSlot(int preset) { return userPresets_.GetSettings().presets[preset]; }
    // Blocking QSPI erase + write of every slot; skipped when flash already matches.
    void SaveUserPresets() { userPresets_.Save(); }

private:
    daisy::PersistentStorage<Settings>    settings_;
    daisy::PersistentStorage<UserPresets> userPresets_;
    bool     settingsSavePending_ = false;  // RAM copy differs from what was last saved
    uint32_t settingsChangedAtMs_ = 0;      // System::GetNow() of the last change
};

#endif
