#include <linux/module.h>
#include <linux/kernel.h>

#include "nv_fermi_drv.h"

int nv_fermi_i2c_init(struct nv_fermi_priv *priv)
{
	if (!priv->dcb_valid) {
		pr_warn(DRV_NAME ": DCB not parsed, skipping I2C init\n");
		return -ENODEV;
	}

	pr_info(DRV_NAME ": I2C engine init not yet implemented\n");
	return 0;
}
