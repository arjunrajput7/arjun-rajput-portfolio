// SPDX-License-Identifier: GPL-2.0
/*
 * bmp280_simple - minimal out-of-tree I2C client driver for the Bosch BMP280
 * temperature / pressure sensor.
 *
 * Demonstrates the Linux platform driver model without pulling in the IIO
 * subsystem: an i2c_driver with probe/remove, an OF match table, and two
 * read-only sysfs attributes that return datasheet-compensated values:
 *
 *   /sys/bus/i2c/devices/<bus>-00<addr>/temperature   milli-degrees Celsius
 *   /sys/bus/i2c/devices/<bus>-00<addr>/pressure      pascals
 *
 * Compensation math is the fixed-point reference implementation from the
 * BMP280 datasheet (rev 1.19), section 3.11.3 / 8.2.
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>
#include <linux/of.h>

#define BMP280_REG_ID          0xD0
#define BMP280_REG_RESET       0xE0
#define BMP280_REG_CTRL_MEAS   0xF4
#define BMP280_REG_CONFIG      0xF5
#define BMP280_REG_PRESS_MSB   0xF7
#define BMP280_REG_CALIB_START 0x88

#define BMP280_CHIP_ID         0x58
#define BMP280_RESET_MAGIC     0xB6

/* osrs_t x1 (0x20) | osrs_p x1 (0x04) | mode = normal (0x03) */
#define BMP280_CTRL_MEAS_VAL   0x27
/* t_sb = 250ms (0x60) | IIR filter x4 (0x08) */
#define BMP280_CONFIG_VAL      0x68

struct bmp280_calib {
	u16 dig_t1;
	s16 dig_t2, dig_t3;
	u16 dig_p1;
	s16 dig_p2, dig_p3, dig_p4, dig_p5, dig_p6, dig_p7, dig_p8, dig_p9;
};

struct bmp280_data {
	struct i2c_client  *client;
	struct mutex        lock;    /* serialises a measurement + compensation */
	struct bmp280_calib calib;
};

static int bmp280_read_calib(struct bmp280_data *d)
{
	u8 buf[24];
	int ret;

	ret = i2c_smbus_read_i2c_block_data(d->client, BMP280_REG_CALIB_START,
					    sizeof(buf), buf);
	if (ret < 0)
		return ret;
	if (ret != sizeof(buf))
		return -EIO;

	d->calib.dig_t1 = le16_to_cpup((__le16 *)&buf[0]);
	d->calib.dig_t2 = le16_to_cpup((__le16 *)&buf[2]);
	d->calib.dig_t3 = le16_to_cpup((__le16 *)&buf[4]);
	d->calib.dig_p1 = le16_to_cpup((__le16 *)&buf[6]);
	d->calib.dig_p2 = le16_to_cpup((__le16 *)&buf[8]);
	d->calib.dig_p3 = le16_to_cpup((__le16 *)&buf[10]);
	d->calib.dig_p4 = le16_to_cpup((__le16 *)&buf[12]);
	d->calib.dig_p5 = le16_to_cpup((__le16 *)&buf[14]);
	d->calib.dig_p6 = le16_to_cpup((__le16 *)&buf[16]);
	d->calib.dig_p7 = le16_to_cpup((__le16 *)&buf[18]);
	d->calib.dig_p8 = le16_to_cpup((__le16 *)&buf[20]);
	d->calib.dig_p9 = le16_to_cpup((__le16 *)&buf[22]);
	return 0;
}

/* Raw 20-bit ADC words from a single burst read of PRESS_MSB..TEMP_XLSB. */
static int bmp280_read_raw(struct bmp280_data *d, s32 *adc_t, s32 *adc_p)
{
	u8 buf[6];
	int ret;

	ret = i2c_smbus_read_i2c_block_data(d->client, BMP280_REG_PRESS_MSB,
					    sizeof(buf), buf);
	if (ret < 0)
		return ret;
	if (ret != sizeof(buf))
		return -EIO;

	*adc_p = ((s32)buf[0] << 12) | ((s32)buf[1] << 4) | (buf[2] >> 4);
	*adc_t = ((s32)buf[3] << 12) | ((s32)buf[4] << 4) | (buf[5] >> 4);
	return 0;
}

/* Datasheet fixed-point compensation. Returns temp in 0.01 degC, sets t_fine. */
static s32 bmp280_compensate_temp(struct bmp280_data *d, s32 adc_t, s32 *t_fine)
{
	const struct bmp280_calib *c = &d->calib;
	s32 var1, var2;

	var1 = ((((adc_t >> 3) - ((s32)c->dig_t1 << 1))) * ((s32)c->dig_t2)) >> 11;
	var2 = (((((adc_t >> 4) - ((s32)c->dig_t1)) *
		  ((adc_t >> 4) - ((s32)c->dig_t1))) >> 12) *
		((s32)c->dig_t3)) >> 14;
	*t_fine = var1 + var2;
	return (*t_fine * 5 + 128) >> 8;
}

