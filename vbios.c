#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/ctype.h>

#include "nv_fermi_drv.h"

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

#define NV_BIT_MIN_HEADER_SIZE (NV_BIT_SIGNATURE_LEN + 6)
#define NV_BIT_MIN_TOKEN_SIZE 6

int nv_fermi_read_vbios(struct nv_fermi_priv *priv)
{
	struct pci_dev *pdev = priv->pdev;
	void __iomem *rom;
	size_t rom_size;

	rom = pci_map_rom(pdev, &rom_size);
	if (!rom || !rom_size) {
		pr_err(DRV_NAME ": failed to map the expansion ROM (VBIOS)\n");
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
		pr_warn(DRV_NAME ": VBIOS signature 0x55 0xAA not found (got 0x%02x 0x%02x) -- ROM may not be directly accessible without an ACPI/legacy fallback\n",
			priv->vbios_len >= 1 ? priv->vbios[0] : 0,
			priv->vbios_len >= 2 ? priv->vbios[1] : 0);
		return -EINVAL;
	}

	pr_info(DRV_NAME ": VBIOS read, size=%zu bytes, signature valid\n",
		priv->vbios_len);
	return 0;
}

static int nv_fermi_find_bit(struct nv_fermi_priv *priv, size_t *out_off)
{
	size_t i;

	if (priv->vbios_len < NV_BIT_SIGNATURE_LEN)
		return -ENOENT;

	for (i = 0; i <= priv->vbios_len - NV_BIT_SIGNATURE_LEN; i++) {
		if (memcmp(priv->vbios + i, nv_bit_signature, NV_BIT_SIGNATURE_LEN) == 0) {
			*out_off = i;
			return 0;
		}
	}
	return -ENOENT;
}

static int nv_fermi_parse_bit_header(struct nv_fermi_priv *priv, size_t bit_sig_off,
				      struct nv_bit_header *hdr, size_t *tokens_off)
{
	const u8 *p = priv->vbios + bit_sig_off + NV_BIT_SIGNATURE_LEN;
	u8 sum = 0;
	size_t i;

	if (bit_sig_off + NV_BIT_SIGNATURE_LEN + 6 > priv->vbios_len)
		return -EINVAL;

	hdr->bcd_version = p[0] | (p[1] << 8);
	hdr->header_size = p[2];
	hdr->token_size  = p[3];
	hdr->token_count = p[4];
	hdr->checksum    = p[5];

	if (hdr->header_size < NV_BIT_MIN_HEADER_SIZE) {
		pr_err(DRV_NAME ": BIT header_size=%u is below the minimum of %u -- corrupted image or unknown format\n",
			hdr->header_size, NV_BIT_MIN_HEADER_SIZE);
		return -EINVAL;
	}
	if (hdr->token_size < NV_BIT_MIN_TOKEN_SIZE) {
		pr_err(DRV_NAME ": BIT token_size=%u is below the minimum of %u -- corrupted image or unknown format\n",
			hdr->token_size, NV_BIT_MIN_TOKEN_SIZE);
		return -EINVAL;
	}
	if (bit_sig_off + hdr->header_size > priv->vbios_len) {
		pr_err(DRV_NAME ": BIT header_size=%u runs past the end of the VBIOS (len=%zu)\n",
			hdr->header_size, priv->vbios_len);
		return -EINVAL;
	}

	for (i = 0; i < hdr->header_size; i++)
		sum += priv->vbios[bit_sig_off + i];
	if (sum != 0)
		pr_warn(DRV_NAME ": BIT header checksum does not add up (sum=0x%02x, expected 0x00) -- data may be corrupted\n",
			sum);

	*tokens_off = bit_sig_off + hdr->header_size;
	return 0;
}

