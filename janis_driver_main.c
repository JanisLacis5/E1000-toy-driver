#include "linux/if_ether.h"
#include "linux/in6.h"
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/pci.h>
#include <linux/mii.h>
#include <linux/slab.h>
#include <linux/iopoll.h>
#include <linux/string.h>

/*
   The driver is built for Intel's 82540EM Gigabit Ethernet Controller 
    Datasheet / manual: https://www.intel.com/content/dam/doc/manual/pci-pci-x-family-gbe-controllers-software-dev-manual.pdf
*/

#define DRIVER_NAME "janis-toy-driver"

#define JANIS_BAR		0
#define JANIS_MAX_MTU		1500
#define JANIS_TMP_BUF_SIZE	256
#define JANIS_REG_SPACE_SIZE	0x20000 // 128kb
#define JANIS_EEPROM_SIZE_WORDS 0x100

#define JANIS_RXDESC_STATUS_PIF	  BIT(7)
#define JANIS_RXDESC_STATUS_IPCS  BIT(6)
#define JANIS_RXDESC_STATUS_TCPCS BIT(5)
#define JANIS_RXDESC_STATUS_RSV	  BIT(4)
#define JANIS_RXDESC_STATUS_VP	  BIT(3)
#define JANIS_RXDESC_STATUS_IXSM  BIT(2)
#define JANIS_RXDESC_STATUS_EOP	  BIT(1)
#define JANIS_RXDESC_STATUS_DD	  BIT(0)
struct janis_rx_desc {
	__le64 addr;
	__le16 len;
	__le16 checksum;
	u8 status;
	u8 errors;
	__le16 special;
};

// TODO: define bitmasks to see indvidual bits in fields like init_ctrl etc
#define JANIS_RX_RING_SIZE 64
#define JANIS_RX_BUF_SIZE  JANIS_MAX_MTU + ETH_HLEN + 8
struct janis_private {
	struct net_device *netdev;
	struct pci_dev *pdev;
	void __iomem *regs;
	u32 msg_enable;

	struct sk_buff *rx_skb[JANIS_RX_RING_SIZE];
	struct janis_rx_desc *rx_ring;
	int rx_ring_size;
	u32 rx_tail;

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
	struct mii_if_info mii_if;
};

#define RX_TAIL_NEXT(curr) ((curr + 1) % JANIS_RX_RING_SIZE)

#define read8(pos)	  readb((jpriv->regs) + (pos))
#define read16(pos)	  readw((jpriv->regs) + (pos))
#define read32(pos)	  readl((jpriv->regs) + (pos))
#define write8(val, pos)  writeb((val), (jpriv->regs) + (pos))
#define write16(val, pos) writew((val), (jpriv->regs) + (pos))
#define write32(val, pos) writel((val), (jpriv->regs) + (pos))

enum regs {
	REG_CTRL = 0x0,
	REG_STATUS = 0x8,
	REG_EERD = 0x14,
	REG_ICR = 0xC0,
	REG_IMS = 0xD0,
	REG_RCTL = 0x100,
	REG_RDBAL = 0x2800,
	REG_RDBAH = 0x2804,
	REG_RDLEN = 0x2808,
	REG_RDH = 0x2810,
	REG_RDT = 0x2818,
	REG_RAL = 0x5400,
	REG_RAH = 0x5404,
};

enum irqs {
	IRQ_TXDW = BIT(0),
	IRQ_RXDMT0 = BIT(4),
	IRQ_RX0 = BIT(6),
	IRQ_RXT0 = BIT(7),
	IRQ_SRPD = BIT(16),
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
	struct net_device *netdev = data;
	struct janis_private *jpriv = netdev_priv(netdev);
	u32 rx_tail = jpriv->rx_tail;
	struct janis_rx_desc *desc;
	struct sk_buff *skb;

	u32 reason = read32(REG_ICR);
	printk("Interrupt reason: %08X\n", reason);

	u32 status = read32(REG_STATUS);
	printk("device status: %08X\n", status);

	desc = &jpriv->rx_ring[rx_tail];
	printk("curr desc len: %i\n", desc->len);
	printk("curr desc status: %02X\n", desc->status);
	printk("curr desc errors: %02X\n", desc->errors);
	printk("curr desc special: %04X\n", desc->special);

	skb = jpriv->rx_skb[rx_tail];
	printk("skb len: %i\n", skb->len);
	printk("skb data len: %i\n", skb->data_len);

	jpriv->rx_tail = RX_TAIL_NEXT(rx_tail);
	return IRQ_HANDLED;
}

static netdev_tx_t janis_xmit(struct sk_buff *skb, struct net_device *dev)
{
	return NETDEV_TX_OK;
}

static const struct net_device_ops janis_netdev_ops = {
	.ndo_start_xmit = janis_xmit,
};

static void janis_setup_irqs(struct janis_private *jpriv)
{
	u32 ims = 0;
	ims |= IRQ_RXT0 | IRQ_SRPD | IRQ_RXDMT0 | IRQ_RX0;
	write32(ims, REG_IMS);
}

