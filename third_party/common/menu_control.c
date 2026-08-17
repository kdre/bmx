#include "menu_control.h"

#include <string.h>

#include "menu.h"

static const struct menu_control_public_binding *public_bindings;
static size_t public_binding_count;

static int is_public_key(const char *key) {
  size_t i;
  size_t length;
  if (key == NULL || key[0] == '\0') return 0;
  length = strlen(key);
  if (length >= MENU_CONTROL_KEY_SIZE) return 0;
  for (i = 0; i < length; ++i) {
    const char c = key[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.')) {
      return 0;
    }
  }
  return 1;
}

int menu_control_public_set_bindings(
    const struct menu_control_public_binding *bindings, size_t count) {
  size_t i;
  size_t previous;
  if (bindings == NULL && count != 0) return 0;
  for (i = 0; i < count; ++i) {
    if (!is_public_key(bindings[i].key) ||
        (bindings[i].sub_id < 0 &&
         bindings[i].sub_id != MENU_CONTROL_SUB_ID_ANY) ||
        (bindings[i].kind != MENU_CONTROL_PUBLIC_CONTROL &&
         bindings[i].kind != MENU_CONTROL_PUBLIC_ACTION) ||
        (bindings[i].kind == MENU_CONTROL_PUBLIC_CONTROL &&
         bindings[i].argument != MENU_CONTROL_ACTION_NONE) ||
        (bindings[i].argument != MENU_CONTROL_ACTION_NONE &&
         bindings[i].argument != MENU_CONTROL_ACTION_MEDIA_PATH)) {
      return 0;
    }
    for (previous = 0; previous < i; ++previous) {
      if (strcmp(bindings[previous].key, bindings[i].key) == 0) return 0;
    }
  }
  public_bindings = bindings;
  public_binding_count = count;
  return 1;
}

static const struct menu_control_public_binding *find_public_binding(
    const char *key, menu_control_public_binding_kind kind) {
  size_t i;
  if (key == NULL || key[0] == '\0') return NULL;
  for (i = 0; i < public_binding_count; ++i) {
    if (public_bindings[i].kind == kind &&
        strcmp(public_bindings[i].key, key) == 0) {
      return &public_bindings[i];
    }
  }
  return NULL;
}

static int is_functional_item(const struct menu_item *item) {
  return item != NULL && item->id != MENU_ID_DO_NOTHING &&
         item->id != MENU_TEXT &&
         item->type != FOLDER && item->type != DIVIDER;
}

static int is_secret(int id) {
  return id == MENU_NETWORK_WIFI_PSK ||
         id == MENU_SYSTEM_DEVELOPER_PASSWORD ||
         id == MENU_SYSTEM_API_PASSWORD;
}

static struct menu_item *find_item_in(struct menu_item *node, int id,
                                      int sub_id) {
  while (node != NULL) {
    if (node->id == id &&
        (sub_id == MENU_CONTROL_SUB_ID_ANY || node->sub_id == sub_id) &&
        is_functional_item(node)) {
      return node;
    }
    if (node->type == FOLDER) {
      struct menu_item *found = find_item_in(node->first_child, id, sub_id);
      if (found != NULL) return found;
    }
    node = node->next;
  }
  return NULL;
}

static struct menu_item *find_item_by_id(int id, int sub_id) {
  struct menu_item *root = ui_menu_root();
  return root != NULL ? find_item_in(root->first_child, id, sub_id) : NULL;
}

static void copy_text(char *output, size_t output_size, const char *input) {
  if (output == NULL || output_size == 0) return;
  if (input == NULL) input = "";
  strncpy(output, input, output_size - 1);
  output[output_size - 1] = '\0';
}

static void read_value(const struct menu_item *item,
                       struct menu_control_value *value, int redacted) {
  memset(value, 0, sizeof(*value));
  if (redacted) {
    value->kind = MENU_CONTROL_VALUE_STRING;
    return;
  }
  switch (item->type) {
    case TOGGLE:
    case CHECKBOX:
      value->kind = MENU_CONTROL_VALUE_BOOL;
      value->integer = item->value != 0;
      break;
    case MULTIPLE_CHOICE:
    case RANGE:
      value->kind = MENU_CONTROL_VALUE_INTEGER;
      value->integer = item->value;
      break;
    case TEXTFIELD:
      value->kind = MENU_CONTROL_VALUE_STRING;
      copy_text(value->string, sizeof(value->string), item->str_value);
      break;
    default:
      value->kind = MENU_CONTROL_VALUE_NONE;
      break;
  }
}

