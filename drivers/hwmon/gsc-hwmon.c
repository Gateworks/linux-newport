/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018 Gateworks Corporation
 *
 * This driver registers Linux HWMON attributes for GSC ADC's
 */
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/mfd/gsc.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include <linux/platform_data/gsc_hwmon.h>

#define GSC_HWMON_MAX_TEMP_CH	16
#define GSC_HWMON_MAX_IN_CH	16
#define GSC_HWMON_MAX_FAN_CH	6

struct gsc_hwmon_data {
	struct gsc_dev *gsc;
	struct device *dev;
	struct gsc_hwmon_platform_data *pdata;
	const struct gsc_hwmon_channel *temp_ch[GSC_HWMON_MAX_TEMP_CH];
	const struct gsc_hwmon_channel *in_ch[GSC_HWMON_MAX_IN_CH];
	const struct gsc_hwmon_channel *fan_ch[GSC_HWMON_MAX_FAN_CH];
	u32 temp_config[GSC_HWMON_MAX_TEMP_CH + 1];
	u32 in_config[GSC_HWMON_MAX_IN_CH + 1];
	u32 fan_config[GSC_HWMON_MAX_FAN_CH + 1];
	struct hwmon_channel_info temp_info;
	struct hwmon_channel_info in_info;
	struct hwmon_channel_info fan_info;
	const struct hwmon_channel_info *info[4];
	struct hwmon_chip_info chip;
};

static int
gsc_hwmon_read(struct device *dev, enum hwmon_sensor_types type, u32 attr,
	       int channel, long *val)
{
	struct gsc_hwmon_data *hwmon = dev_get_drvdata(dev);
	int sz, ret;
	u8 reg;
	u8 buf[3];

	dev_dbg(dev, "%s type=%d attr=%d channel=%d\n", __func__, type, attr,
		channel);
	switch (type) {
	case hwmon_in:
		if (channel > GSC_HWMON_MAX_IN_CH)
			return -EOPNOTSUPP;
		reg = hwmon->in_ch[channel]->reg;
		sz = 3;
		break;
	case hwmon_temp:
		if (channel > GSC_HWMON_MAX_TEMP_CH)
			return -EOPNOTSUPP;
		reg = hwmon->temp_ch[channel]->reg;
		sz = 2;
		break;
	case hwmon_fan:
		if (channel > GSC_HWMON_MAX_FAN_CH)
			return -EOPNOTSUPP;
		reg = hwmon->fan_ch[channel]->reg;
		sz = 2;
		break;
	default:
		return -EOPNOTSUPP;
	}

	ret = regmap_bulk_read(hwmon->gsc->regmap_hwmon, reg, &buf, sz);
	if (ret)
		return ret;

	*val = 0;
	while (sz-- > 0)
		*val |= (buf[sz] << (8*sz));
	if ((type == hwmon_temp) && *val > 0x8000)
		*val -= 0xffff;

	return 0;
}

static int
gsc_hwmon_read_string(struct device *dev, enum hwmon_sensor_types type,
		      u32 attr, int channel, const char **buf)
{
	struct gsc_hwmon_data *hwmon = dev_get_drvdata(dev);

	dev_dbg(dev, "%s type=%d attr=%d channel=%d\n", __func__, type, attr,
		channel);
	switch (type) {
	case hwmon_in:
		if (channel > GSC_HWMON_MAX_IN_CH)
			return -EOPNOTSUPP;
		*buf = hwmon->in_ch[channel]->name;
		break;
	case hwmon_temp:
		if (channel > GSC_HWMON_MAX_TEMP_CH)
			return -EOPNOTSUPP;
		*buf = hwmon->temp_ch[channel]->name;
		break;
	case hwmon_fan:
		if (channel > GSC_HWMON_MAX_FAN_CH)
			return -EOPNOTSUPP;
		*buf = hwmon->fan_ch[channel]->name;
		break;
	default:
		return -ENOTSUPP;
	}

	return 0;
}

