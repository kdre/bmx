#ifndef BMX_REMOTE_BMX_API_TYPES_H
#define BMX_REMOTE_BMX_API_TYPES_H

#include "menu_control.h"

#include <stddef.h>
#include <stdint.h>

namespace bmx {
namespace remote {

static const size_t kBmxApiMaximumInputEvents = 64U;
static const size_t kBmxApiMaximumTextBytes = 16U * 1024U;
static const size_t kBmxApiPathBytes = 512U;
static const size_t kBmxApiFileNameBytes = 256U;
static const size_t kBmxApiMaximumFileEntries = 16U;
static const size_t kBmxApiMaximumStorageVolumes = 6U;
static const size_t kBmxApiMaximumMediaSlots = 8U;
static const size_t kBmxApiMediaSlotKeyBytes = 32U;
static const size_t kBmxDeveloperMemoryMaximumTransferBytes = 64U * 1024U;
static const int kBmxApiMaximumKeycode = 255;
static const int kBmxApiMaximumJoystickDevice = 22;

enum class BmxApiOperation : uint8_t {
    None = 0,
    State,
    Menu,
    ListControls,
    ListActions,
    DescribeControl,
    SetControl,
    InvokeAction,
    Input,
    TextInput,
    Screenshot,
    Audio,
    AudioWav,
    Storage,
    Files,
    Media,
    DeveloperMemoryRead
};

enum class BmxInputType : uint8_t {
    Key = 0,
    Joystick,
    Mouse
};

enum class BmxKeyAction : uint8_t {
    Down = 0,
    Up,
    Tap
};

enum class BmxMenuAction : uint8_t {
    Open = 0,
    Close,
    Toggle
};

struct BmxInputEvent {
    BmxInputType type;
    BmxKeyAction key_action;
    int keycode;
    int modifiers;
    int joystick_port;
    int joystick_device;
    int joystick_value;
    int mouse_dx;
    int mouse_dy;
    int mouse_buttons;
    int mouse_wheel;
};

struct BmxControlState {
    char board[8U];
    char machine[32U];
    char release_version[65U];
    uint64_t uptime_ms;
    bool network_ready;
    uint32_t heap_free_kb;
    uint32_t heap_low_free_kb;
    uint32_t arm_clock_hz;
    uint32_t emu_cycles_per_sec;
    uint32_t target_fps_milli;
    uint32_t actual_fps_milli;
    int temperature_c;
    uint32_t throttle_clock_hz;
    char video_output[12U];
    uint32_t display_width;
    uint32_t display_height;
    char audio_output[12U];
    uint32_t audio_sample_rate;
    uint32_t audio_channels;
    uint32_t audio_queue_frames;
    uint32_t audio_queue_fill_frames;
    uint32_t audio_queue_min_fill_frames;
    uint64_t audio_write_waits;
    uint64_t audio_capture_drops;
    bool menu_visible;
    bool warp;
    bool diagnostics_overlay;
};

struct BmxBinaryPayload {
    uint8_t *data;
    size_t size;
    uint32_t width;
    uint32_t height;
    uint32_t sample_rate;
    uint32_t channels;
    bool wav;
};

struct BmxStorageVolume {
    char name[8U];
    bool mounted;
    uint64_t total_bytes;
    uint64_t free_bytes;
};

struct BmxStorageState {
    BmxStorageVolume volumes[kBmxApiMaximumStorageVolumes];
    size_t count;
};

struct BmxFileEntry {
    char name[kBmxApiFileNameBytes];
    uint64_t size;
    bool directory;
    bool read_only;
};

struct BmxFilePage {
    char path[kBmxApiPathBytes];
    BmxFileEntry entries[kBmxApiMaximumFileEntries];
    size_t count;
    bool has_more;
    char next_after[kBmxApiFileNameBytes];
};

enum class BmxMediaKind : uint8_t {
    Unknown = 0,
    Disk,
    Tape,
    Cartridge,
    Floppy,
    HardDisk
};

struct BmxMediaSlot {
    char key[kBmxApiMediaSlotKeyBytes];
    BmxMediaKind kind;
    char path[kBmxApiPathBytes];
};

struct BmxMediaState {
    BmxMediaSlot slots[kBmxApiMaximumMediaSlots];
    size_t count;
};

struct BmxApiRequest {
    BmxApiOperation operation;
    BmxMenuAction menu_action;
    char key[MENU_CONTROL_KEY_SIZE];
    char after[MENU_CONTROL_KEY_SIZE];
    char path[kBmxApiPathBytes];
    char file_after[kBmxApiFileNameBytes];
    size_t limit;
    menu_control_value value;
    BmxInputEvent input[kBmxApiMaximumInputEvents];
    size_t input_count;
    char *text;
    size_t text_size;
    uint32_t width;
    uint32_t duration_ms;
    uint32_t memory_address;
    size_t memory_size;
};

struct BmxApiResponse {
    BmxApiOperation operation;
    menu_control_status status;
    menu_control_description control;
    menu_control_page page;
    BmxControlState state;
    BmxStorageState storage;
    BmxFilePage files;
    BmxMediaState media;
    BmxBinaryPayload binary;
    size_t text_queued;
    bool text_accepted;
};

}  // namespace remote
}  // namespace bmx

#endif
