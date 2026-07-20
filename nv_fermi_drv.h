#ifndef NV_FERMI_DRV_H
#define NV_FERMI_DRV_H

#include <linux/pci.h>
#include <linux/types.h>

#define DRV_NAME "nv_fermi_drv"

#define NV_VENDOR_ID_NVIDIA 0x10de
#define NV_DEVICE_ID_GF108 0x0de1

#define NV_PMC_BOOT_0 0x000000

#define NV_CHIPSET_GF108 0xC1

#define NV_BOOT0_CHIPSET(boot0) (((boot0) >> 20) & 0x1ff)
#define NV_BOOT0_REVISION(boot0) ((boot0) & 0xff)

struct nv_dcb_header {
  u16 offset;
  u8 version;
  u8 header_len;
  u8 entry_count;
  u8 entry_len;
};

struct nv_fermi_priv {
  struct pci_dev *pdev;
  void __iomem *bar0;
  void __iomem *bar1;
  resource_size_t bar0_len;
  resource_size_t bar1_len;

  u8 *vbios;
  size_t vbios_len;

  struct nv_dcb_header dcb;
  bool dcb_valid;
};

static inline u32 nv_rd32(struct nv_fermi_priv *priv, u32 reg) {
  return ioread32(priv->bar0 + reg);
}

static inline void nv_wr32(struct nv_fermi_priv *priv, u32 reg, u32 val) {
  iowrite32(val, priv->bar0 + reg);
}

int nv_fermi_read_vbios(struct nv_fermi_priv *priv);
int nv_fermi_parse_dcb(struct nv_fermi_priv *priv, struct nv_dcb_header *dcb);

int nv_fermi_i2c_init(struct nv_fermi_priv *priv);

#endif
