#include "ets_sys.h"
#include "osapi.h"
#include "gpio.h"
#include "os_type.h"
#include "user_config.h"
#include "v7_periph.h"
#include "v7_gpio.h"

/* These declarations are missing in SDK headers since ~1.0 */
#define PERIPHS_IO_MUX_PULLDWN BIT6
#define PIN_PULLDWN_DIS(PIN_NAME) \
  CLEAR_PERI_REG_MASK(PIN_NAME, PERIPHS_IO_MUX_PULLDWN)
#define PIN_PULLDWN_EN(PIN_NAME) \
  SET_PERI_REG_MASK(PIN_NAME, PERIPHS_IO_MUX_PULLDWN)

#define GPIO_FLOAT 0
#define GPIO_PULLUP 1
#define GPIO_PULLDOWN 2

#define GPIO_INOUT 0
#define GPIO_INPUT 1
#define GPIO_OUTPUT 2
#define GPIO_INT 3

#define GPIO_HIGH 1
#define GPIO_LOW 0

#define GPIO_PIN_COUNT 16

static GPIO_INT_TYPE int_map[GPIO_PIN_COUNT] = {0};
#define GPIO_TASK_QUEUE_LEN 25
static os_event_t gpio_task_queue[GPIO_TASK_QUEUE_LEN];
#define GPIO_TASK_SIG 0x123
/* TODO(alashkin): introduce some kind of tasks priority registry */
#define TASK_PRIORITY 1

ICACHE_FLASH_ATTR static void gpio16_set_output_mode() {
  WRITE_PERI_REG(
      PAD_XPD_DCDC_CONF,
      (READ_PERI_REG(PAD_XPD_DCDC_CONF) & 0xffffffbc) | (uint32_t) 0x1);

  WRITE_PERI_REG(
      RTC_GPIO_CONF,
      (READ_PERI_REG(RTC_GPIO_CONF) & (uint32_t) 0xfffffffe) | (uint32_t) 0x0);

  WRITE_PERI_REG(RTC_GPIO_ENABLE,
                 (READ_PERI_REG(RTC_GPIO_ENABLE) & (uint32_t) 0xfffffffe) |
                     (uint32_t) 0x1);
}

ICACHE_FLASH_ATTR static void gpio16_output_set(uint8_t value) {
  WRITE_PERI_REG(RTC_GPIO_OUT,
                 (READ_PERI_REG(RTC_GPIO_OUT) & (uint32_t) 0xfffffffe) |
                     (uint32_t)(value & 1));
}

ICACHE_FLASH_ATTR static void gpio16_set_input_mode() {
  WRITE_PERI_REG(
      PAD_XPD_DCDC_CONF,
      (READ_PERI_REG(PAD_XPD_DCDC_CONF) & 0xffffffbc) | (uint32_t) 0x1);

  WRITE_PERI_REG(
      RTC_GPIO_CONF,
      (READ_PERI_REG(RTC_GPIO_CONF) & (uint32_t) 0xfffffffe) | (uint32_t) 0x0);

  WRITE_PERI_REG(RTC_GPIO_ENABLE,
                 READ_PERI_REG(RTC_GPIO_ENABLE) & (uint32_t) 0xfffffffe);
}

ICACHE_FLASH_ATTR static uint8_t gpio16_input_get() {
  return (uint8_t)(READ_PERI_REG(RTC_GPIO_IN_DATA) & 1);
}

