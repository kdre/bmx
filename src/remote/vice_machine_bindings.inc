/* Stable native REST actions for machine-specific VICE menu operations.
 *
 * The includer must define BMX_REMOTE_ACTION_ANY_SUB(key, id).  PET stores the
 * private VICE model number in each button's sub_id, while the stable menu ID
 * already identifies the button uniquely.  Entries whose menu item is absent
 * for the running emulator are omitted from the live API.
 */

BMX_REMOTE_ACTION_ANY_SUB("machine.pet.model.2001nb", MENU_MODEL_PET_2001),
BMX_REMOTE_ACTION_ANY_SUB("machine.pet.model.3008", MENU_MODEL_PET_3008),
BMX_REMOTE_ACTION_ANY_SUB("machine.pet.model.3016", MENU_MODEL_PET_3016),
BMX_REMOTE_ACTION_ANY_SUB("machine.pet.model.3032", MENU_MODEL_PET_3032),
BMX_REMOTE_ACTION_ANY_SUB("machine.pet.model.3032b", MENU_MODEL_PET_3032B),
BMX_REMOTE_ACTION_ANY_SUB("machine.pet.model.4016", MENU_MODEL_PET_4016),
BMX_REMOTE_ACTION_ANY_SUB("machine.pet.model.4032", MENU_MODEL_PET_4032),
BMX_REMOTE_ACTION_ANY_SUB("machine.pet.model.4032b", MENU_MODEL_PET_4032B),
BMX_REMOTE_ACTION_ANY_SUB("machine.pet.model.8032", MENU_MODEL_PET_8032),
BMX_REMOTE_ACTION_ANY_SUB("machine.pet.model.8096", MENU_MODEL_PET_8096),
BMX_REMOTE_ACTION_ANY_SUB("machine.pet.model.8296", MENU_MODEL_PET_8296),
BMX_REMOTE_ACTION_ANY_SUB("machine.pet.model.superpet", MENU_MODEL_PET_SUPERPET),