static int
gsc_hwmon_write(struct device *dev, enum hwmon_sensor_types type, u32 attr,
		int channel, long val)
{
	struct gsc_hwmon_data *hwmon = dev_get_drvdata(dev);
	u8 buf[3];

	dev_dbg(dev, "%s type=%d attr=%d channel=%d\n", __func__, type, attr,
		channel);
	switch (type) {
	case hwmon_fan:
		if (channel == GSC_HWMON_MAX_FAN_CH)
			return -EOPNOTSUPP;
		buf[0] = val & 0xff;
		buf[1] = (val >> 8) & 0xff;
		return regmap_bulk_write(hwmon->gsc->regmap_hwmon,
					 hwmon->fan_ch[channel]->reg, buf, 2);
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static umode_t
gsc_hwmon_is_visible(const void *_data, enum hwmon_sensor_types type, u32 attr,
		     int ch)
{
	const struct gsc_hwmon_data *hwmon = _data;
	struct device *dev = hwmon->gsc->dev;
	umode_t mode = 0;

	switch (type) {
	case hwmon_fan:
		mode = S_IRUGO;
		if (attr == hwmon_fan_input)
			mode |= S_IWUSR;
		break;
	case hwmon_temp:
	case hwmon_in:
		mode = S_IRUGO;
		break;
	default:
		break;
	}
	dev_dbg(dev, "%s type=%d attr=%d ch=%d mode=0x%x\n", __func__, type,
		attr, ch, mode);

	return mode;
}

static const struct hwmon_ops gsc_hwmon_ops = {
	.is_visible = gsc_hwmon_is_visible,
	.read = gsc_hwmon_read,
	.read_string = gsc_hwmon_read_string,
	.write = gsc_hwmon_write,
};

static struct gsc_hwmon_platform_data *
gsc_hwmon_get_devtree_pdata(struct device* dev)
{
	struct gsc_hwmon_platform_data *pdata;
	struct gsc_hwmon_channel *ch;
	struct fwnode_handle *child;
	int nchannels;

        nchannels = device_get_child_node_count(dev);
        dev_dbg(dev, "channels=%d\n", nchannels);
        if (nchannels == 0)
                return ERR_PTR(-ENODEV);

	pdata = devm_kzalloc(dev,
			     sizeof(*pdata) + nchannels * sizeof(*ch),
			     GFP_KERNEL);
	if (!pdata)
		return ERR_PTR(-ENOMEM);
	ch = (struct gsc_hwmon_channel *)(pdata + 1);
        pdata->channels = ch;
        pdata->nchannels = nchannels;

	/* allocate structures for channels and count instances of each type */
	device_for_each_child_node(dev, child) {
		if (fwnode_property_read_string(child, "label", &ch->name)) {
			dev_err(dev, "channel without label\n");
			fwnode_handle_put(child);
			return ERR_PTR(-EINVAL);
		}
		if (fwnode_property_read_u32(child, "reg", &ch->reg)) {
			dev_err(dev, "channel without reg\n");
			fwnode_handle_put(child);
			return ERR_PTR(-EINVAL);
		}
		if (fwnode_property_read_u32(child, "type", &ch->type)) {
			dev_err(dev, "channel without type\n");
			fwnode_handle_put(child);
			return ERR_PTR(-EINVAL);
		}
		dev_dbg(dev, "of: reg=0x%02x type=%d %s\n", ch->reg, ch->type,
			ch->name);
		ch++;
	}

	return pdata;
}

static int gsc_hwmon_probe(struct platform_device *pdev)
{
	struct gsc_dev *gsc = dev_get_drvdata(pdev->dev.parent);
	struct device *dev = &pdev->dev;
	struct gsc_hwmon_platform_data *pdata = dev_get_platdata(dev);
	struct gsc_hwmon_data *hwmon;
	int i, i_in, i_temp, i_fan;

	if (!pdata) {
		pdata = gsc_hwmon_get_devtree_pdata(dev);
		if (IS_ERR(pdata))
			return PTR_ERR(pdata);
	}

	hwmon = devm_kzalloc(dev, sizeof(*hwmon), GFP_KERNEL);
	if (!hwmon)
		return -ENOMEM;
	hwmon->gsc = gsc;
	hwmon->pdata = pdata;

	for (i = 0, i_in = 0, i_temp = 0, i_fan = 0;
	     i < hwmon->pdata->nchannels; i++)
	{
		const struct gsc_hwmon_channel *ch = &pdata->channels[i];

		if (ch->reg > GSC_HWMON_MAX_REG) {
			dev_err(dev, "invalid reg: 0x%02x\n", ch->reg);
			return -EINVAL;
		}
		switch(ch->type) {
		case type_temperature:
			if (i_temp == GSC_HWMON_MAX_TEMP_CH) {
				dev_err(dev, "too many temp channels\n");
				return -EINVAL;
			}
			hwmon->temp_ch[i_temp] = ch;
			hwmon->temp_config[i_temp] =
				HWMON_T_INPUT | HWMON_T_LABEL;
			i_temp++;
			break;
		case type_voltage:
			if (i_in == GSC_HWMON_MAX_IN_CH) {
				dev_err(dev, "too many voltage channels\n");
				return -EINVAL;
			}
			hwmon->in_ch[i_in] = ch;
			hwmon->in_config[i_in] =
				HWMON_I_INPUT | HWMON_I_LABEL;
			i_in++;
			break;
		case type_fan:
			if (i_fan == GSC_HWMON_MAX_FAN_CH) {
				dev_err(dev, "too many voltage channels\n");
				return -EINVAL;
			}
			hwmon->fan_ch[i_fan] = ch;
			hwmon->fan_config[i_fan] =
				HWMON_F_INPUT | HWMON_F_LABEL;
			i_fan++;
			break;
		default:
			dev_err(dev, "invalid type: %d\n", ch->type);
			return -EINVAL;
		}
		dev_dbg(dev, "pdata: reg=0x%02x type=%d %s\n", ch->reg,
			ch->type, ch->name);
	}

	/* terminate channel config lists */
	hwmon->temp_config[i_temp] = 0;
	hwmon->in_config[i_in] = 0;
	hwmon->fan_config[i_fan] = 0;

	/* setup config structures */
	hwmon->chip.ops = &gsc_hwmon_ops;
	hwmon->chip.info = hwmon->info;
	hwmon->info[0] = &hwmon->temp_info;
	hwmon->info[1] = &hwmon->in_info;
	hwmon->info[2] = &hwmon->fan_info;
	hwmon->temp_info.type = hwmon_temp;
	hwmon->temp_info.config = hwmon->temp_config;
	hwmon->in_info.type = hwmon_in;
	hwmon->in_info.config = hwmon->in_config;
	hwmon->fan_info.type = hwmon_fan;
	hwmon->fan_info.config = hwmon->fan_config;

	hwmon->dev = devm_hwmon_device_register_with_info(dev,
							  KBUILD_MODNAME, hwmon,
							  &hwmon->chip, NULL);
	return PTR_ERR_OR_ZERO(hwmon->dev);
}

static const struct of_device_id gsc_hwmon_of_match[] = {
	{ .compatible = "gw,gsc-hwmon", },
	{}
};

static struct platform_driver gsc_hwmon_driver = {
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = gsc_hwmon_of_match,
	},
	.probe = gsc_hwmon_probe,
};

module_platform_driver(gsc_hwmon_driver);

MODULE_AUTHOR("Tim Harvey <tharvey@gateworks.com>");
MODULE_DESCRIPTION("GSC hardware monitor driver");
MODULE_LICENSE("GPL v2");
