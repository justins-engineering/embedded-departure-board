#include "light_sensor.h"

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(light_sensor, LOG_LEVEL_INF);

#define SEN_ADDR 0x23
#define SEN_REG_LUX 0x10

const struct device *i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));

int get_lux(void) {
  uint8_t lux[2] = {0, 0};
  int ret;

  if (i2c_dev == NULL || !device_is_ready(i2c_dev)) {
    LOG_ERR("Could not get I2C device");
    return -1;
  }

  ret = i2c_burst_read(i2c_dev, SEN_ADDR, SEN_REG_LUX, &lux[0], 2);
  if (ret) {
    LOG_ERR("Unable get lux data. (err %i)\n", ret);
    return -1;
  }

  ret = ((uint16_t)lux[0] << 8) | (uint16_t)lux[1];
  LOG_INF("LUX: 0x%x", ret);

  return ret;
}