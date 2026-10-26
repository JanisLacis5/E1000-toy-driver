#include "linux/if_ether.h"
#include "linux/in6.h"
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/iopoll.h>
#include <linux/string.h>

/*
   The driver is built for Intel's 82540EM Gigabit Ethernet Controller 
    Datasheet / manual: https://www.intel.com/content/dam/doc/manual/pci-pci-x-family-gbe-controllers-software-dev-manual.pdf
*/

#define DRIVER_NAME "janis-toy-driver"
#define JANIS_BAR_L 0
#define JANIS_BAR_H 1
#define JANIS_MAX_MTU 1500
#define JANIS_TMP_BUF_SIZE 256
#define JANIS_REG_SPACE_SIZE 0x20000 // 128kb
#define JANIS_EEPROM_SIZE_WORDS 0x100

// TODO: define bitmasks to see indvidual bits in fields like init_ctrl etc

struct rx_desc {
	__le64 addr;
	__le16 len;
	__le16 checksum;
	u8 status;
	u8 errors;
	__le16 special;
};

struct janis_private {
	void __iomem *regs;

	/* EEPROM data. noone outside the driver is allowed to update it. 
	   if it is needd for some reason, helpers should be implemented here */
	u8 mac_addr[ETH_ALEN];
	u16 compatability[4];
	u32 pba; // [3rd byte][4th byte][1st byte][2nd byte]
	u16 init_ctrl1;
	u16 init_ctrl2;
	u8 common_pwr;
	u16 circuit_ctrl;
	u8 d3_pwr;
	u8 d0_pwr;
	u16 mgmt_ctrl;
	u8 smbus_addr;
	u8 init_ctrl3;
	__be32 ipv4;
	struct in6_addr ipv6;
	u8 led_ctrl;
	u16 checksum;

	dma_addr_t dma_handle;
};

#define read8(pos) readb((jpriv->regs) + (pos))
#define read16(pos) readw((jpriv->regs) + (pos))
#define read32(pos) readl((jpriv->regs) + (pos))
#define write8(pos) writeb((jpriv->regs) + (pos))
#define write16(pos) writew((jpriv->regs) + (pos))
#define write32(pos) writel((jpriv->regs) + (pos))

enum regs {
	REG_EECD = 0x10,
	REG_EERD = 0x14,
};

static int janis_read_eeprom_word(void __iomem *base, u8 addr, u16 *ret)
{
	u32 eerd_value;

	u32 to_write = (addr << 8) | 1;
	writel(to_write, base + REG_EERD);

	if (readl_poll_timeout(base + REG_EERD, eerd_value,
			       eerd_value & (1u << 4), 0, 100))
		return -ETIMEDOUT;

	*ret = eerd_value >> 0x10;
	return 0;
}

static int janis_eeprom_to_priv(void __iomem *base, struct janis_private *jpriv)
{
	u16 eeprom[JANIS_EEPROM_SIZE_WORDS];
	u8 *ip4 = (u8 *)&jpriv->ipv4;
	int err;

	for (int i = 0; i < JANIS_EEPROM_SIZE_WORDS; i++) {
		err = janis_read_eeprom_word(base, i, &eeprom[i]);
		if (err)
			return err;
	}

	for (int i = 0; i < ETH_ALEN / 2; i++) {
		jpriv->mac_addr[2 * i] = eeprom[i] & 0xff;
		jpriv->mac_addr[2 * i + 1] = eeprom[i] >> 8;
	}
	memcpy(jpriv->compatability, &eeprom[0x03], 4);
	jpriv->pba = (eeprom[0x09] << 16) | eeprom[0x08];
	jpriv->init_ctrl1 = eeprom[0x0A];
	jpriv->init_ctrl2 = eeprom[0x0F];
	jpriv->common_pwr = eeprom[0x12];
	jpriv->circuit_ctrl = eeprom[0x21];
	jpriv->d3_pwr = eeprom[0x22] & 0xff;
	jpriv->d0_pwr = eeprom[0x22] >> 8;
	jpriv->mgmt_ctrl = eeprom[0x23];
	jpriv->smbus_addr = eeprom[0x24] & 0xff;
	jpriv->init_ctrl3 = eeprom[0x24] >> 8;
	for (int i = 0; i < sizeof(__be32) / 2; i++) {
		ip4[2 * i] = eeprom[0x25 + i] & 0xff;
		ip4[2 * i + 1] = (eeprom[0x25 + i] >> 8);
	}
	for (int i = 0; i < sizeof(jpriv->ipv6) / 2; i++) {
		jpriv->ipv6.s6_addr[2 * i] = eeprom[0x27 + i] & 0xff;
		jpriv->ipv6.s6_addr[2 * i + 1] = (eeprom[0x27 + i] >> 8);
	}
	jpriv->led_ctrl = eeprom[0x2F];
	jpriv->checksum = 0; // TODO

	return 0;
}

