#ifndef BMX_UPDATE_UPDATE_SERVICE_H
#define BMX_UPDATE_UPDATE_SERVICE_H

#include "update/update_foreground_progress.h"

namespace bmx {
namespace update {

enum class UpdateServiceOperation : uint8_t {
    Check = 0,
    DraftBegin,
    DraftComplete,
    Cancel,
    Install
};

static const int kUpdateServiceRebootReady = 100;

// These functions are reachable only through the explicit BMX Update menu
// action. No constructor, boot hook, timer or network callback invokes online
// discovery.
int CheckForUpdateFromMenu(
    char *message, unsigned message_size,
    UpdateForegroundProgress *progress_override = 0);
int PreparedDraftTestAvailableForMenu();
int BeginPreparedDraftTestFromMenu(
    char *message, unsigned message_size,
    UpdateForegroundProgress *progress_override = 0);
int CompletePreparedDraftTestFromMenu(
    char *message, unsigned message_size,
    UpdateForegroundProgress *progress_override = 0);
void CancelPendingUpdateFromMenu();
int InstallCheckedUpdateFromMenu(
    bool destructive_reset_consent, char *message, unsigned message_size,
    UpdateForegroundProgress *progress_override = 0,
    bool defer_reboot = false);
bool ReadInstalledVersionForMenu(char *version, unsigned version_size);

// Called only by NetworkService's pre-created Core-0 worker.
int ExecuteNetworkServiceOperation(
    UpdateServiceOperation operation, bool destructive_reset_consent,
    char *message, unsigned message_size,
    UpdateForegroundProgress *progress);

// Completes the VICE/storage shutdown on the caller core after a Core-0
// install job has published kUpdateServiceRebootReady.
int CompleteDeferredUpdateReboot(char *message, unsigned message_size);

}  // namespace update
}  // namespace bmx

#endif  // BMX_UPDATE_UPDATE_SERVICE_H