static void describe_item(const struct menu_item *item,
                          struct menu_control_description *description) {
  int i;
  memset(description, 0, sizeof(*description));
  copy_text(description->name, sizeof(description->name), item->name);
  description->id = item->id;
  description->sub_id = item->sub_id;
  description->type = item->type;
  description->hidden = item->hidden != 0;
  description->disabled = item->disabled != 0;
  description->redacted = is_secret(item->id);
  description->min = item->min;
  description->max = item->max;
  description->step = item->step;
  description->divisor = item->divisor;
  description->choice_count = item->type == MULTIPLE_CHOICE
                                  ? item->num_choices
                                  : 0;
  if (description->choice_count < 0) description->choice_count = 0;
  if (description->choice_count > MAX_CHOICES) {
    description->choice_count = MAX_CHOICES;
  }
  for (i = 0; i < description->choice_count; ++i) {
    copy_text(description->choices[i].label,
              sizeof(description->choices[i].label), item->choices[i]);
    description->choices[i].value = item->choice_ints[i];
    description->choices[i].disabled = item->choice_disabled[i] != 0;
  }
  read_value(item, &description->value, description->redacted);
}

static menu_control_status set_item(
    struct menu_item *item, const struct menu_control_value *value,
    struct menu_control_description *description) {
  size_t length;
  int maximum;
  if (item == NULL) return MENU_CONTROL_NOT_FOUND;
  if (item->hidden) return MENU_CONTROL_HIDDEN;
  if (item->disabled) return MENU_CONTROL_DISABLED;
  if (value == NULL || description == NULL) return MENU_CONTROL_INVALID_VALUE;

  switch (item->type) {
    case TOGGLE:
    case CHECKBOX:
      if (value->kind != MENU_CONTROL_VALUE_BOOL ||
          (value->integer != 0 && value->integer != 1)) {
        return MENU_CONTROL_INVALID_VALUE;
      }
      item->value = value->integer;
      break;
    case RANGE:
      if (value->kind != MENU_CONTROL_VALUE_INTEGER ||
          value->integer < item->min || value->integer > item->max ||
          (item->step > 0 && (value->integer - item->min) % item->step != 0)) {
        return MENU_CONTROL_INVALID_VALUE;
      }
      item->value = value->integer;
      break;
    case MULTIPLE_CHOICE:
      if (value->kind != MENU_CONTROL_VALUE_INTEGER || value->integer < 0 ||
          value->integer >= item->num_choices ||
          item->choice_disabled[value->integer]) {
        return MENU_CONTROL_INVALID_VALUE;
      }
      item->value = value->integer;
      break;
    case TEXTFIELD:
      if (value->kind != MENU_CONTROL_VALUE_STRING) {
        return MENU_CONTROL_INVALID_VALUE;
      }
      length = strlen(value->string);
      maximum = item->max_text_len > 0 ? item->max_text_len : MAX_FN_NAME;
      if (length > (size_t)maximum || length >= sizeof(item->str_value)) {
        return MENU_CONTROL_INVALID_VALUE;
      }
      memcpy(item->str_value, value->string, length + 1);
      item->value = (int)length;
      break;
    default:
      return MENU_CONTROL_WRONG_TYPE;
  }

  ui_menu_commit(item);
  describe_item(item, description);
  return MENU_CONTROL_OK;
}

static menu_control_status invoke_item(
    struct menu_item *item, struct menu_control_description *description) {
  if (item == NULL) return MENU_CONTROL_NOT_FOUND;
  if (item->hidden) return MENU_CONTROL_HIDDEN;
  if (item->disabled) return MENU_CONTROL_DISABLED;
  if (item->type != BUTTON) return MENU_CONTROL_WRONG_TYPE;
  ui_menu_commit(item);
  if (description != NULL) describe_item(item, description);
  return MENU_CONTROL_OK;
}

static menu_control_status public_describe_binding(
    const struct menu_control_public_binding *binding,
    struct menu_control_description *description) {
  if (binding == NULL) return MENU_CONTROL_NOT_FOUND;
  if (description == NULL) return MENU_CONTROL_UNAVAILABLE;
  {
    struct menu_item *item = find_item_by_id(binding->id, binding->sub_id);
    if (item == NULL) return MENU_CONTROL_NOT_FOUND;
    describe_item(item, description);
    copy_text(description->key, sizeof(description->key), binding->key);
    return MENU_CONTROL_OK;
  }
}

menu_control_status menu_control_public_describe(
    const char *key, struct menu_control_description *description) {
  return public_describe_binding(
      find_public_binding(key, MENU_CONTROL_PUBLIC_CONTROL), description);
}

menu_control_status menu_control_public_set(
    const char *key, const struct menu_control_value *value,
    struct menu_control_description *description) {
  const struct menu_control_public_binding *binding =
      find_public_binding(key, MENU_CONTROL_PUBLIC_CONTROL);
  menu_control_status status;
  if (binding == NULL) return MENU_CONTROL_NOT_FOUND;
  status = set_item(find_item_by_id(binding->id, binding->sub_id), value,
                    description);
  if (status == MENU_CONTROL_OK) {
    copy_text(description->key, sizeof(description->key), binding->key);
  }
  return status;
}