static int nv_fermi_bit_find(struct nv_fermi_priv *priv, u8 id, struct nv_bit_token *out)
{
	struct nv_bit_header hdr;
	size_t bit_off;
	size_t tokens_off;
	int i;

	if (nv_fermi_find_bit(priv, &bit_off)) {
		pr_err(DRV_NAME ": BIT signature not found in VBIOS\n");
		return -ENOENT;
	}

	if (nv_fermi_parse_bit_header(priv, bit_off, &hdr, &tokens_off))
		return -EINVAL;

	pr_info(DRV_NAME ": BIT found at offset 0x%04zx, version=%u.%u, "
		"header_size=%u token_size=%u token_count=%u\n",
		bit_off, hdr.bcd_version >> 8, hdr.bcd_version & 0xff,
		hdr.header_size, hdr.token_size, hdr.token_count);

	for (i = 0; i < hdr.token_count; i++) {
		size_t off = tokens_off + (size_t)i * hdr.token_size;
		const u8 *t;

		if (off + NV_BIT_MIN_TOKEN_SIZE > priv->vbios_len) {
			pr_warn(DRV_NAME ": token[%d] runs past the end of the VBIOS, stopping the scan\n", i);
			break;
		}
		t = priv->vbios + off;

		pr_info(DRV_NAME ":   BIT token[%d]: id='%c' (0x%02x) version=%u length=%u offset=0x%04x\n",
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

static bool nv_fermi_dcb_header_plausible(const u8 *p)
{
	u8 version = p[0];
	u8 header_len = p[1];
	u8 entry_count = p[2];
	u8 entry_len = p[3];

	if (version < 0x20 || version > 0x50)
		return false;
	if (header_len < 4 || header_len > 64)
		return false;
	if (entry_count < 1 || entry_count > 32)
		return false;
	if (entry_len != 1 && entry_len != 2 && entry_len != 4 &&
	    entry_len != 8 && entry_len != 10)
		return false;
	return true;
}

static void nv_fermi_scan_dcb_candidates(struct nv_fermi_priv *priv, struct nv_bit_token *bit_i)
{
	size_t i;
	size_t dump_len = min_t(size_t, (size_t)bit_i->length, 64);

	pr_info(DRV_NAME ": BIT 'i' token: version=%u length=%u offset=0x%04x\n",
		bit_i->version, bit_i->length, bit_i->offset);

	if ((size_t)bit_i->offset + dump_len <= priv->vbios_len) {
		pr_info(DRV_NAME ": BIT 'i' table raw dump (%zu bytes at 0x%04x):\n",
			dump_len, bit_i->offset);
		print_hex_dump(KERN_INFO, DRV_NAME ": bit_i ", DUMP_PREFIX_OFFSET,
				16, 1, priv->vbios + bit_i->offset, dump_len, false);
	}

	for (i = 0; i + 2 <= dump_len; i += 2) {
		size_t off = bit_i->offset + i;
		u16 candidate;
		const u8 *p;

		if (off + 2 > priv->vbios_len)
			break;
		candidate = priv->vbios[off] | (priv->vbios[off + 1] << 8);

		if (!candidate || (size_t)candidate + 4 > priv->vbios_len)
			continue;

		p = priv->vbios + candidate;
		pr_info(DRV_NAME ": candidate @ bit_i+0x%02zx -> 0x%04x: version=0x%02x header_len=%u entry_count=%u entry_len=%u %s\n",
			i, candidate, p[0], p[1], p[2], p[3],
			nv_fermi_dcb_header_plausible(p) ? "PLAUSIBLE" : "");
	}

	if (priv->vbios_len > 0x38) {
		u16 legacy = priv->vbios[0x36] | (priv->vbios[0x37] << 8);

		if (legacy && (size_t)legacy + 4 <= priv->vbios_len) {
			const u8 *p = priv->vbios + legacy;

			pr_info(DRV_NAME ": legacy candidate @ 0x36 -> 0x%04x: version=0x%02x header_len=%u entry_count=%u entry_len=%u %s\n",
				legacy, p[0], p[1], p[2], p[3],
				nv_fermi_dcb_header_plausible(p) ? "PLAUSIBLE" : "");
		}
	}
}

int nv_fermi_parse_dcb(struct nv_fermi_priv *priv, struct nv_dcb_header *dcb)
{
	struct nv_bit_token bit_i;
	u16 bit_i_off = 0;
	u16 legacy_off = 0;
	u16 dcb_off = 0;
	bool bit_i_ok = false;
	bool legacy_ok = false;
	const u8 *p;

	if (nv_fermi_bit_find(priv, 'i', &bit_i) == 0) {
		nv_fermi_scan_dcb_candidates(priv, &bit_i);

		if (bit_i.length >= 2 && (size_t)bit_i.offset + 2 <= priv->vbios_len) {
			bit_i_off = priv->vbios[bit_i.offset] | (priv->vbios[bit_i.offset + 1] << 8);
			if (bit_i_off && (size_t)bit_i_off + 4 <= priv->vbios_len)
				bit_i_ok = nv_fermi_dcb_header_plausible(priv->vbios + bit_i_off);
		}
	}

	if (priv->vbios_len > 0x38) {
		legacy_off = priv->vbios[0x36] | (priv->vbios[0x37] << 8);
		if (legacy_off && (size_t)legacy_off + 4 <= priv->vbios_len)
			legacy_ok = nv_fermi_dcb_header_plausible(priv->vbios + legacy_off);
	}

	pr_info(DRV_NAME ": DCB candidate from BIT 'i': 0x%04x (%s), legacy 0x36: 0x%04x (%s)\n",
		bit_i_off, bit_i_ok ? "plausible" : "not plausible",
		legacy_off, legacy_ok ? "plausible" : "not plausible");

	if (bit_i_ok) {
		dcb_off = bit_i_off;
		pr_info(DRV_NAME ": using DCB offset from BIT 'i' (plausible header)\n");
	} else if (legacy_ok) {
		dcb_off = legacy_off;
		pr_info(DRV_NAME ": BIT 'i' pointer not plausible, using legacy offset 0x36 instead\n");
	}

	if (!dcb_off || (size_t)dcb_off + 4 > priv->vbios_len) {
		pr_err(DRV_NAME ": could not obtain a valid DCB offset\n");
		return -ENOENT;
	}

	p = priv->vbios + dcb_off;
	dcb->offset      = dcb_off;
	dcb->version     = p[0];
	dcb->header_len  = p[1];
	dcb->entry_count = p[2];
	dcb->entry_len   = p[3];

	pr_info(DRV_NAME ": DCB at offset 0x%04x: version=0x%02x header_len=%u entry_count=%u entry_len=%u\n",
		dcb->offset, dcb->version, dcb->header_len, dcb->entry_count, dcb->entry_len);

	if ((size_t)dcb_off + dcb->header_len <= priv->vbios_len) {
		pr_info(DRV_NAME ": DCB header raw dump (%u bytes):\n", dcb->header_len);
		print_hex_dump(KERN_INFO, DRV_NAME ": dcb_hdr ", DUMP_PREFIX_OFFSET,
				16, 1, p, dcb->header_len, false);
	}

	if (dcb->header_len >= 8) {
		dcb->i2c_table_off = p[4] | (p[5] << 8);

		pr_info(DRV_NAME ": DCB i2c_table_off=0x%04x (confirmed)\n", dcb->i2c_table_off);

		if ((size_t)dcb->i2c_table_off + 32 <= priv->vbios_len) {
			pr_info(DRV_NAME ": I2C table raw dump (32 bytes at 0x%04x):\n",
				dcb->i2c_table_off);
			print_hex_dump(KERN_INFO, DRV_NAME ": i2c_tbl ", DUMP_PREFIX_OFFSET,
					16, 1, priv->vbios + dcb->i2c_table_off, 32, false);
		} else {
			pr_warn(DRV_NAME ": i2c_table_off=0x%04x out of bounds, skipping dump\n",
				dcb->i2c_table_off);
		}

		pr_info(DRV_NAME ": header+0x06 (0x%04x) is confirmed NOT the GPIO table (executable code) -- scanning the rest of the header for candidates\n",
			(unsigned)(p[6] | (p[7] << 8)));

		{
			size_t off;

			for (off = 8; off + 2 <= dcb->header_len; off += 2) {
				u16 candidate = p[off] | (p[off + 1] << 8);
				const u8 *cp;

				if (!candidate || (size_t)candidate + 4 > priv->vbios_len)
					continue;

				cp = priv->vbios + candidate;
				pr_info(DRV_NAME ": gpio candidate @ dcb_hdr+0x%02zx -> 0x%04x: %02x %02x %02x %02x %s\n",
					off, candidate, cp[0], cp[1], cp[2], cp[3],
					nv_fermi_dcb_header_plausible(cp) ? "PLAUSIBLE" : "");
			}
		}
	} else {
		pr_warn(DRV_NAME ": DCB header_len=%u too small to contain i2c/gpio pointers\n",
			dcb->header_len);
	}

	if (dcb->entry_count && dcb->entry_len) {
		size_t entries_off = (size_t)dcb_off + dcb->header_len;
		size_t dump_len = min_t(size_t, (size_t)dcb->entry_count * dcb->entry_len, 8u * dcb->entry_len);

		if (entries_off + dump_len <= priv->vbios_len) {
			pr_info(DRV_NAME ": DCB entries raw dump (first %zu bytes out of %u entries of %u bytes each):\n",
				dump_len, dcb->entry_count, dcb->entry_len);
			print_hex_dump(KERN_INFO, DRV_NAME ": dcb_ent ", DUMP_PREFIX_OFFSET,
					16, 1, priv->vbios + entries_off, dump_len, false);
		}
	}

	return 0;
}