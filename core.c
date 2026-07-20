#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/init.h>

#include "nv_fermi_drv.h"

static const struct pci_device_id nv_fermi_pci_ids[] = {
	{ PCI_DEVICE(NV_VENDOR_ID_NVIDIA, NV_DEVICE_ID_GF108) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, nv_fermi_pci_ids);

static int nv_fermi_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct nv_fermi_priv *priv;
	u32 boot0;
	int ret;

	pr_info(DRV_NAME ": found device %04x:%04x (bus %s), revision=0x%02x\n",
		id->vendor, id->device, pci_name(pdev), pdev->revision);

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->pdev = pdev;

	ret = pci_enable_device(pdev);
	if (ret) {
		pr_err(DRV_NAME ": pci_enable_device() failed, code %d\n", ret);
		return ret;
	}

	ret = pci_request_regions(pdev, DRV_NAME);
	if (ret) {
		pr_err(DRV_NAME ": pci_request_regions() failed, code %d\n", ret);
		goto err_disable;
	}

	pci_set_master(pdev);

	priv->bar0_len = pci_resource_len(pdev, 0);
	priv->bar0 = pci_ioremap_bar(pdev, 0);
	if (!priv->bar0) {
		pr_err(DRV_NAME ": failed to map BAR0\n");
		ret = -ENOMEM;
		goto err_release;
	}
	pr_info(DRV_NAME ": BAR0 (MMIO) mapped, size=%llu bytes\n",
		(unsigned long long)priv->bar0_len);

	priv->bar1_len = pci_resource_len(pdev, 1);
	priv->bar1 = pci_ioremap_bar(pdev, 1);
	if (!priv->bar1) {
		pr_err(DRV_NAME ": failed to map BAR1\n");
		ret = -ENOMEM;
		goto err_unmap_bar0;
	}
	pr_info(DRV_NAME ": BAR1 (VRAM) mapped, size=%llu bytes\n",
		(unsigned long long)priv->bar1_len);

	boot0 = nv_rd32(priv, NV_PMC_BOOT_0);
	pr_info(DRV_NAME ": NV_PMC_BOOT_0 = 0x%08x (raw chip id)\n", boot0);
	pr_info(DRV_NAME ": chipset=0x%03x revision=0x%02x\n",
		NV_BOOT0_CHIPSET(boot0), NV_BOOT0_REVISION(boot0));

	if (NV_BOOT0_CHIPSET(boot0) == NV_CHIPSET_GF108) {
		pr_info(DRV_NAME ": chipset confirmed as GF108 (GeForce GT 430)\n");
	} else {
		pr_warn(DRV_NAME ": chipset 0x%03x does not match the expected GF108 (0x%03x) -- check the test rig hardware!\n",
			NV_BOOT0_CHIPSET(boot0), NV_CHIPSET_GF108);
	}

	pci_set_drvdata(pdev, priv);

	pr_info(DRV_NAME ": Phase 2 complete -- device active, MMIO available read-only\n");

	ret = nv_fermi_read_vbios(priv);
	if (ret) {
		pr_warn(DRV_NAME ": VBIOS read failed (code %d) -- BIT/DCB parsing skipped\n", ret);
	} else {
		if (nv_fermi_parse_dcb(priv, &priv->dcb) == 0)
			priv->dcb_valid = true;
		else
			pr_warn(DRV_NAME ": DCB parsing failed -- I2C/GPIO tables unavailable\n");
	}

	nv_fermi_i2c_init(priv);

	return 0;

err_unmap_bar0:
	iounmap(priv->bar0);
err_release:
	pci_release_regions(pdev);
err_disable:
	pci_disable_device(pdev);
	return ret;
}

static void nv_fermi_remove(struct pci_dev *pdev)
{
	struct nv_fermi_priv *priv = pci_get_drvdata(pdev);

	if (priv) {
		kfree(priv->vbios);
		if (priv->bar1)
			iounmap(priv->bar1);
		if (priv->bar0)
			iounmap(priv->bar0);
		pci_release_regions(pdev);
	}

	pci_disable_device(pdev);
	pr_info(DRV_NAME ": device %s released\n", pci_name(pdev));
}

static struct pci_driver nv_fermi_pci_driver = {
	.name     = DRV_NAME,
	.id_table = nv_fermi_pci_ids,
	.probe    = nv_fermi_probe,
	.remove   = nv_fermi_remove,
};

static int __init nv_fermi_init(void)
{
	int ret;

	pr_info(DRV_NAME ": loading module (Fermi/GF108 out-of-tree driver, Phase 1)\n");

	ret = pci_register_driver(&nv_fermi_pci_driver);
	if (ret) {
		pr_err(DRV_NAME ": failed to register pci_driver, error code %d\n", ret);
		return ret;
	}

	pr_info(DRV_NAME ": pci_driver registered successfully\n");
	return 0;
}

static void __exit nv_fermi_exit(void)
{
	pci_unregister_driver(&nv_fermi_pci_driver);
	pr_info(DRV_NAME ": module unloaded\n");
}

module_init(nv_fermi_init);
module_exit(nv_fermi_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("eco1kd");
MODULE_DESCRIPTION("Out-of-tree DRM/KMS skeleton driver for NVIDIA Fermi GF108 (GT 430)");
MODULE_VERSION("0.3-alpha");
