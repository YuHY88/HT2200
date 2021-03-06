/*
 * zl6100.c - driver for the Intersil zl2006 chip
 *
 * Copyright (C) 2010 Freescale Semiconductor, Inc.
 * Author: Yuantian Tang <b29983@freescale.com>
 *
 * The ZL2006 is a digital DC-DC controller with integrated MOSFET
 * drivers made by Intersil Americas Inc.
 * The ZL2006 is designed to be a flexible building block for DC
 * power and can be easily adapted to designs ranging from a
 * single-phase power supply operating from a 3.3V input to a multi-phase
 * supply operating from a 12V input. The output voltage range is from
 * 0.54V to 5.5V with 1% or -1% output voltage accuracy.
 * All of ZL2006's operating features can be configured by simple
 * pinstrap/resistor selection or through the SMBus serial interface.
 * The ZL2006 uses the PMBus protocol for communication with a host
 * controller. Complete datasheet can be obtained from Intersil's website
 * at:
 *		http://www.intersil.com/products/deviceinfo.asp?pn=ZL2006
 *
 * This driver exports the values of output voltage and current
 * to sysfs. The voltage unit is mV, and the current unit is mA.
 * The user space lm-sensors tool(VER:3.1.2 or above) can get and display
 * these values through the sysfs interface.
 *
 * NOTE: If the lm-sensors tool get the value of 0, probably because something
 * wrong happens with I2C bus, such as BUSY or NOT READY etc.
 * It is recommended to retrieve the data again.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/module.h>
#include <linux/jiffies.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/i2c.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <asm/fsl_pixis.h>

/*
 * zl6100 COMMAND
 */
#define VOUT_MODE	0x20	/* the output mode */
#define DEVICE_ID	0xE4	/* device id */
#define READ_VOUT	0x8B	/* the output voltage */
#define READ_IOUT	0x8C	/* the output current */

/**
 * micro defined
 */
#define LEN_BYTE		0x01	/* read one byte data */
#define LEN_WORD		0x02	/* read two bytes data */
#define ZL6100_ID		"\x10ZL6100-002-FE03"	/* chip id */

/**
 * Client data (each client gets its own)
 */
struct zl6100_data {
	struct device *hwmon_dev;
	s32 val;
};

/**
 * read zl6100 register
 *
 * @client: handle to slave device
 * @cmd	:	command
 * @len	:	word or byte
 *
 */
static s32 read_register(struct i2c_client *client, u8 cmd, u8 len)
{

	struct zl6100_data *priv_data = i2c_get_clientdata(client);

	/* The shortest interval of time between two READ command is 2ms
	 * for ZL2006 chip.
	 * So, we delay 2ms, just in case.
	 */
	mdelay(2);

	switch (len) {
	case LEN_BYTE:
		priv_data->val = i2c_smbus_read_byte_data(client, cmd);
		break;

	case LEN_WORD:
		priv_data->val = i2c_smbus_read_word_data(client, cmd);
		break;

	default:
		priv_data->val = -EIO;
	}

	return priv_data->val;
}

/**
 * check the data format, ONLY linear mode data format with value 0x13
 * is supported
 *
 * @client	:	handle to slave device
 *
 */
static int check_data_mode(struct i2c_client *client)
{
	s32	mode;

	mode = read_register(client, VOUT_MODE, LEN_BYTE);
	if (mode != 0x13) {
		printk(KERN_WARNING "chip 0x%2x mode is not LINEAR.\n",
				client->addr);
		return mode;
	}

	return 0;
}

/**
 * make sure zl6100 chip is used. id is "\x10ZL6100-002-FE03"
 *
 * @client	:	handle to slave
 *
 */
static int zl6100_check_id(struct i2c_client *client)
{
	s32 id;
	u8	buf[16];

	mdelay(2);
	id = i2c_smbus_read_i2c_block_data(client, DEVICE_ID, 16, buf);
	if (id != 16 || memcmp(buf, ZL6100_ID, 7))
		printk(KERN_WARNING
			"chip 0x%2x ID does not match.\n", client->addr);

	return 0;
}

/**
 * call back function to output the voltage value
 *
 * If we get error,like BUSY, just return 0 to user
 */
static int show_volt(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	s32 volt;

	struct i2c_client *client = to_i2c_client(dev);

	volt = read_register(client, READ_VOUT, LEN_WORD);
	if (volt < 0)
		return sprintf(buf, "%d\n", 0);

	volt = pmbus_2volt(volt);

	return sprintf(buf, "%d\n", volt);
}

/**
 * the call back function to output current value
 *
 * If we get error, just return 0 to user
 */
static int show_curr(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	s32	cur;

	struct i2c_client *client = to_i2c_client(dev);

	cur = read_register(client, READ_IOUT, LEN_WORD);

	cur = pmbus_2cur(cur);

	return sprintf(buf, "%d\n", cur);
}

static DEVICE_ATTR(in0_input, S_IRUGO, show_volt, NULL);
static DEVICE_ATTR(curr1_input, S_IRUGO, show_curr, NULL);

static struct attribute *zl6100_attributes[] = {
	&dev_attr_in0_input.attr,
	&dev_attr_curr1_input.attr,
	NULL
};

static const struct attribute_group zl6100_group = {
	.attrs = zl6100_attributes,
};

static int zl6100_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	int err;
	struct zl6100_data *data = NULL;

	if (!i2c_check_functionality(client->adapter,
			I2C_FUNC_SMBUS_WORD_DATA |
			I2C_FUNC_SMBUS_BYTE_DATA |
			I2C_FUNC_SMBUS_I2C_BLOCK))
		return -EIO;


	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	i2c_set_clientdata(client, data);

	err = sysfs_create_group(&client->dev.kobj, &zl6100_group);
	if (err)
		goto exit_free;

	data->hwmon_dev = hwmon_device_register(&client->dev);
	if (IS_ERR(data->hwmon_dev)) {
		err = PTR_ERR(data->hwmon_dev);
		goto exit_remove;
	}


	/* dummy read, wait for the parameters can be monitored */
	mdelay(2);
	i2c_smbus_read_word_data(client, READ_VOUT);

	check_data_mode(client);
	zl6100_check_id(client);

	return 0;

exit_remove:
	sysfs_remove_group(&client->dev.kobj, &zl6100_group);
exit_free:
	kfree(data);
	i2c_set_clientdata(client, NULL);
	return err;
}

static int zl6100_remove(struct i2c_client *client)
{
	struct zl6100_data *data = i2c_get_clientdata(client);

	hwmon_device_unregister(data->hwmon_dev);
	sysfs_remove_group(&client->dev.kobj, &zl6100_group);
	kfree(data);
	i2c_set_clientdata(client, NULL);

	return 0;
}

static const struct i2c_device_id zl6100_id[] = {
	{ "zl6100", 0},
	{ }
};

static struct i2c_driver zl6100_driver = {
	.driver = {
		.name = "zl6100",
	},
	.probe = zl6100_probe,
	.remove = zl6100_remove,
	.id_table = zl6100_id,
};

static int __init zl6100_init(void)
{
	return i2c_add_driver(&zl6100_driver);
}

static void __exit zl6100_exit(void)
{
	i2c_del_driver(&zl6100_driver);
}

MODULE_AUTHOR("Yuantian Tang <b29983@freescale.com>");
MODULE_DESCRIPTION("Intersil zl6100 driver");
MODULE_LICENSE("GPL");

module_init(zl6100_init);
module_exit(zl6100_exit);