static void janis_setup_regs(struct janis_private *jpriv)
{
	u32 macl = 0;
	u16 mach = jpriv->mac_addr[4] | (jpriv->mac_addr[5] << 8);
	for (int i = 0; i < 4; i++) {
		macl |= (u32)jpriv->mac_addr[i] << (8 * i);
	}
	u32 ctrl = read32(REG_CTRL);

	write32(0b11110, REG_RCTL);
	write32(0x0, REG_RDH);
	write32(0x1, REG_RDT);
	write32(macl, REG_RAL);
	write32(mach, REG_RAH);
	write32(jpriv->dma_handle & 0xffffffff, REG_RDBAL);
	write32(jpriv->dma_handle >> 32, REG_RDBAH);
	write32(jpriv->rx_ring_size, REG_RDLEN);
	write32(ctrl | BIT(6), REG_CTRL);
}

static int janis_init_rings(struct janis_private *jpriv)
{
	struct net_device *netdev = jpriv->netdev;
	struct device *dev = &jpriv->pdev->dev;
	int err = -ENOMEM;

	void *mem = dma_alloc_coherent(dev, jpriv->rx_ring_size,
				       &jpriv->dma_handle, GFP_KERNEL);
	if (!mem)
		return -ENOMEM;

	jpriv->rx_ring = mem;
	memset(jpriv->rx_ring, 0, jpriv->rx_ring_size);

	for (int i = 0; i < JANIS_RX_RING_SIZE; i++) {
		struct sk_buff *skb;
		dma_addr_t mapping;

		skb = netdev_alloc_skb_ip_align(netdev, JANIS_RX_BUF_SIZE);
		if (!skb)
			goto err_done;

		mapping = dma_map_single(dev, skb->data, JANIS_RX_BUF_SIZE,
					 DMA_FROM_DEVICE);
		if (dma_mapping_error(dev, mapping)) {
			kfree_skb(skb);
			goto err_done;
		}

		jpriv->rx_skb[i] = skb;
		jpriv->rx_ring[i].addr = cpu_to_le64(mapping);
	}

	return 0;

err_done:
	dma_free_coherent(dev, jpriv->rx_ring_size, jpriv->rx_ring,
			  jpriv->dma_handle);
	return err;
}

static int janis_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
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
	netif_carrier_off(netdev);

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
	pciaddr = pci_resource_start(pdev, JANIS_BAR);
	if (!pciaddr) {
		printk(KERN_ERR "no mmio resources");
		err = -EIO;
		goto release_region;
	}
	if (pci_resource_len(pdev, JANIS_BAR) < JANIS_REG_SPACE_SIZE) {
		printk(KERN_ERR "not enough room for registers");
		err = -EIO;
		goto release_region;
	}

	/* assigns virtual ones so they are accessible */
	regs = ioremap(pciaddr, JANIS_REG_SPACE_SIZE);
	if (!regs) {
		err = -EIO;
		printk(KERN_ERR "failed to map mmio");
		goto release_region;
	}
	jpriv->regs = regs;
	jpriv->netdev = netdev;
	jpriv->pdev = pdev;
	jpriv->rx_ring_size = JANIS_RX_RING_SIZE * sizeof(struct janis_rx_desc);
	jpriv->rx_tail = 0;

	err = janis_eeprom_to_priv(regs, jpriv);
	if (err)
		goto io_unmap;

	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (err) {
		dev_warn(&pdev->dev,
			 "janis_e1000: No suitable DMA available\n");
		goto io_unmap;
	}

	/* set up net device */
	eth_hw_addr_set(netdev, jpriv->mac_addr);
	netdev->min_mtu = ETH_MIN_MTU;
	netdev->max_mtu = JANIS_MAX_MTU;

	err = register_netdev(netdev);
	if (err)
		goto io_unmap;

	err = request_irq(pdev->irq, janis_irq, IRQF_SHARED, netdev->name,
			  netdev);
	if (err)
		goto io_unmap;

	if (janis_init_rings(jpriv))
		goto io_unmap;
	janis_setup_regs(jpriv);
	janis_setup_irqs(jpriv);

	// TODO: do this when phy reports link up not like this
	netif_carrier_on(netdev);

	/* TODO: set up mii_if_info struct in jpriv */
	// mii_check_media(&jpriv->mii_if, netif_msg_link(jpriv), true);
	// netif_start_queue(netdev);

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
	struct net_device *netdev = pci_get_drvdata(pdev);
	struct janis_private *jpriv = netdev_priv(netdev);

	write32(0, REG_IMS);
	unregister_netdev(netdev);
	free_irq(pdev->irq, netdev);
	// TODO: stop all dma operations from the device
	iounmap(jpriv->regs);
	// dma_free_coherent(&pdev->dev, jpriv->rx_ring_size, jpriv->rx_ring,
	// 		  jpriv->dma_handle);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	free_netdev(netdev);
}

const struct pci_device_id id_table[] = {
	{ PCI_VDEVICE(INTEL, 0x100E) },
	{},
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
