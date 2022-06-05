#include <mgos_homeassistant.h>

#include <mgos-helpers/json.h>
#include <mgos-helpers/log.h>
#include <mgos-helpers/mem.h>
#include <mgos-helpers/tmr.h>

enum cr703_state {      // two-bit value
  CR_ST_TRANSIENT = 0,  // -shut -open
  CR_ST_OPEN,           // -shut +open
  CR_ST_SHUT,           // +shut -open
  CR_ST_INVALID         // +shut +open
};

struct cr703_ha {
  struct mgos_homeassistant_object *o;  // Linked HA object
  mgos_timer_id tmr;                    // Delayed operations

  struct {
    bool invert;
    int open;
    int shut;
  } in;
  struct {
    bool invert;
    int open;
    int power;
  } out;

  struct {
    uint8_t now;  // enum cr703_state value
    uint8_t tgt;  // Ditto
  } st;
};

static bool cr_is_303(const struct cr703_ha *cr) {
  return cr->in.open < 0;
}

static bool cr_st_is_good(const struct cr703_ha *cr) {
  return cr->st.now == CR_ST_OPEN || cr->st.now == CR_ST_SHUT;
}

static void cr_st_set_tmr(void *opaque) {
  struct cr703_ha *cr = opaque;
  cr->tmr = MGOS_INVALID_TIMER_ID;
  mgos_gpio_write(cr->out.power, cr->out.invert);
  mgos_gpio_write(cr->out.open, cr->out.invert);
  if (cr_is_303(cr)) cr->st.now = cr->st.tgt;  // Presume successful switching
  if (cr->o) mgos_homeassistant_object_send_status(cr->o);
}

static void cr_st_set(struct cr703_ha *cr, enum cr703_state tgt) {
  if (!MGOS_TMR_RESET(cr->tmr,
                      mgos_sys_config_get_cr703_ha_max_switch_sec() * 1000, 0,
                      cr_st_set_tmr, cr))
    FNERR_RET(, CALL_FAILED(mgos_set_timer));
  mgos_gpio_write(cr->out.open, cr->out.invert ^ (tgt == CR_ST_OPEN));
  mgos_gpio_write(cr->out.power, !cr->out.invert);
  cr->st.tgt = tgt;
}

static void cr_cmd(struct mgos_homeassistant_object *o, const char *s,
                   const int sz) {
  if (!o) return;
  struct cr703_ha *cr = o->user_data;
  if (sz == 2 && !strncasecmp(s, "ON", sz))
    cr_st_set(cr, CR_ST_OPEN);
  else if (sz == 3 && !strncasecmp(s, "OFF", sz))
    cr_st_set(cr, CR_ST_SHUT);
}

static void cr_stat(struct mgos_homeassistant_object *o, struct json_out *out) {
  const struct cr703_ha *cr = o->user_data;
  if (cr_st_is_good(cr))
    json_printf(out, "state:%Q", ON_OFF(cr->st.now == CR_ST_OPEN));
  else
    json_printf(out, "open:%B,shut:%B,state:%Q", cr->st.now & CR_ST_OPEN,
                cr->st.now & CR_ST_SHUT, NULL);
}

static void cr_int(int pin, void *opaque) {
  struct cr703_ha *cr = opaque;
  uint8_t bit = pin == cr->in.open ? CR_ST_OPEN : CR_ST_SHUT;
  if (!cr->in.invert ^ !mgos_gpio_read(pin))
    cr->st.now |= bit;
  else
    cr->st.now &= ~bit;
  if (cr->st.now == cr->st.tgt && cr->tmr) {
    mgos_clear_timer(cr->tmr);
    cr_st_set_tmr(cr);
  }
}

