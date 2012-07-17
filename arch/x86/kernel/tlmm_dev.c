#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <asm/tlmm.h>

static long tlmm_dev_ioctl(struct file *fp, unsigned int cmd,
			   unsigned long arg)
{
	switch (cmd) {
	case TLMM_RESERVE:
	{
		long r;

		r = tlmm_reserve();
		if (IS_ERR_VALUE(r))
			return r;
		if (copy_to_user((void *)arg, &r, sizeof(r)))
			return -EFAULT;
		return 0;
	}
	case TLMM_PMAP:
	{
		struct tlmm_pmap p;

		if (copy_from_user(&p, (void *)arg, sizeof(p)))
			return -EFAULT;
		return tlmm_pmap(p.addr, p.upd, p.npd, p.prot);
	}
	case TLMM_PALLOC:
		return tlmm_palloc();
	default:
		printk(KERN_INFO "tlmm_dev_ioctl: cmd %u\n", cmd);
	}
	return -ENOSYS;
}

static const struct file_operations tlmm_dev_ops = {
	.unlocked_ioctl = tlmm_dev_ioctl,
};

static struct miscdevice tlmm_dev = {
	.minor = 167,
	.name = "tlmm",
	.fops = &tlmm_dev_ops,
};

static int __init tlmm_dev_init(void)
{
	int err;

	err = misc_register(&tlmm_dev);
	if (err != 0) {
		printk(KERN_ERR "/dev/tlmm: failed to register: %d\n", err);
		return err;
	}
	return 0;
}
late_initcall(tlmm_dev_init);
