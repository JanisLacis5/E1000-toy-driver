#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/pci.h>
#include <linux/slab.h>

/*
   The driver is built for Intel's 82540EM Gigabit Ethernet Controller 
    Datasheet / manual: https://www.intel.com/content/dam/doc/manual/pci-pci-x-family-gbe-controllers-software-dev-manual.pdf
*/

#define DRIVER_NAME "janis-toy-driver"

struct janis_private {
	struct net_device *netdev;
};

static irqreturn_t janis_irq(int i, void *data)
{
	printk("janis_irq(%i, %p)", i, data);
	return IRQ_HANDLED;
}

static netdev_tx_t janis_xmit(struct sk_buff *skb, struct net_device *dev)
{
	printk("janis_xmit(%p, %p)", skb, dev);
	return NETDEV_TX_OK;
}

static int janis_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	printk("janis_probe(%p, %p)", pdev, id);
	struct net_device *netdev;
	int err;

	netdev = alloc_etherdev(sizeof(struct janis_private));
	if (!netdev)
		return -ENOMEM;

	SET_NETDEV_DEV(netdev, &pdev->dev);
	pci_set_drvdata(pdev, netdev);

	err = pci_enable_device(pdev);
	printk("pci_enable_device err: %d", err);
	if (err)
		goto err_clean_netdev;

	pci_set_master(pdev);

	err = pci_request_regions(pdev, DRIVER_NAME);
	printk("pci_request_regions err: %d", err);
	if (err)
		goto clear_master;

	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	printk("dma_set_mask_and_coherent err: %d", err);
	if (err)
		goto release_region;

	// TODO: maybe some registers on the nic need to be initialized / reset or smth like that (io mappings for example)

	printk("before request_irq");
	err = request_irq(pdev->irq, janis_irq, IRQF_SHARED, DRIVER_NAME,
			  netdev);
	printk("request_irq err: %d", err);
	if (err)
		goto release_region;

	return 0;

release_region:
	printk("goto release_region");
	pci_release_regions(pdev);
clear_master:
	printk("goto clear_master");
	pci_clear_master(pdev);
err_clean_netdev:
	printk("goto err_clean_netdev");
	free_netdev(netdev);
	return err;
}

static void janis_remove(struct pci_dev *pdev)
{
	printk("janis_remove(%p)", pdev);
	struct net_device *netdev = pci_get_drvdata(pdev);

	// TODO: stop interrupts from the device first
	free_irq(pdev->irq, pdev);
	// TODO: stop all dma operations from the device
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	free_netdev(netdev);
}

static const struct net_device_ops janis_netdev_ops = {
	.ndo_start_xmit = janis_xmit,
};

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
