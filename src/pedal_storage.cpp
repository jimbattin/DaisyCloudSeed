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

uint32_t PedalStorage::imageHash() {
    if (!imageHashValid_) {
        imageHash_      = firmwareImageHash();
        imageHashValid_ = true;
    }
    return imageHash_;
}

void PedalStorage::Init(uint32_t presetBankHash) {
    const Settings defaults = {SETTINGS_VERSION, 0, true};  // preset 0, bypassed on first boot
    settings_.Init(defaults);
    // Saved edits belong to one firmware image running one preset bank: an upload,
    // a revert or a reflash all discard them.
    const uint32_t hash = (imageHash() ^ presetBankHash) * 16777619u;
    UserPresets userDefaults = {};
    userDefaults.identity = hash;
    userPresets_.Init(userDefaults, USER_PRESETS_QSPI_OFFSET);
    // Edits written by a different firmware image or bank are discarded (one sector erase).
    if (userPresets_.GetSettings().identity != hash)
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

const char* PedalStorage::StoredBankText(uint32_t& length) {
    const StoredBankHeader& header =
        *static_cast<const StoredBankHeader*>(qspi_.GetData(STORED_BANK_HEADER_OFFSET));
    return ValidStoredBankText(header,
                               static_cast<const char*>(qspi_.GetData(STORED_BANK_TEXT_OFFSET)),
                               imageHash(), length);
}

// The QSPI window is cached by the M7 (PersistentStorage.h does the same), so each
// read-back is invalidated before it is compared.
static bool flashMatches(daisy::QSPIHandle& qspi, uint32_t offset, const void* data, uint32_t size) {
    uint8_t* mapped = static_cast<uint8_t*>(qspi.GetData(offset));
    dsy_dma_invalidate_cache_for_buffer(mapped, size);
    return memcmp(mapped, data, size) == 0;
}

bool PedalStorage::WriteStoredBank(const char* text, uint32_t length, uint32_t textHash) {
    using Result = daisy::QSPIHandle::Result;
    if (qspi_.Erase(STORED_BANK_HEADER_OFFSET, STORED_BANK_TEXT_OFFSET + length) != Result::OK)
        return false;
    // Write() only reads the buffer; the API just is not const-correct.
    if (qspi_.Write(STORED_BANK_TEXT_OFFSET, length,
                    reinterpret_cast<uint8_t*>(const_cast<char*>(text))) != Result::OK
        || !flashMatches(qspi_, STORED_BANK_TEXT_OFFSET, text, length))
        return false;
    StoredBankHeader header = {STORED_BANK_MAGIC, length, textHash, imageHash()};
    return qspi_.Write(STORED_BANK_HEADER_OFFSET, sizeof header,
                       reinterpret_cast<uint8_t*>(&header)) == Result::OK
           && flashMatches(qspi_, STORED_BANK_HEADER_OFFSET, &header, sizeof header);
}

bool PedalStorage::EraseStoredBank() {
    return qspi_.Erase(STORED_BANK_HEADER_OFFSET, STORED_BANK_HEADER_OFFSET + QSPI_SECTOR_BYTES)
           == daisy::QSPIHandle::Result::OK;
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

void PedalStorage::FlushSettingsSave(int currentPreset, bool bypass) {
    ServiceSettingsSave(currentPreset, bypass);  // mirror the latest state
    if (settingsSavePending_) {
        settings_.Save();
        settingsSavePending_ = false;
    }
}
