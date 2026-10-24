#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/iopoll.h>

/*
   The driver is built for Intel's 82540EM Gigabit Ethernet Controller 
    Datasheet / manual: https://www.intel.com/content/dam/doc/manual/pci-pci-x-family-gbe-controllers-software-dev-manual.pdf
*/

#define DRIVER_NAME "janis-toy-driver"
#define JANIS_BAR_L 0
#define JANIS_BAR_H 1
#define JANIS_REG_SPACE_SIZE 0x20000 // 128kb

// TODO: define bitmasks to see indvidual bits in fields like init_ctrl etc

struct janis_private {
	void __iomem *regs;

	/* EEPROM data. noone outside the driver is allowed to update it. 
	   if it is needd for some reason, helpers should be implemented here */
	u8 mac_addr[ETH_ALEN];
	u16 compatability[4];
	u32 pba;
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

static int janis_read_eeprom_mac(void __iomem *base, u8 addr, u8 *ret)
{
	u16 words[3];
	int err;

	for (int i = 0; i < ETH_ALEN / 2; i++) {
		err = janis_read_eeprom_word(base, addr + i, &words[i]);
		if (err)
			return err;
	}

	for (int i = 0; i < ETH_ALEN / 2; i++) {
		ret[2 * i] = words[i] & 0xff;
		ret[2 * i + 1] = words[i] >> 8;
	}

	return 0;
}

static int janis_read_eeprom_ip4(void __iomem *base, u8 addr, __be32 *res)
{
	return 0;
}

static int janis_read_eeprom_ip6(void __iomem *base, u8 addr,
				 struct in6_addr *res)
{
	return 0;
}

static irqreturn_t janis_irq(int i, void *data)
{
	printk("janis_irq(%i, %p)\n", i, data);
	return IRQ_HANDLED;
}

static netdev_tx_t janis_xmit(struct sk_buff *skb, struct net_device *dev)
{
	printk("janis_xmit(%p, %p)\n", skb, dev);
	return NETDEV_TX_OK;
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
		goto clear_master;

	/* gets pyhsical addresses */
	pciaddr = pci_resource_start(pdev, JANIS_BAR_L);
	if (!pciaddr) {
		printk(KERN_ERR "no mmio resources");
		err = -EIO;
		goto clear_master;
	}
	if (pci_resource_len(pdev, JANIS_BAR_L) < JANIS_REG_SPACE_SIZE) {
		printk(KERN_ERR "not enough room for registers");
		err = -EIO;
		goto clear_master;
	}

	/* assigns virtual ones so they are accessable */
	regs = ioremap(pciaddr, JANIS_REG_SPACE_SIZE);
	if (!regs) {
		err = -EIO;
		printk(KERN_ERR "failed to map mmio");
		goto clear_master;
	}
	jpriv->regs = regs;

	/* populate eeprom data in private data */
	err = janis_read_eeprom_mac(regs, 0, jpriv->mac_addr);
	if (err)
		goto release_region;
	pr_info("eeprom MAC: %pM\n", jpriv->mac_addr);

	err = janis_read_eeprom_ip4(regs, 0x25, &jpriv->ipv4);
	if (err)
		goto release_region;
	pr_info("eeprom IPv4: %pI4\n", &jpriv->ipv4);

	err = janis_read_eeprom_ip6(regs, 0x27, &jpriv->ipv6);
	if (err)
		goto release_region;
	pr_info("eeprom IPv6: %pI6c\n", &jpriv->ipv6);

	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (err)
		goto release_region;

	err = request_irq(pdev->irq, janis_irq, IRQF_SHARED, netdev->name,
			  netdev);
	if (err)
		goto release_region;

	return 0;

release_region:
	iounmap(regs);
	pci_release_regions(pdev);
clear_master:
	pci_clear_master(pdev);
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
