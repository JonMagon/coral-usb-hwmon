// SPDX-License-Identifier: GPL-2.0-only
/* Temperature telemetry for the runtime firmware of the Coral USB Accelerator. */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/hwmon.h>
#include <linux/idr.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/usb.h>
#include <linux/version.h>

#define OMC0_D0 0x1a0d0
#define OMC0_D8 0x1a0d8
#define OMC0_DC 0x1a0dc
#define ADC_MASK GENMASK(25, 16)
#define CLEAR_ALARMS GENMASK(29, 28)
#define USB_TIMEOUT_MS 500

static const struct sensor_field {
	u32 reg;
	u32 mask;
} enables[] = {
	{ OMC0_D0, BIT(7) },       /* sensor clocks */
	{ OMC0_D8, GENMASK(2, 0) }, /* ENAD, ENVR, ENBG */
	{ OMC0_DC, BIT(0) },       /* ENTHMC */
};

struct coral {
	struct list_head list;
	struct usb_device *usb;
	struct device *hwmon, *apex;
	struct mutex lock;
	u32 changed[ARRAY_SIZE(enables)];
	int index;
	bool cached;
	unsigned long sampled;
	long temperature;
};

static bool apex_compat;
module_param(apex_compat, bool, 0444);
MODULE_PARM_DESC(apex_compat, "Provide /sys/class/apex/apex_N/temp on USB-only systems");
static struct class *apex_class;
static DEFINE_IDA(indices);
static LIST_HEAD(devices);
static DEFINE_MUTEX(devices_lock);

static bool is_coral(struct usb_device *usb)
{
	return le16_to_cpu(usb->descriptor.idVendor) == 0x18d1 &&
	       le16_to_cpu(usb->descriptor.idProduct) == 0x9302;
}

static int csr_read(struct coral *c, u32 reg, u32 *value)
{
	__le32 raw;
	int ret;

	ret = usb_control_msg_recv(c->usb, 0, 1, USB_DIR_IN | USB_TYPE_VENDOR |
				   USB_RECIP_DEVICE, reg & 0xffff, reg >> 16,
				   &raw, sizeof(raw), USB_TIMEOUT_MS, GFP_KERNEL);
	if (!ret)
		*value = le32_to_cpu(raw);
	return ret;
}

static int csr_write(struct coral *c, u32 reg, u32 value)
{
	__le32 raw;

	/* Never echo the write-one-to-clear thermal interrupt flags. */
	if (reg == OMC0_DC)
		value &= ~CLEAR_ALARMS;
	raw = cpu_to_le32(value);
	return usb_control_msg_send(c->usb, 0, 1, USB_TYPE_VENDOR | USB_RECIP_DEVICE,
				    reg & 0xffff, reg >> 16, &raw, sizeof(raw),
				    USB_TIMEOUT_MS, GFP_KERNEL);
}

static int enable_sensor(struct coral *c)
{
	int i, ret;
	u32 value, missing;

	for (i = 0; i < ARRAY_SIZE(enables); i++) {
		if (enables[i].reg == OMC0_DC)
			usleep_range(100, 300);
		ret = csr_read(c, enables[i].reg, &value);
		if (ret)
			return ret;
		missing = enables[i].mask & ~value;
		if (missing) {
			ret = csr_write(c, enables[i].reg, value | missing);
			if (ret)
				return ret;
			c->changed[i] |= missing;
		}
	}
	return 0;
}

static int temperature(struct coral *c, long *out)
{
	u32 raw, adc;
	int ret = 0, i;

	mutex_lock(&c->lock);
	if (c->cached && time_before(jiffies, c->sampled + HZ))
		goto result;
	c->cached = false;
	/* Disconnect holds the USB device lock while draining sysfs readers. */
	for (i = 0; !usb_trylock_device(c->usb); i++) {
		if (i == 25) {
			ret = -EAGAIN;
			goto done;
		}
		usleep_range(1000, 2000);
	}
	if (c->usb->state != USB_STATE_CONFIGURED) {
		ret = -ENODATA;
		goto unlock_usb;
	}
	ret = csr_read(c, OMC0_DC, &raw);
	if (ret)
		goto unlock_usb;
	if (!(raw & BIT(0))) {
		ret = enable_sensor(c);
		if (ret)
			goto unlock_usb;
	}
	/* Conversion can still contain the reset sentinel just after enable. */
	for (i = 0; i < 12; i++) {
		adc = FIELD_GET(ADC_MASK, raw);
		if ((raw & BIT(0)) && adc && adc != 1023)
			break;
		usleep_range(8000, 10000);
		ret = csr_read(c, OMC0_DC, &raw);
		if (ret)
			goto unlock_usb;
	}
	adc = FIELD_GET(ADC_MASK, raw);
	if (!(raw & BIT(0)) || !adc || adc == 1023) {
		ret = -ENODATA;
		goto unlock_usb;
	}
	c->temperature = (662 - (int)adc) * 250 + 550;
	c->sampled = jiffies;
	c->cached = true;
unlock_usb:
	usb_unlock_device(c->usb);
result:
	if (!ret)
		*out = c->temperature;
done:
	mutex_unlock(&c->lock);
	return ret;
}

