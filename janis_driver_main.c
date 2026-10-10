#include <linux/netdevice.h>
#include <linux/pci.h>

/*
   The driver is built for Intel's 82540EM Gigabit Ethernet Controller 
    Datasheet / manual: https://www.intel.com/content/dam/doc/manual/pci-pci-x-family-gbe-controllers-software-dev-manual.pdf
*/

static netdev_tx_t janis_xmit(struct sk_buff *skb, struct net_device *dev)
{
	printk("xmitting...");
	return NETDEV_TX_OK;
}

static const struct net_device_ops janis_netdev_ops = {
	.ndo_start_xmit = janis_xmit,
};

static int janis_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	printk("JANIS DRIVER PROBE");
	return 0;
}

static void janis_remove(struct pci_dev *dev)
{
	printk("JANIS DRIVER REMOVE");
}

const struct pci_device_id id_table[] = {
	{ PCI_VDEVICE(INTEL, 0x100E) },
};

static struct pci_driver janis_driver = {
	.name = "janis-toy-driver",
	.id_table = id_table,
	.probe = janis_probe,
	.remove = janis_remove,
};

module_pci_driver(janis_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Toy driver for Intel's e1000");