/* Datasheet 32-bit compensation. Returns pressure in Pa. */
static u32 bmp280_compensate_press(struct bmp280_data *d, s32 adc_p, s32 t_fine)
{
	const struct bmp280_calib *c = &d->calib;
	s32 var1, var2;
	u32 p;

	var1 = (t_fine >> 1) - 64000;
	var2 = (((var1 >> 2) * (var1 >> 2)) >> 11) * ((s32)c->dig_p6);
	var2 = var2 + ((var1 * ((s32)c->dig_p5)) << 1);
	var2 = (var2 >> 2) + (((s32)c->dig_p4) << 16);
	var1 = ((((s32)c->dig_p3 * (((var1 >> 2) * (var1 >> 2)) >> 13)) >> 3) +
		((((s32)c->dig_p2) * var1) >> 1)) >> 18;
	var1 = ((32768 + var1) * ((s32)c->dig_p1)) >> 15;
	if (var1 == 0)
		return 0;	/* avoid divide-by-zero */

	p = (((u32)(((s32)1048576) - adc_p) - (var2 >> 12))) * 3125;
	if (p < 0x80000000)
		p = (p << 1) / ((u32)var1);
	else
		p = (p / (u32)var1) * 2;

	var1 = (((s32)c->dig_p9) * ((s32)(((p >> 3) * (p >> 3)) >> 13))) >> 12;
	var2 = (((s32)(p >> 2)) * ((s32)c->dig_p8)) >> 13;
	p = (u32)((s32)p + ((var1 + var2 + (s32)c->dig_p7) >> 4));
	return p;
}

static int bmp280_measure(struct bmp280_data *d, s32 *temp_c100, u32 *press_pa)
{
	s32 adc_t, adc_p, t_fine;
	int ret;

	mutex_lock(&d->lock);
	ret = bmp280_read_raw(d, &adc_t, &adc_p);
	if (ret) {
		mutex_unlock(&d->lock);
		return ret;
	}
	*temp_c100 = bmp280_compensate_temp(d, adc_t, &t_fine);
	*press_pa  = bmp280_compensate_press(d, adc_p, t_fine);
	mutex_unlock(&d->lock);
	return 0;
}

static ssize_t temperature_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct bmp280_data *d = dev_get_drvdata(dev);
	s32 t_c100;
	u32 p_pa;
	int ret = bmp280_measure(d, &t_c100, &p_pa);

	if (ret)
		return ret;
	/* 0.01 degC -> millidegC */
	return sysfs_emit(buf, "%d\n", t_c100 * 10);
}
static DEVICE_ATTR_RO(temperature);

static ssize_t pressure_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct bmp280_data *d = dev_get_drvdata(dev);
	s32 t_c100;
	u32 p_pa;
	int ret = bmp280_measure(d, &t_c100, &p_pa);

	if (ret)
		return ret;
	return sysfs_emit(buf, "%u\n", p_pa);
}
static DEVICE_ATTR_RO(pressure);

static struct attribute *bmp280_attrs[] = {
	&dev_attr_temperature.attr,
	&dev_attr_pressure.attr,
	NULL,
};
ATTRIBUTE_GROUPS(bmp280);

static int bmp280_probe(struct i2c_client *client)
{
	struct bmp280_data *d;
	int id, ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_I2C_BLOCK))
		return dev_err_probe(&client->dev, -EOPNOTSUPP,
				     "adapter lacks I2C block read\n");

	id = i2c_smbus_read_byte_data(client, BMP280_REG_ID);
	if (id < 0)
		return dev_err_probe(&client->dev, id, "chip ID read failed\n");
	if (id != BMP280_CHIP_ID)
		return dev_err_probe(&client->dev, -ENODEV,
				     "unexpected chip ID 0x%02x\n", id);

	d = devm_kzalloc(&client->dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	d->client = client;
	mutex_init(&d->lock);
	i2c_set_clientdata(client, d);
	dev_set_drvdata(&client->dev, d);

	ret = i2c_smbus_write_byte_data(client, BMP280_REG_RESET, BMP280_RESET_MAGIC);
	if (ret)
		return dev_err_probe(&client->dev, ret, "soft reset failed\n");
	usleep_range(3000, 4000);	/* datasheet: 2 ms start-up */

	ret = bmp280_read_calib(d);
	if (ret)
		return dev_err_probe(&client->dev, ret, "calibration read failed\n");

	ret = i2c_smbus_write_byte_data(client, BMP280_REG_CONFIG, BMP280_CONFIG_VAL);
	if (ret)
		return dev_err_probe(&client->dev, ret, "config write failed\n");
	ret = i2c_smbus_write_byte_data(client, BMP280_REG_CTRL_MEAS,
					BMP280_CTRL_MEAS_VAL);
	if (ret)
		return dev_err_probe(&client->dev, ret, "ctrl_meas write failed\n");

	dev_info(&client->dev, "BMP280 ready at 0x%02x (normal mode, x1/x1)\n",
		 client->addr);
	return 0;
}

static void bmp280_remove(struct i2c_client *client)
{
	/* Put the sensor back to sleep (mode bits = 0). */
	i2c_smbus_write_byte_data(client, BMP280_REG_CTRL_MEAS,
				  BMP280_CTRL_MEAS_VAL & ~0x03);
}

static const struct i2c_device_id bmp280_id[] = {
	{ "bmp280_simple", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, bmp280_id);

static const struct of_device_id bmp280_of_match[] = {
	{ .compatible = "mycompany,bmp280-simple" },
	{ }
};
MODULE_DEVICE_TABLE(of, bmp280_of_match);

static struct i2c_driver bmp280_driver = {
	.driver = {
		.name           = "bmp280_simple",
		.of_match_table = bmp280_of_match,
		.dev_groups     = bmp280_groups,
	},
	.probe    = bmp280_probe,
	.remove   = bmp280_remove,
	.id_table = bmp280_id,
};
module_i2c_driver(bmp280_driver);

MODULE_AUTHOR("Arjun Rajput");
MODULE_DESCRIPTION("Minimal BMP280 I2C client driver with sysfs interface");
MODULE_LICENSE("GPL v2");