static umode_t visible(const void *data, enum hwmon_sensor_types type,
		       u32 attr, int channel)
{
	return type == hwmon_temp && attr == hwmon_temp_input ? 0444 : 0;
}

static int hwmon_read(struct device *dev, enum hwmon_sensor_types type,
		      u32 attr, int channel, long *val)
{
	return temperature(dev_get_drvdata(dev), val);
}

static const struct hwmon_ops ops = { .is_visible = visible, .read = hwmon_read };
static const struct hwmon_channel_info *channels[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT), NULL
};
static const struct hwmon_chip_info chip = { .ops = &ops, .info = channels };

static ssize_t temp_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	long value;
	int ret = temperature(dev_get_drvdata(dev), &value);

	return ret ? ret : sysfs_emit(buf, "%ld\n", value);
}
static DEVICE_ATTR_RO(temp);
static struct attribute *apex_attrs[] = { &dev_attr_temp.attr, NULL };
ATTRIBUTE_GROUPS(apex);

static void attach(struct usb_device *usb)
{
	struct coral *c;

	mutex_lock(&devices_lock);
	list_for_each_entry(c, &devices, list)
		if (c->usb == usb)
			goto out;
	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		goto out;
	c->index = ida_alloc(&indices, GFP_KERNEL);
	if (c->index < 0)
		goto free;
	c->usb = usb_get_dev(usb);
	mutex_init(&c->lock);
	c->hwmon = hwmon_device_register_with_info(&usb->dev, "coral_usb", c,
						    &chip, NULL);
	if (IS_ERR(c->hwmon)) {
		dev_err(&usb->dev, "Coral hwmon registration failed: %ld\n", PTR_ERR(c->hwmon));
		usb_put_dev(usb);
		ida_free(&indices, c->index);
		goto free;
	}
	if (apex_class) {
		c->apex = device_create_with_groups(apex_class, &usb->dev, 0, c,
						    apex_groups, "apex_%d", c->index);
		if (IS_ERR(c->apex)) {
			dev_warn(&usb->dev, "Coral compatibility attribute failed: %ld\n",
				 PTR_ERR(c->apex));
			c->apex = NULL;
		}
	}
	list_add(&c->list, &devices);
	goto out;
free:
	kfree(c);
out:
	mutex_unlock(&devices_lock);
}

/* Readers are drained before restoring only the bits this module enabled. */
static void detach(struct coral *c, bool restore)
{
	int i, ret;
	u32 value;

	if (c->apex)
		device_unregister(c->apex);
	hwmon_device_unregister(c->hwmon);
	if (restore && c->usb->state == USB_STATE_CONFIGURED) {
		for (i = ARRAY_SIZE(enables) - 1; i >= 0; i--) {
			if (!c->changed[i])
				continue;
			ret = csr_read(c, enables[i].reg, &value);
			if (!ret && (value & c->changed[i]))
				ret = csr_write(c, enables[i].reg, value & ~c->changed[i]);
			if (ret)
				dev_warn(&c->usb->dev, "Could not restore sensor register %#x: %d\n",
					 enables[i].reg, ret);
		}
	}
	list_del(&c->list);
	ida_free(&indices, c->index);
	usb_put_dev(c->usb);
	kfree(c);
}

static int notify(struct notifier_block *nb, unsigned long event, void *data)
{
	struct usb_device *usb = data;
	struct coral *c;

	if ((event != USB_DEVICE_ADD && event != USB_DEVICE_REMOVE) || !is_coral(usb))
		return NOTIFY_DONE;
	if (event == USB_DEVICE_ADD) {
		attach(usb);
	} else {
		mutex_lock(&devices_lock);
		list_for_each_entry(c, &devices, list) {
			if (c->usb == usb) {
				detach(c, false);
				break;
			}
		}
		mutex_unlock(&devices_lock);
	}
	return NOTIFY_OK;
}

static struct notifier_block notifier = { .notifier_call = notify };

static int scan(struct usb_device *usb, void *unused)
{
	if (is_coral(usb)) {
		usb_lock_device(usb);
		if (usb->actconfig && usb->state != USB_STATE_NOTATTACHED)
			attach(usb);
		usb_unlock_device(usb);
	}
	return 0;
}

static int __init coral_init(void)
{
	if (apex_compat) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
		apex_class = class_create(THIS_MODULE, "apex");
#else
		apex_class = class_create("apex");
#endif
		if (IS_ERR(apex_class)) {
			pr_warn("coral_usb_hwmon: apex class unavailable (%ld), using hwmon only\n",
				PTR_ERR(apex_class));
			apex_class = NULL;
		}
	}
	usb_register_notify(&notifier);
	usb_for_each_dev(NULL, scan);
	return 0;
}

static void __exit coral_exit(void)
{
	struct coral *c, *next;

	usb_unregister_notify(&notifier);
	list_for_each_entry_safe(c, next, &devices, list) {
		struct usb_device *usb = usb_get_dev(c->usb);

		usb_lock_device(usb);
		detach(c, true);
		usb_unlock_device(usb);
		usb_put_dev(usb);
	}
	if (apex_class)
		class_destroy(apex_class);
	ida_destroy(&indices);
}

module_init(coral_init);
module_exit(coral_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Temperature monitoring for the Coral USB Accelerator");
