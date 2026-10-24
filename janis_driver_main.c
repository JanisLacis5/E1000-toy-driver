#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/pci.h>
#include <linux/slab.h>

/*
   The driver is built for Intel's 82540EM Gigabit Ethernet Controller 
    Datasheet / manual: https://www.intel.com/content/dam/doc/manual/pci-pci-x-family-gbe-controllers-software-dev-manual.pdf
*/

#define DRIVER_NAME "janis-toy-driver"
#define JANIS_BAR_L 0
#define JANIS_BAR_H 1
#define JANIS_REG_SPACE_SIZE 0x20000 // 128kb

struct janis_private {
	void __iomem *regs;
};

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
	struct janis_private *priv;
	struct net_device *netdev;
	resource_size_t pciaddr;
	void __iomem *regs;
	int err;

	netdev = alloc_etherdev(sizeof(struct janis_private));
	if (!netdev)
		return -ENOMEM;
	netdev->netdev_ops = &janis_netdev_ops;
	priv = netdev_priv(netdev);

	SET_NETDEV_DEV(netdev, &pdev->dev);
	pci_set_drvdata(pdev, netdev);

	err = pci_enable_device(pdev);
	printk("pci_enable_device err: %d\n", err);
	if (err)
		goto err_clean_netdev;

	pci_set_master(pdev);

	err = pci_request_regions(pdev, DRIVER_NAME);
	printk("pci_request_regions err: %d\n", err);
	if (err)
		goto clear_master;

	/* gets they pyhsical addresses */
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
	priv->regs = regs;

	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	printk("dma_set_mask_and_coherent err: %d\n", err);
	if (err)
		goto release_region;

	printk("doing request_irq with name: %s", netdev->name);
	err = request_irq(pdev->irq, janis_irq, IRQF_SHARED, netdev->name,
			  netdev);
	printk("request_irq err: %d\n", err);
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
	struct janis_private *priv = netdev_priv(netdev);

	// TODO: stop interrupts from the device first
	free_irq(pdev->irq, netdev);
	// TODO: stop all dma operations from the device
	if (priv->regs)
		iounmap(priv->regs);
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