static bool cr_obj_setup_in(struct cr703_ha *cr) {
  if (cr_is_303(cr)) return true;

  enum mgos_gpio_pull_type pull =
      cr->in.invert ? MGOS_GPIO_PULL_UP : MGOS_GPIO_PULL_DOWN;
  TRY_GT(mgos_gpio_set_button_handler, cr->in.open, pull,
         MGOS_GPIO_INT_EDGE_ANY, 50, cr_int, cr);
  TRY_GT(mgos_gpio_set_button_handler, cr->in.shut, pull,
         MGOS_GPIO_INT_EDGE_ANY, 50, cr_int, cr);
  cr->st.tgt = cr->st.now =
      (!cr->in.invert ^ !mgos_gpio_read(cr->in.open) ? CR_ST_OPEN : 0) |
      (!cr->in.invert ^ !mgos_gpio_read(cr->in.shut) ? CR_ST_SHUT : 0);
  return true;

err:
  mgos_gpio_set_button_handler(cr->in.open, MGOS_GPIO_PULL_NONE,
                               MGOS_GPIO_INT_EDGE_ANY, 0, NULL, NULL);
  mgos_gpio_set_button_handler(cr->in.shut, MGOS_GPIO_PULL_NONE,
                               MGOS_GPIO_INT_EDGE_ANY, 0, NULL, NULL);
  return false;
}

static bool cr_obj_setup_out(const struct cr703_ha *cr) {
  TRY_RETF(mgos_gpio_setup_output, cr->out.open, cr->out.invert);
  TRY_RETF(mgos_gpio_setup_output, cr->out.power, cr->out.invert);
  return true;
}

static bool cr_obj_setup(struct cr703_ha *cr) {
  return cr_obj_setup_in(cr) && cr_obj_setup_out(cr);
}

#define CONF_FMT                    \
  "{boot_on:%B,name:%Q,"            \
  "in:{invert:%B,open:%d,shut:%d}," \
  "out:{invert:%B,open:%d,power:%d}}"
static bool cr_obj_fromjson(struct mgos_homeassistant *ha,
                            struct json_token v) {
  struct cr703_ha *cr = NULL;
  unsigned boot_on = BOOL_INVAL;
  char *name = NULL;
  bool ok = false;

  cr = TRY_CALLOC_OR(goto err, cr);
  cr->in.open = cr->in.shut = cr->out.open = cr->out.power = -1;

  TRY_JSON_SCANF_OR(goto err, v.ptr, v.len, CONF_FMT, &boot_on, &name,
                    &cr->in.invert, &cr->in.open, &cr->in.shut, &cr->out.invert,
                    &cr->out.open, &cr->out.power);

  if (boot_on == BOOL_INVAL && cr_is_303(cr))
    FNERR_GT("need boot_on %s", "or both in.open+in.shut");
  if ((cr->in.open < 0) ^ (cr->in.shut < 0))
    FNERR_GT("need neither %s", "or both in.open+in.shut");
  if (cr->out.open < 0 || cr->out.power < 0)
    FNERR_GT("need out.open+out.power");

  TRY_GT(cr_obj_setup, cr);

  cr->o = mgos_homeassistant_object_add(
      ha,
      name
          ?: cr_is_303(cr) ? "cr303"
                           : "cr703",
      COMPONENT_SWITCH,
      "\"ic\":\"hass:valve\",\"val_tpl\":\"{{value_json.state}}\"", cr_stat,
      cr);
  if (!cr->o) FNERR_GT(CALL_FAILED("mgos_homeassistant_object_add"));
  TRY_GT(mgos_homeassistant_object_add_cmd_cb, cr->o, NULL, cr_cmd);
  cr->o->config_sent = false;
  if (boot_on != BOOL_INVAL) cr_st_set(cr, boot_on ? CR_ST_OPEN : CR_ST_SHUT);
  if (boot_on == BOOL_INVAL && !cr_st_is_good(cr)) cr_st_set(cr, CR_ST_OPEN);
  ok = true;

err:
  if (name) free(name);
  if (!ok && cr && cr->o) mgos_homeassistant_object_remove(&cr->o);
  if (!ok && cr) free(cr);
  return ok;
}

bool mgos_cr703_ha_init(void) {
  if (!mgos_sys_config_get_cr703_ha_enable()) return true;
  TRY_RETF(mgos_homeassistant_register_provider, "cr703", cr_obj_fromjson,
           NULL);
  return true;
}
