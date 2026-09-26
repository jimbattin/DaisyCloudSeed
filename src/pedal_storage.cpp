#include "pedal_storage.h"

// Increment this when changing the settings struct so the software will know
// to reset to defaults if this ever changes.
constexpr int SETTINGS_VERSION = 2;

// Settings are written to flash this long after the last preset/bypass change, so a
// burst of footswitch presses costs one QSPI sector erase instead of one per press.
constexpr uint32_t SETTINGS_SAVE_DELAY_MS = 3000;

// User preset edits live in the 4 KB QSPI sector after Settings (offset 0, sector 0).
// Both sit in the first 256 KB the Daisy bootloader never touches.
constexpr uint32_t USER_PRESETS_QSPI_OFFSET = 0x1000;

extern "C" {
    // Linker symbols bounding .text + .rodata (libdaisy/core/STM32H750IB_sram.lds:36,47),
    // which include the embedded presets.toml. Used as the firmware identity.
    extern const uint32_t _stext[];
    extern const uint32_t _etext[];
}

// FNV-1a over the loaded code + read-only data. Any change to code or presets.toml
// changes it, so saved edits never outlive the firmware that wrote them.
static uint32_t firmwareImageHash() {
    uint32_t h = 2166136261u;
    for (const uint32_t* p = _stext; p < _etext; ++p) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

void PedalStorage::Init() {
    const Settings defaults = {SETTINGS_VERSION, 0, true};  // preset 0, bypassed on first boot
    settings_.Init(defaults);
    const uint32_t hash = firmwareImageHash();
    UserPresets userDefaults = {};
    userDefaults.firmwareHash = hash;
    userPresets_.Init(userDefaults, USER_PRESETS_QSPI_OFFSET);
    // Edits written by a different firmware image are discarded (one sector erase).
    if (userPresets_.GetSettings().firmwareHash != hash)
        userPresets_.RestoreDefaults();
    // A layout change (SETTINGS_VERSION mismatch) discards the stored struct.
    if (settings_.GetSettings().version != SETTINGS_VERSION)
        settings_.RestoreDefaults();
}

int PedalStorage::RestoredPreset(int presetCount) {
    const Settings& s = settings_.GetSettings();
    return (s.currentPreset >= 0 && s.currentPreset < presetCount) ? s.currentPreset : 0;
}

bool PedalStorage::RestoredBypass() {
    return settings_.GetSettings().bypass;
}

// Mirrors pedal state into the RAM copy of the settings and writes flash
// SETTINGS_SAVE_DELAY_MS after the last change, so a burst of preset/bypass changes
// costs one QSPI sector erase. Save() skips the erase entirely when flash already
// matches (PersistentStorage::StoreSettingsIfChanged).
void PedalStorage::ServiceSettingsSave(int currentPreset, bool bypass) {
    Settings& s = settings_.GetSettings();
    if (s.currentPreset != currentPreset || s.bypass != bypass) {
        s.currentPreset      = currentPreset;
        s.bypass             = bypass;
        settingsSavePending_ = true;
        settingsChangedAtMs_ = daisy::System::GetNow();
    }
    if (settingsSavePending_
        && daisy::System::GetNow() - settingsChangedAtMs_ >= SETTINGS_SAVE_DELAY_MS) {
        settings_.Save();
        settingsSavePending_ = false;
    }
}
