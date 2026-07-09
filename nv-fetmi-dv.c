#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/ctype.h>

#define DRV_NAME "nv_fermi_drv"

#define NV_VENDOR_ID_NVIDIA 0x10de
#define NV_DEVICE_ID_GF108  0x0de1

static const struct pci_device_id nv_fermi_pci_ids[] = {
	{ PCI_DEVICE(NV_VENDOR_ID_NVIDIA, NV_DEVICE_ID_GF108) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, nv_fermi_pci_ids);

#define NV_PMC_BOOT_0 0x000000

#define NV_CHIPSET_GF108 0xC1

#define NV_BOOT0_CHIPSET(boot0)  (((boot0) >> 20) & 0x1ff)
#define NV_BOOT0_REVISION(boot0) ((boot0) & 0xff)

#define NV_BIT_SIGNATURE_LEN 6
static const u8 nv_bit_signature[NV_BIT_SIGNATURE_LEN] = { 0xFF, 0xB8, 'B', 'I', 'T', 0x00 };

struct nv_bit_header {
	u16 bcd_version;
	u8  header_size;
	u8  token_size;
	u8  token_count;
	u8  checksum;
};

struct nv_bit_token {
	u8  id;
	u8  version;
	u16 length;
	u16 offset;
};

struct nv_dcb_header {
	u16 offset;
	u8  version;
	u8  header_len;
	u8  entry_count;
	u8  entry_len;
};

struct nv_fermi_priv {
	struct pci_dev  *pdev;
	void __iomem    *bar0;
	void __iomem    *bar1;
	resource_size_t  bar0_len;
	resource_size_t  bar1_len;

	u8  *vbios;
	size_t vbios_len;

	struct nv_dcb_header dcb;
	bool dcb_valid;
};

static inline u32 nv_rd32(struct nv_fermi_priv *priv, u32 reg)
{
	return ioread32(priv->bar0 + reg);
}

static inline void nv_wr32(struct nv_fermi_priv *priv, u32 reg, u32 val)
{
	iowrite32(val, priv->bar0 + reg);
}

static int nv_fermi_read_vbios(struct nv_fermi_priv *priv)
{
	struct pci_dev *pdev = priv->pdev;
	void __iomem *rom;
	size_t rom_size;

	rom = pci_map_rom(pdev, &rom_size);
	if (!rom || !rom_size) {
		pr_err(DRV_NAME ": не удалось замаппить expansion ROM (VBIOS)\n");
		if (rom)
			pci_unmap_rom(pdev, rom);
		return -ENODEV;
	}

	priv->vbios = kmalloc(rom_size, GFP_KERNEL);
	if (!priv->vbios) {
		pci_unmap_rom(pdev, rom);
		return -ENOMEM;
	}

	memcpy_fromio(priv->vbios, rom, rom_size);
	priv->vbios_len = rom_size;

	pci_unmap_rom(pdev, rom);

	if (priv->vbios_len < 2 || priv->vbios[0] != 0x55 || priv->vbios[1] != 0xAA) {
		pr_warn(DRV_NAME ": VBIOS сигнатура 0x55 0xAA не найдена (получено 0x%02x 0x%02x) — возможно ROM недоступен без acpi/legacy fallback\n",
			priv->vbios_len >= 1 ? priv->vbios[0] : 0,
			priv->vbios_len >= 2 ? priv->vbios[1] : 0);
		return -EINVAL;
	}

	pr_info(DRV_NAME ": VBIOS прочитан, размер=%zu байт, сигнатура валидна\n",
		priv->vbios_len);
	return 0;
}

static size_t nv_fermi_find_bit(struct nv_fermi_priv *priv)
{
	size_t i;

	if (priv->vbios_len < NV_BIT_SIGNATURE_LEN)
		return 0;

	for (i = 0; i <= priv->vbios_len - NV_BIT_SIGNATURE_LEN; i++) {
		if (memcmp(priv->vbios + i, nv_bit_signature, NV_BIT_SIGNATURE_LEN) == 0)
			return i;
	}
	return 0;
}

static int nv_fermi_parse_bit_header(struct nv_fermi_priv *priv, size_t bit_sig_off,
				      struct nv_bit_header *hdr, size_t *tokens_off)
{
	const u8 *p = priv->vbios + bit_sig_off + NV_BIT_SIGNATURE_LEN;

	if (bit_sig_off + NV_BIT_SIGNATURE_LEN + 6 > priv->vbios_len)
		return -EINVAL;

	hdr->bcd_version = p[0] | (p[1] << 8);
	hdr->header_size = p[2];
	hdr->token_size  = p[3];
	hdr->token_count = p[4];
	hdr->checksum    = p[5];

	*tokens_off = bit_sig_off + hdr->header_size;
	return 0;
}

static int nv_fermi_bit_find(struct nv_fermi_priv *priv, u8 id, struct nv_bit_token *out)
{
	size_t bit_off = nv_fermi_find_bit(priv);
	struct nv_bit_header hdr;
	size_t tokens_off;
	int i;

	if (!bit_off) {
		pr_err(DRV_NAME ": сигнатура BIT не найдена в VBIOS\n");
		return -ENOENT;
	}

	if (nv_fermi_parse_bit_header(priv, bit_off, &hdr, &tokens_off))
		return -EINVAL;

	pr_info(DRV_NAME ": BIT найден на офсете 0x%04zx, версия=%u.%u, "
		"header_size=%u token_size=%u token_count=%u\n",
		bit_off, hdr.bcd_version >> 8, hdr.bcd_version & 0xff,
		hdr.header_size, hdr.token_size, hdr.token_count);

	for (i = 0; i < hdr.token_count; i++) {
		size_t off = tokens_off + (size_t)i * hdr.token_size;
		const u8 *t;

		if (off + hdr.token_size > priv->vbios_len)
			break;
		t = priv->vbios + off;

		pr_info(DRV_NAME ":   BIT токен[%d]: id='%c' (0x%02x) version=%u length=%u offset=0x%04x\n",
			i, isprint(t[0]) ? t[0] : '?', t[0], t[1],
			t[2] | (t[3] << 8), t[4] | (t[5] << 8));

		if (t[0] == id) {
			out->id     = t[0];
			out->version = t[1];
			out->length = t[2] | (t[3] << 8);
			out->offset = t[4] | (t[5] << 8);
			return 0;
		}
	}

	return -ENOENT;
}

static int nv_fermi_parse_dcb(struct nv_fermi_priv *priv, struct nv_dcb_header *dcb)
{
	struct nv_bit_token bit_i;
	u16 dcb_off = 0;
	const u8 *p;

	if (nv_fermi_bit_find(priv, 'i', &bit_i) == 0 && bit_i.length >= 2 &&
	    (size_t)bit_i.offset + 2 <= priv->vbios_len) {
		dcb_off = priv->vbios[bit_i.offset] | (priv->vbios[bit_i.offset + 1] << 8);
		pr_info(DRV_NAME ": указатель DCB получен из BIT-токена 'i': 0x%04x\n", dcb_off);
	}

	if (!dcb_off && priv->vbios_len > 0x38) {
		dcb_off = priv->vbios[0x36] | (priv->vbios[0x37] << 8);
		pr_info(DRV_NAME ": BIT-токен 'i' не дал указателя, fallback на легаси-офсет 0x36: 0x%04x\n",
			dcb_off);
	}

	if (!dcb_off || (size_t)dcb_off + 4 > priv->vbios_len) {
		pr_err(DRV_NAME ": не удалось получить валидный офсет DCB\n");
		return -ENOENT;
	}

	p = priv->vbios + dcb_off;
	dcb->offset      = dcb_off;
	dcb->version     = p[0];
	dcb->header_len  = p[1];
	dcb->entry_count = p[2];
	dcb->entry_len   = p[3];

	pr_info(DRV_NAME ": DCB на офсете 0x%04x: версия=0x%02x header_len=%u entry_count=%u entry_len=%u\n",
		dcb->offset, dcb->version, dcb->header_len, dcb->entry_count, dcb->entry_len);

	if ((size_t)dcb_off + dcb->header_len <= priv->vbios_len) {
		pr_info(DRV_NAME ": DCB header raw dump (%u байт):\n", dcb->header_len);
		print_hex_dump(KERN_INFO, DRV_NAME ": dcb_hdr ", DUMP_PREFIX_OFFSET,
				16, 1, p, dcb->header_len, false);
	}

	if (dcb->entry_count && dcb->entry_len) {
		size_t entries_off = (size_t)dcb_off + dcb->header_len;
		size_t dump_len = min_t(size_t, (size_t)dcb->entry_count * dcb->entry_len, 8u * dcb->entry_len);

		if (entries_off + dump_len <= priv->vbios_len) {
			pr_info(DRV_NAME ": DCB entries raw dump (первые %zu байт из %u записей по %u байт):\n",
				dump_len, dcb->entry_count, dcb->entry_len);
			print_hex_dump(KERN_INFO, DRV_NAME ": dcb_ent ", DUMP_PREFIX_OFFSET,
					16, 1, priv->vbios + entries_off, dump_len, false);
		}
	}

	return 0;
}

static int nv_fermi_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct nv_fermi_priv *priv;
	u32 boot0;
	int ret;

	pr_info(DRV_NAME ": обнаружено устройство %04x:%04x (bus %s), revision=0x%02x\n",
		id->vendor, id->device, pci_name(pdev), pdev->revision);

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->pdev = pdev;

	ret = pci_enable_device(pdev);
	if (ret) {
		pr_err(DRV_NAME ": pci_enable_device() провалился, код %d\n", ret);
		return ret;
	}

	ret = pci_request_regions(pdev, DRV_NAME);
	if (ret) {
		pr_err(DRV_NAME ": pci_request_regions() провалился, код %d\n", ret);
		goto err_disable;
	}

	pci_set_master(pdev);

	priv->bar0_len = pci_resource_len(pdev, 0);
	priv->bar0 = pci_ioremap_bar(pdev, 0);
	if (!priv->bar0) {
		pr_err(DRV_NAME ": не удалось замаппить BAR0\n");
		ret = -ENOMEM;
		goto err_release;
	}
	pr_info(DRV_NAME ": BAR0 (MMIO) замаппен, размер=%llu байт\n",
		(unsigned long long)priv->bar0_len);

	priv->bar1_len = pci_resource_len(pdev, 1);
	priv->bar1 = pci_ioremap_bar(pdev, 1);
	if (!priv->bar1) {
		pr_err(DRV_NAME ": не удалось замаппить BAR1\n");
		ret = -ENOMEM;
		goto err_unmap_bar0;
	}
	pr_info(DRV_NAME ": BAR1 (VRAM) замаппен, размер=%llu байт\n",
		(unsigned long long)priv->bar1_len);

	boot0 = nv_rd32(priv, NV_PMC_BOOT_0);
	pr_info(DRV_NAME ": NV_PMC_BOOT_0 = 0x%08x (raw chip id)\n", boot0);
	pr_info(DRV_NAME ": chipset=0x%03x revision=0x%02x\n",
		NV_BOOT0_CHIPSET(boot0), NV_BOOT0_REVISION(boot0));

	if (NV_BOOT0_CHIPSET(boot0) == NV_CHIPSET_GF108) {
		pr_info(DRV_NAME ": chipset подтверждён как GF108 (GeForce GT 430)\n");
	} else {
		pr_warn(DRV_NAME ": chipset 0x%03x не совпадает с ожидаемым GF108 (0x%03x) — проверь железо стенда!\n",
			NV_BOOT0_CHIPSET(boot0), NV_CHIPSET_GF108);
	}

	pci_set_drvdata(pdev, priv);

	pr_info(DRV_NAME ": Фаза 2 завершена — устройство активно, MMIO доступен только на чтение\n");

	ret = nv_fermi_read_vbios(priv);
	if (ret) {
		pr_warn(DRV_NAME ": чтение VBIOS не удалось (код %d) — парсинг BIT/DCB пропущен\n", ret);
	} else {

		if (nv_fermi_parse_dcb(priv, &priv->dcb) == 0)
			priv->dcb_valid = true;
		else
			pr_warn(DRV_NAME ": парсинг DCB не удался — I2C/GPIO таблицы недоступны\n");
	}

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
	pr_info(DRV_NAME ": устройство %s освобождено\n", pci_name(pdev));
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

	pr_info(DRV_NAME ": загрузка модуля (Fermi/GF108 out-of-tree driver, Фаза 1)\n");

	ret = pci_register_driver(&nv_fermi_pci_driver);
	if (ret) {
		pr_err(DRV_NAME ": не удалось зарегистрировать pci_driver, код ошибки %d\n", ret);
		return ret;
	}

	pr_info(DRV_NAME ": pci_driver успешно зарегистрирован\n");
	return 0;
}

static void __exit nv_fermi_exit(void)
{
	pci_unregister_driver(&nv_fermi_pci_driver);
	pr_info(DRV_NAME ": модуль выгружен\n");
}

module_init(nv_fermi_init);
module_exit(nv_fermi_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("fsociety");
MODULE_DESCRIPTION("Out-of-tree DRM/KMS skeleton driver for NVIDIA Fermi GF108 (GT 430)");
MODULE_VERSION("0.1");#include <linux/module.h>
