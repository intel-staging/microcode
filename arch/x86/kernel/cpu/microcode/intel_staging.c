// SPDX-License-Identifier: GPL-2.0-or-later

#define pr_fmt(fmt) "microcode: " fmt
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/pci_ids.h>

#include "internal.h"

#define MBOX_REG_NUM		4
#define MBOX_REG_SIZE		sizeof(u32)

#define MBOX_CONTROL_OFFSET	0x0
#define MBOX_STATUS_OFFSET	0x4
#define MBOX_WRDATA_OFFSET	0x8
#define MBOX_RDDATA_OFFSET	0xc

#define MASK_MBOX_CTRL_ABORT	BIT(0)
#define MASK_MBOX_CTRL_GO	BIT(31)

#define MASK_MBOX_STATUS_ERROR	BIT(2)
#define MASK_MBOX_STATUS_READY	BIT(31)

#define MASK_MBOX_RESP_SUCCESS	BIT(0)
#define MASK_MBOX_RESP_PROGRESS	BIT(1)
#define MASK_MBOX_RESP_ERROR	BIT(2)

#define MBOX_CMD_LOAD		0x3
#define MBOX_OBJ_STAGING	0xb
#define MBOX_HDR		(PCI_VENDOR_ID_INTEL | (MBOX_OBJ_STAGING << 16))
#define MBOX_HDR_SIZE		16

#define MBOX_XACTION_LEN	PAGE_SIZE
#define MBOX_XACTION_MAX(imgsz)	((imgsz) * 2)
#define MBOX_XACTION_TIMEOUT	(10 * MSEC_PER_SEC)

#define STAGING_OFFSET_END	0xffffffff
#define DWORD_SIZE(s)		((s) / sizeof(u32))

static inline u32 read_mbox_dword(void __iomem *base)
{
	u32 dword = readl(base + MBOX_RDDATA_OFFSET);

	/* Inform the read completion to the staging firmware */
	writel(0, base + MBOX_RDDATA_OFFSET);
	return dword;
}

static inline void write_mbox_dword(void __iomem *base, u32 dword)
{
	writel(dword, base + MBOX_WRDATA_OFFSET);
}

static inline void abort_xaction(void __iomem *base)
{
	writel(MASK_MBOX_CTRL_ABORT, base + MBOX_CONTROL_OFFSET);
}

static void request_xaction(void __iomem *base, u32 *chunk, unsigned int chunksize)
{
	unsigned int i, dwsize = DWORD_SIZE(chunksize);

	write_mbox_dword(base, MBOX_HDR);
	write_mbox_dword(base, dwsize + DWORD_SIZE(MBOX_HDR_SIZE));

	write_mbox_dword(base, MBOX_CMD_LOAD);
	write_mbox_dword(base, 0);

	for (i = 0; i < dwsize; i++)
		write_mbox_dword(base, chunk[i]);

	writel(MASK_MBOX_CTRL_GO, base + MBOX_CONTROL_OFFSET);
}

static enum ucode_state wait_for_xaction(void __iomem *base)
{
	u32 timeout, status;

	for (timeout = 0; timeout < MBOX_XACTION_TIMEOUT; timeout++) {
		msleep(1);
		status = readl(base + MBOX_STATUS_OFFSET);
		if (status & MASK_MBOX_STATUS_READY)
			break;
	}

	status = readl(base + MBOX_STATUS_OFFSET);
	if (status & MASK_MBOX_STATUS_ERROR)
		return UCODE_ERROR;
	if (!(status & MASK_MBOX_STATUS_READY))
		return UCODE_TIMEOUT;

	return UCODE_OK;
}

static enum ucode_state read_xaction_response(void __iomem *base, unsigned int *offset)
{
	u32 flag;

	WARN_ON_ONCE(read_mbox_dword(base) != MBOX_HDR);
	WARN_ON_ONCE(read_mbox_dword(base) != DWORD_SIZE(MBOX_HDR_SIZE));

	*offset = read_mbox_dword(base);

	flag = read_mbox_dword(base);
	if (flag & MASK_MBOX_RESP_ERROR)
		return UCODE_ERROR;

	return UCODE_OK;
}

static inline unsigned int get_chunksize(unsigned int totalsize, unsigned int offset)
{
	WARN_ON_ONCE(totalsize < offset);
	return min(MBOX_XACTION_LEN, totalsize - offset);
}

bool staging_work(u64 mmio_pa, void *ucode_ptr, unsigned int totalsize)
{
	unsigned int xaction_bytes = 0, offset = 0, chunksize;
	void __iomem *mmio_base;
	enum ucode_state state;

	mmio_base = ioremap(mmio_pa, MBOX_REG_NUM * MBOX_REG_SIZE);
	if (WARN_ON_ONCE(!mmio_base))
		return false;

	abort_xaction(mmio_base);

	while (offset != STAGING_OFFSET_END) {
		chunksize = get_chunksize(totalsize, offset);
		if (xaction_bytes + chunksize > MBOX_XACTION_MAX(totalsize)) {
			state = UCODE_TIMEOUT;
			break;
		}

		request_xaction(mmio_base, ucode_ptr + offset, chunksize);
		state = wait_for_xaction(mmio_base);
		if (state != UCODE_OK)
			break;

		xaction_bytes += chunksize;
		state = read_xaction_response(mmio_base, &offset);
		if (state != UCODE_OK)
			break;
	}

	iounmap(mmio_base);

	if (state == UCODE_OK)
		return true;

	pr_err("Staging failed with %s.\n", state == UCODE_TIMEOUT ? "timeout" : "error");
	return false;
}
