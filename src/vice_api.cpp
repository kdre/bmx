#include "vice_api.h"

#include "machines/machine_descriptor.h"
#include "viceoptions.h"

#include <cstdio>
#include <cstring>
extern "C" {
#include "bmc64_log.h"
#include "main.h"
#include "rs232drv/rs232.h"
#include "rs232drv/rs232net.h"
#include "version.h"
#include "third_party/common/circle.h"
}

#if defined(VICE_VERSION_MAJOR) && \
    ((VICE_VERSION_MAJOR > 3) || \
     (VICE_VERSION_MAJOR == 3 && VICE_VERSION_MINOR >= 10))
#define BMC64_USE_VICE_310_ARGS 1
#endif

namespace bmc64::vice {

namespace {

const char *Basename(const char *path) {
  if (path == nullptr) {
    return nullptr;
  }

  const char *last_slash = std::strrchr(path, '/');
  return last_slash == nullptr ? path : last_slash + 1;
}

bool HasSavedViceSettings() {
  const char *paths[] = {"/vicerc", "/vice.ini"};
  for (const char *path : paths) {
    FILE *file = std::fopen(path, "rb");
    if (file != nullptr) {
      std::fclose(file);
      return true;
    }
  }
  return false;
}

const char *Rs232InterfaceKey(TBmxRs232Interface interface) {
  switch (interface) {
    case BMX_RS232_INTERFACE_UP9600:
      return "up9600";
    case BMX_RS232_INTERFACE_SWIFT_DE:
      return "swift-de";
    case BMX_RS232_INTERFACE_SWIFT_DF:
      return "swift-df";
    case BMX_RS232_INTERFACE_SWIFT_D7:
      return "swift-d7";
    default:
      return "userport";
  }
}

const char *Rs232AciaBase(TBmxRs232Interface interface) {
  switch (interface) {
    case BMX_RS232_INTERFACE_SWIFT_DF:
      return "57088";
    case BMX_RS232_INTERFACE_SWIFT_D7:
      return "55040";
    case BMX_RS232_INTERFACE_SWIFT_DE:
    default:
      return "56832";
  }
}

unsigned MaxRs232Baud(TBmxRs232Interface interface) {
  switch (interface) {
    case BMX_RS232_INTERFACE_USERPORT:
      return 2400;
    case BMX_RS232_INTERFACE_UP9600:
      return 9600;
    default:
      return 38400;
  }
}

unsigned ClampRs232Baud(unsigned baud, TBmxRs232Interface interface) {
  if (baud != 300 && baud != 1200 && baud != 2400 && baud != 4800 &&
      baud != 9600 && baud != 19200 && baud != 38400) {
    baud = 2400;
  }
  if (baud > MaxRs232Baud(interface)) {
    baud = MaxRs232Baud(interface);
  }
  return baud;
}

bool SupportsUserportRs232(MachineId id) {
  return id == MachineId::C64 || id == MachineId::SCPU64 ||
         id == MachineId::C128 ||
         id == MachineId::VIC20;
}

bool SupportsAciaRs232(MachineId id) {
  return id == MachineId::C64 || id == MachineId::SCPU64 ||
         id == MachineId::C128 ||
         id == MachineId::VIC20 || id == MachineId::PLUS4;
}

}  // namespace

void RunMainProgram(const char *timing_option,
                    ViceOptions *options,
                    const MachineDescriptor &machine) {
  char *argv[64];
  int argc = 0;
  char rs232_baud[12];

  argv[argc++] = (char *)"vice";
  argv[argc++] = (char *)timing_option;
  argv[argc++] = (char *)"-sounddev";
  argv[argc++] = (char *)"raspi";
  rs232net_set_hayes_mode(options->GetRs232NetMode() == BMX_RS232_MODE_HAYES);
  rs232net_set_hayes_audio_mode(
      options->GetRs232NetMode() == BMX_RS232_MODE_HAYES
          ? options->GetHayesAudio()
          : BMX_HAYES_AUDIO_OFF);
  rs232net_load_phonebook(options->GetRs232NetMode() == BMX_RS232_MODE_HAYES
                              ? options->GetRs232NetPhonebook()
                              : nullptr);
  if (machine.default_keymap != nullptr && !HasSavedViceSettings()) {
    argv[argc++] = (char *)"-keymap";
    argv[argc++] = (char *)"3";
    argv[argc++] = (char *)"-poskeymap";
    argv[argc++] = (char *)Basename(machine.default_keymap);
  }

  if (machine.id == MachineId::SCPU64) {
    argv[argc++] = (char *)"+drive8truedrive";
    argv[argc++] = (char *)"+drive9truedrive";
    argv[argc++] = (char *)"+drive10truedrive";
    argv[argc++] = (char *)"+drive11truedrive";
    argv[argc++] = (char *)"-trapdevice8";
    argv[argc++] = (char *)"-trapdevice9";
    argv[argc++] = (char *)"-trapdevice10";
    argv[argc++] = (char *)"-trapdevice11";
  }

  bool hayes_mode = options->GetRs232NetMode() == BMX_RS232_MODE_HAYES;
  bool raw_tcp_mode = options->GetRs232NetMode() == BMX_RS232_MODE_RAW_TCP;
  if (options->Rs232NetEnabled() &&
      (hayes_mode ||
       (raw_tcp_mode && options->GetRs232NetTarget()[0] != '\0'))) {
    TBmxRs232Interface interface = options->GetRs232NetInterface();
    const char *rs232_target =
        hayes_mode ? ":0" : options->GetRs232NetTarget();
    bool wants_userport = interface == BMX_RS232_INTERFACE_USERPORT ||
                          interface == BMX_RS232_INTERFACE_UP9600;
    unsigned rs232_baud_value =
        ClampRs232Baud(options->GetRs232NetBaud(), interface);
    std::snprintf(rs232_baud, sizeof rs232_baud, "%u",
                  rs232_baud_value);

    argv[argc++] = (char *)"-rsdev1";
    argv[argc++] = (char *)rs232_target;
    argv[argc++] = (char *)(!hayes_mode && options->Rs232NetIP232Enabled()
                                ? "-rsdev1ip232"
                                : "+rsdev1ip232");

    if (wants_userport && SupportsUserportRs232(machine.id)) {
      argv[argc++] = (char *)"-userportdevice";
      argv[argc++] = (char *)"modem";
      argv[argc++] = (char *)"-rsuserdev";
      argv[argc++] = (char *)"0";
      argv[argc++] = (char *)"-rsuserbaud";
      argv[argc++] = rs232_baud;
      argv[argc++] = (char *)(interface == BMX_RS232_INTERFACE_UP9600
                                  ? "-rsuserup9600"
                                  : "+rsuserup9600");
      BMC64_RS232_EVENT("boot mode %s target %s baud %s ip232 %s device %s",
                        hayes_mode ? "hayes" : "raw", rs232_target,
                        rs232_baud,
                        !hayes_mode && options->Rs232NetIP232Enabled()
                            ? "on"
                            : "off",
                        Rs232InterfaceKey(interface));
      BMC64_RS232_DEBUG("boot userport RsDevice1=%s RsDevice1ip232=%d "
                        "UserportDevice=modem RsUserDev=0 RsUserBaud=%s "
                        "RsUserUP9600=%d",
                        rs232_target,
                        !hayes_mode && options->Rs232NetIP232Enabled() ? 1 : 0,
                        rs232_baud,
                        interface == BMX_RS232_INTERFACE_UP9600 ? 1 : 0);
    } else if (!wants_userport && SupportsAciaRs232(machine.id)) {
      argv[argc++] = (char *)"-myaciadev";
      argv[argc++] = (char *)"0";
      if (machine.id == MachineId::C64 || machine.id == MachineId::SCPU64 ||
          machine.id == MachineId::C128 ||
          machine.id == MachineId::VIC20) {
        argv[argc++] = (char *)"-acia1base";
        argv[argc++] = (char *)Rs232AciaBase(interface);
        argv[argc++] = (char *)"-acia1mode";
        argv[argc++] = (char *)"1";
        argv[argc++] = (char *)"-acia1irq";
        argv[argc++] = (char *)"1";
        argv[argc++] = (char *)"-acia1";
      } else {
        argv[argc++] = (char *)"-acia";
      }
      BMC64_RS232_EVENT("boot mode %s target %s ip232 %s device %s",
                        hayes_mode ? "hayes" : "raw", rs232_target,
                        !hayes_mode && options->Rs232NetIP232Enabled()
                            ? "on"
                            : "off",
                        machine.id == MachineId::PLUS4
                            ? "plus4-acia"
                            : Rs232InterfaceKey(interface));
      if (machine.id == MachineId::PLUS4) {
        BMC64_RS232_DEBUG("boot plus4 acia RsDevice1=%s RsDevice1ip232=%d "
                          "Acia1Dev=0 Acia1Enable=1",
                          rs232_target,
                          !hayes_mode && options->Rs232NetIP232Enabled() ? 1
                                                                         : 0);
      } else {
        BMC64_RS232_DEBUG("boot acia RsDevice1=%s RsDevice1ip232=%d "
                          "Acia1Dev=0 Acia1Base=%s Acia1Mode=1 Acia1Irq=1 "
                          "Acia1Enable=1",
                          rs232_target,
                          !hayes_mode && options->Rs232NetIP232Enabled() ? 1
                                                                         : 0,
                          Rs232AciaBase(interface));
      }
    } else {
      BMC64_RS232_EVENT("boot interface %s unsupported on %s",
                        Rs232InterfaceKey(interface), machine.display_name);
    }
  }

#ifndef BMC64_USE_VICE_310_ARGS
  if (machine.legacy_sound_output_arg) {
    argv[argc++] = (char *)"-soundoutput";
    argv[argc++] = (char *)"1";
  }
  argv[argc++] = (char *)"-soundsync";
  argv[argc++] = (char *)"0";
  argv[argc++] = (char *)"-refresh";
  argv[argc++] = (char *)"1";
  if (machine.legacy_video_cache_arg_0 != nullptr) {
    argv[argc++] = (char *)machine.legacy_video_cache_arg_0;
  }
  if (machine.legacy_video_cache_arg_1 != nullptr) {
    argv[argc++] = (char *)machine.legacy_video_cache_arg_1;
  }
#endif

  emu_machine_init(options->GetRasterSkip(), options->GetRasterSkip2());
  main_program(argc, argv);
  emu_exit();
}

}  // namespace bmc64::vice

extern "C" int emux_developer_mode_enabled(void) {
  ViceOptions *options = ViceOptions::Get();
  return options != nullptr && options->DeveloperModeEnabled() ? 1 : 0;
}

extern "C" int emux_get_developer_password(char *password,
                                             unsigned password_size) {
  ViceOptions *options = ViceOptions::Get();
  if (password == nullptr || password_size == 0 || options == nullptr) {
    return 0;
  }
  const char *configured = options->GetDeveloperPassword();
  const size_t size = std::strlen(configured);
  if (size >= password_size) {
    password[0] = '\0';
    return 0;
  }
  std::memcpy(password, configured, size + 1);
  return 1;
}

extern "C" unsigned emux_get_developer_log_buffer_kb(void) {
  ViceOptions *options = ViceOptions::Get();
  return options != nullptr ? options->GetDeveloperLogBufferKB()
                            : BMX_DEVELOPER_LOG_BUFFER_DEFAULT_KB;
}