menu_control_status menu_control_public_describe_action(
    const char *key, struct menu_control_description *description) {
  return public_describe_binding(
      find_public_binding(key, MENU_CONTROL_PUBLIC_ACTION), description);
}

menu_control_status menu_control_public_invoke(
    const char *key, struct menu_control_description *description) {
  const struct menu_control_public_binding *binding =
      find_public_binding(key, MENU_CONTROL_PUBLIC_ACTION);
  int ui_was_active;
  menu_control_status status;
  if (binding == NULL) return MENU_CONTROL_NOT_FOUND;
  ui_was_active = emu_is_ui_activated();
  status = invoke_item(find_item_by_id(binding->id, binding->sub_id),
                       description);
  /* Menu button callbacks may close the menu with a toggle. When the same
     callback is invoked headlessly, that toggle would open the root menu. */
  if (!ui_was_active && emu_is_ui_activated()) {
    ui_pop_all_and_toggle();
  }
  if (status == MENU_CONTROL_OK && description != NULL) {
    copy_text(description->key, sizeof(description->key), binding->key);
  }
  return status;
}

static menu_control_status public_list_bindings(
    const char *after, size_t limit, struct menu_control_page *page,
    menu_control_public_binding_kind kind) {
  size_t i;
  int after_seen;
  if (ui_menu_root() == NULL || page == NULL) return MENU_CONTROL_UNAVAILABLE;
  if (limit == 0 || limit > MENU_CONTROL_PAGE_MAX) {
    return MENU_CONTROL_INVALID_VALUE;
  }
  if (after == NULL) after = "";
  memset(page, 0, sizeof(*page));
  after_seen = after[0] == '\0';
  for (i = 0; i < public_binding_count; ++i) {
    struct menu_control_description description;
    struct menu_control_summary *summary;
    menu_control_status status;
    if (public_bindings[i].kind != kind) continue;
    if (!after_seen) {
      if (strcmp(public_bindings[i].key, after) == 0) after_seen = 1;
      continue;
    }
    status = public_describe_binding(&public_bindings[i], &description);
    if (status == MENU_CONTROL_NOT_FOUND) continue;
    if (status != MENU_CONTROL_OK) return status;
    if (page->count >= limit) {
      page->has_more = 1;
      break;
    }
    summary = &page->controls[page->count++];
    memset(summary, 0, sizeof(*summary));
    copy_text(summary->key, sizeof(summary->key), description.key);
    copy_text(summary->name, sizeof(summary->name), description.name);
    summary->type = description.type;
    summary->hidden = description.hidden;
    summary->disabled = description.disabled;
    summary->redacted = description.redacted;
    summary->value = description.value;
  }
  if (!after_seen) return MENU_CONTROL_NOT_FOUND;
  return MENU_CONTROL_OK;
}

menu_control_status menu_control_public_list(
    const char *after, size_t limit, struct menu_control_page *page) {
  return public_list_bindings(after, limit, page,
                              MENU_CONTROL_PUBLIC_CONTROL);
}

menu_control_status menu_control_public_list_actions(
    const char *after, size_t limit, struct menu_control_page *page) {
  return public_list_bindings(after, limit, page,
                              MENU_CONTROL_PUBLIC_ACTION);
}

menu_control_action_argument menu_control_public_action_argument(
    const char *key) {
  const struct menu_control_public_binding *binding =
      find_public_binding(key, MENU_CONTROL_PUBLIC_ACTION);
  return binding != NULL ? binding->argument : MENU_CONTROL_ACTION_INVALID;
}

const char *menu_control_status_name(menu_control_status status) {
  switch (status) {
    case MENU_CONTROL_OK: return "ok";
    case MENU_CONTROL_NOT_FOUND: return "not_found";
    case MENU_CONTROL_HIDDEN: return "hidden";
    case MENU_CONTROL_DISABLED: return "disabled";
    case MENU_CONTROL_WRONG_TYPE: return "wrong_type";
    case MENU_CONTROL_INVALID_VALUE: return "invalid_value";
    case MENU_CONTROL_UNAVAILABLE: return "unavailable";
  }
  return "unavailable";
}

const char *menu_control_type_name(menu_item_type type) {
  switch (type) {
    case TOGGLE: return "toggle";
    case CHECKBOX: return "checkbox";
    case MULTIPLE_CHOICE: return "choice";
    case BUTTON: return "button";
    case RANGE: return "range";
    case FOLDER: return "folder";
    case DIVIDER: return "divider";
    case TEXTFIELD: return "text";
  }
  return "unknown";
}
