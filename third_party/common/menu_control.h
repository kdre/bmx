#ifndef BMX_MENU_CONTROL_H
#define BMX_MENU_CONTROL_H

#include <stddef.h>

#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MENU_CONTROL_KEY_SIZE 64
#define MENU_CONTROL_PAGE_MAX 16
/* Use only when the stable menu ID uniquely identifies an item and sub_id is
   private callback data rather than part of the public binding identity. */
#define MENU_CONTROL_SUB_ID_ANY (-1)

typedef enum menu_control_status {
  MENU_CONTROL_OK = 0,
  MENU_CONTROL_NOT_FOUND,
  MENU_CONTROL_HIDDEN,
  MENU_CONTROL_DISABLED,
  MENU_CONTROL_WRONG_TYPE,
  MENU_CONTROL_INVALID_VALUE,
  MENU_CONTROL_UNAVAILABLE,
} menu_control_status;

typedef enum menu_control_value_kind {
  MENU_CONTROL_VALUE_NONE = 0,
  MENU_CONTROL_VALUE_BOOL,
  MENU_CONTROL_VALUE_INTEGER,
  MENU_CONTROL_VALUE_STRING,
} menu_control_value_kind;

typedef enum menu_control_action_argument {
  MENU_CONTROL_ACTION_INVALID = -1,
  MENU_CONTROL_ACTION_NONE = 0,
  MENU_CONTROL_ACTION_MEDIA_PATH,
} menu_control_action_argument;

typedef enum menu_control_public_binding_kind {
  MENU_CONTROL_PUBLIC_CONTROL = 0,
  MENU_CONTROL_PUBLIC_ACTION,
} menu_control_public_binding_kind;

/* The owning emulator registers one static table during startup. The table
   maps stable REST keys to its private live menu items without exposing those
   item identifiers as part of the REST contract. */
struct menu_control_public_binding {
  const char *key;
  int id;
  int sub_id;
  menu_control_public_binding_kind kind;
  menu_control_action_argument argument;
};

struct menu_control_value {
  menu_control_value_kind kind;
  int integer;
  char string[MAX_STR_VAL_LEN];
};

struct menu_control_choice {
  char label[MAX_MENU_STR];
  int value;
  int disabled;
};

struct menu_control_description {
  char key[MENU_CONTROL_KEY_SIZE];
  char name[MAX_MENU_STR];
  int id;
  int sub_id;
  menu_item_type type;
  int hidden;
  int disabled;
  int redacted;
  int min;
  int max;
  int step;
  int divisor;
  int choice_count;
  struct menu_control_choice choices[MAX_CHOICES];
  struct menu_control_value value;
};

struct menu_control_summary {
  char key[MENU_CONTROL_KEY_SIZE];
  char name[MAX_MENU_STR];
  menu_item_type type;
  int hidden;
  int disabled;
  int redacted;
  struct menu_control_value value;
};

struct menu_control_page {
  struct menu_control_summary controls[MENU_CONTROL_PAGE_MAX];
  size_t count;
  int has_more;
};

/* Public REST facade. These stable names deliberately expose only a small
   emulator-registered allowlist; item identifiers remain an internal
   implementation detail. The registered table must have static lifetime. */
int menu_control_public_set_bindings(
    const struct menu_control_public_binding *bindings, size_t count);
menu_control_status menu_control_public_describe(
    const char *key, struct menu_control_description *description);
menu_control_status menu_control_public_set(
    const char *key, const struct menu_control_value *value,
    struct menu_control_description *description);
menu_control_status menu_control_public_describe_action(
    const char *key, struct menu_control_description *description);
menu_control_status menu_control_public_invoke(
    const char *key, struct menu_control_description *description);
menu_control_status menu_control_public_list(
    const char *after, size_t limit, struct menu_control_page *page);
menu_control_status menu_control_public_list_actions(
    const char *after, size_t limit, struct menu_control_page *page);
menu_control_action_argument menu_control_public_action_argument(
    const char *key);

const char *menu_control_status_name(menu_control_status status);
const char *menu_control_type_name(menu_item_type type);

#ifdef __cplusplus
}
#endif

#endif