static irqreturn_t janis_irq(int i, void *data)
{
	printk("janis_irq(%i, %p)\n", i, data);
	return IRQ_NONE;
}

static netdev_tx_t janis_xmit(struct sk_buff *skb, struct net_device *dev)
{
	printk("janis_xmit(%p, %p)\n", skb, dev);
	return NETDEV_TX_BUSY;
}

static const struct net_device_ops janis_netdev_ops = {
	.ndo_start_xmit = janis_xmit,
};

static int janis_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	printk("janis_probe(%p, %p)\n", pdev, id);
	struct janis_private *jpriv;
	struct net_device *netdev;
	resource_size_t pciaddr;
	void __iomem *regs;
	int err;

	netdev = alloc_etherdev(sizeof(struct janis_private));
	if (!netdev)
		return -ENOMEM;
	netdev->netdev_ops = &janis_netdev_ops;
	jpriv = netdev_priv(netdev);

	SET_NETDEV_DEV(netdev, &pdev->dev);
	pci_set_drvdata(pdev, netdev);

	err = pci_enable_device(pdev);
	if (err)
		goto err_clean_netdev;

	pci_set_master(pdev);

	err = pci_request_regions(pdev, DRIVER_NAME);
	if (err)
		goto disable_dev;

	/* gets pyhsical addresses */
	pciaddr = pci_resource_start(pdev, JANIS_BAR_L);
	if (!pciaddr) {
		printk(KERN_ERR "no mmio resources");
		err = -EIO;
		goto release_region;
	}
	if (pci_resource_len(pdev, JANIS_BAR_L) < JANIS_REG_SPACE_SIZE) {
		printk(KERN_ERR "not enough room for registers");
		err = -EIO;
		goto release_region;
	}

	/* assigns virtual ones so they are accessable */
	regs = ioremap(pciaddr, JANIS_REG_SPACE_SIZE);
	if (!regs) {
		err = -EIO;
		printk(KERN_ERR "failed to map mmio");
		goto release_region;
	}
	jpriv->regs = regs;

	err = janis_eeprom_to_priv(regs, jpriv);
	if (err)
		goto io_unmap;

	/* set up net device */
	eth_hw_addr_set(netdev, jpriv->mac_addr);
	netdev->min_mtu = ETH_MIN_MTU;
	netdev->max_mtu = JANIS_MAX_MTU;

	err = register_netdev(netdev);
	if (err)
		goto io_unmap;

	/* set up dma stuff (TODO) */
	// see https://docs.kernel.org/core-api/dma-api-howto.html
	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (err) {
		dev_warn(&pdev->dev,
			 "janis_e1000: No suitable DMA available\n");
		goto io_unmap;
	}

	/* set up interrups (TODO) */
	err = request_irq(pdev->irq, janis_irq, IRQF_SHARED, netdev->name,
			  netdev);
	if (err)
		goto io_unmap;

	return 0;

io_unmap:
	iounmap(regs);
release_region:
	pci_release_regions(pdev);
disable_dev:
	pci_clear_master(pdev);
	pci_disable_device(pdev);
err_clean_netdev:
	free_netdev(netdev);
	return err;
}

static void janis_remove(struct pci_dev *pdev)
{
	printk("janis_remove(%p)\n", pdev);
	struct net_device *netdev = pci_get_drvdata(pdev);
	struct janis_private *jpriv = netdev_priv(netdev);

	// TODO: stop interrupts from the device first
	free_irq(pdev->irq, netdev);
	// TODO: stop all dma operations from the device
	if (jpriv->regs)
		iounmap(jpriv->regs);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	free_netdev(netdev);
}

const struct pci_device_id id_table[] = {
	{ PCI_VDEVICE(INTEL, 0x100E) },
};

static struct pci_driver janis_driver = {
	.name = DRIVER_NAME,
	.id_table = id_table,
	.probe = janis_probe,
	.remove = janis_remove,
};

module_pci_driver(janis_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Toy driver for Intel's e1000");