ICACHE_FLASH_ATTR int v7_gpio_set_mode(int pin, int mode, int pull) {
  struct gpio_info* gi;

  if (pin == 16) {
    if (mode == GPIO_INPUT) {
      gpio16_set_input_mode();
    } else {
      gpio16_set_output_mode();
    }
    return 0;
  }

  gi = get_gpio_info(pin);

  if (gi == NULL) {
    return -1;
  }

  switch (pull) {
    case GPIO_PULLUP:
      PIN_PULLDWN_DIS(gi->periph);
      PIN_PULLUP_EN(gi->periph);
      break;
    case GPIO_PULLDOWN:
      PIN_PULLUP_DIS(gi->periph);
      PIN_PULLDWN_EN(gi->periph);
      break;
    case GPIO_FLOAT:
      PIN_PULLUP_DIS(gi->periph);
      PIN_PULLDWN_DIS(gi->periph);
      break;
    default:
      return -1;
  }

  switch (mode) {
    case GPIO_INOUT:
      PIN_FUNC_SELECT(gi->periph, gi->func);
      break;

    case GPIO_INPUT:
      PIN_FUNC_SELECT(gi->periph, gi->func);
      GPIO_DIS_OUTPUT(pin);
      break;

    case GPIO_OUTPUT:
      ETS_GPIO_INTR_DISABLE();
      PIN_FUNC_SELECT(gi->periph, gi->func);

      gpio_pin_intr_state_set(GPIO_ID_PIN(pin), GPIO_PIN_INTR_DISABLE);

      GPIO_REG_WRITE(GPIO_STATUS_W1TC_ADDRESS, BIT(pin));
      GPIO_REG_WRITE(GPIO_PIN_ADDR(GPIO_ID_PIN(pin)),
                     GPIO_REG_READ(GPIO_PIN_ADDR(GPIO_ID_PIN(pin))) &
                         (~GPIO_PIN_PAD_DRIVER_SET(GPIO_PAD_DRIVER_ENABLE)));
      ETS_GPIO_INTR_ENABLE();
      break;

    case GPIO_INT:
      ETS_GPIO_INTR_DISABLE();
      PIN_FUNC_SELECT(gi->periph, gi->func);
      GPIO_DIS_OUTPUT(pin);

      gpio_register_set(GPIO_PIN_ADDR(GPIO_ID_PIN(pin)),
                        GPIO_PIN_INT_TYPE_SET(GPIO_PIN_INTR_DISABLE) |
                            GPIO_PIN_PAD_DRIVER_SET(GPIO_PAD_DRIVER_DISABLE) |
                            GPIO_PIN_SOURCE_SET(GPIO_AS_PIN_SOURCE));
      ETS_GPIO_INTR_ENABLE();
      break;

    default:
      return -1;
  }

  return 0;
}

ICACHE_FLASH_ATTR int v7_gpio_write(int pin, int level) {
  if (get_gpio_info(pin) == NULL) {
    /* Just verifying pin number */
    return -1;
  }

  level &= 0x1;

  if (pin == 16) {
    gpio16_output_set(level);
    return 0;
  }

  GPIO_OUTPUT_SET(GPIO_ID_PIN(pin), level);
  return 0;
}

ICACHE_FLASH_ATTR int v7_gpio_read(int pin) {
  if (get_gpio_info(pin) == NULL) {
    /* Just verifying pin number */
    return -1;
  }

  if (pin == 16) {
    return 0x1 & gpio16_input_get();
  }

  return 0x1 & GPIO_INPUT_GET(GPIO_ID_PIN(pin));
}

ICACHE_FLASH_ATTR static void v7_gpio_intr_dispatcher(
    f_gpio_intr_handler_t callback) {
  uint8_t i, level;

  uint32_t gpio_status = GPIO_REG_READ(GPIO_STATUS_ADDRESS);

  for (i = 0; i < GPIO_PIN_COUNT; i++) {
    if (int_map[i] && (gpio_status & BIT(i))) {
      gpio_pin_intr_state_set(GPIO_ID_PIN(i), GPIO_PIN_INTR_DISABLE);
      GPIO_REG_WRITE(GPIO_STATUS_W1TC_ADDRESS, gpio_status & BIT(i));
      level = 0x1 & GPIO_INPUT_GET(GPIO_ID_PIN(i));

      system_os_post(TASK_PRIORITY,
                     (uint32_t) GPIO_TASK_SIG << 16 | i << 8 | level, callback);

      gpio_pin_intr_state_set(GPIO_ID_PIN(i), int_map[i]);
    }
  }
}

ICACHE_FLASH_ATTR void v7_gpio_task(os_event_t* event) {
  if (event->sig >> 16 != GPIO_TASK_SIG) {
    return;
  }

  ((f_gpio_intr_handler_t) event->par)((event->sig & 0xFFFF) >> 8,
                                       (event->sig & 0xFF));
}

ICACHE_FLASH_ATTR void v7_gpio_intr_init(f_gpio_intr_handler_t cb) {
  system_os_task(v7_gpio_task, TASK_PRIORITY, gpio_task_queue,
                 GPIO_TASK_QUEUE_LEN);
  ETS_GPIO_INTR_ATTACH(v7_gpio_intr_dispatcher, cb);
}

ICACHE_FLASH_ATTR int v7_gpio_intr_set(int pin, GPIO_INT_TYPE type) {
  if (get_gpio_info(pin) == NULL) {
    return -1;
  }
  ETS_GPIO_INTR_DISABLE();
  GPIO_REG_WRITE(GPIO_STATUS_W1TC_ADDRESS, BIT(pin));

  int_map[pin] = type;

  gpio_pin_intr_state_set(GPIO_ID_PIN(pin), type);
  ETS_GPIO_INTR_ENABLE();

  return 0;
}
