/*
 * UIO Hilscher NetX card driver
 *
 * (C) 2007 Hans J. Koch <hjk@linutronix.de>
 * (C) 2013 Sebastian Doell <sdoell@hilscher.com> Added DMA Support
 *
 * Licensed under GPL version 2 only.
 *
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/uio_driver.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <asm/cacheflush.h>

#ifdef DMA_SUPPORT
#define DMA_BUFFER_COUNT 1
#define DMA_BUFFER_SIZE  8*8*1024

unsigned long ulDMADisable = 0;
unsigned long ulDMABufferCount = DMA_BUFFER_COUNT;
unsigned long ulDMABufferSize = DMA_BUFFER_SIZE;

module_param(ulDMADisable, ulong, 0);
MODULE_PARM_DESC(ulDMADisable, "Disable DMA buffer allocation.");
module_param(ulDMABufferCount, ulong, 0);
MODULE_PARM_DESC(ulDMABufferCount, "Number of DMA-buffers to use.");
module_param(ulDMABufferSize, ulong, 0);
MODULE_PARM_DESC(ulDMABufferSize, "Size of a DMA-buffer.");
#endif 


#define PCI_VENDOR_ID_HILSCHER         0x15CF
#define PCI_DEVICE_ID_HILSCHER_NETX    0x0000
#define PCI_DEVICE_ID_HILSCHER_NETPLC  0x0010
#define PCI_DEVICE_ID_HILSCHER_NETJACK 0x0020
#define PCI_SUBDEVICE_ID_NXSB_PCA      0x3235
#define PCI_SUBDEVICE_ID_NXPCA         0x3335
#define PCI_SUBDEVICE_ID_NETPLC_RAM    0x0000
#define PCI_SUBDEVICE_ID_NETPLC_FLASH  0x0001
#define PCI_SUBDEVICE_ID_NETJACK_RAM   0x0000
#define PCI_SUBDEVICE_ID_NETJACK_FLASH 0x0001

#define DPM_HOST_INT_EN0	0xfff0
#define DPM_HOST_INT_STAT0	0xffe0
#define PLX_GPIO_OFFSET         0x15
#define PLX_TIMING_OFFSET       0x0a

#define DPM_HOST_INT_MASK	0xe600ffff
#define DPM_HOST_INT_GLOBAL_EN	0x80000000
#define PLX_GPIO_DATA0_MASK     0x00000004
#define PLX_GPIO_DATA1_MASK     0x00000020

#define NX_PCA_PCI_8_BIT_DPM_MODE  0x5431F962
#define NX_PCA_PCI_16_BIT_DPM_MODE 0x4073F8E2
#define NX_PCA_PCI_32_BIT_DPM_MODE 0x40824122

/* number of bar */
#define DPM_BAR     0 /* points to the DPM -> netX, netPLC, netJACK */
#define EXT_MEM_BAR 1 /* points to the optional extended memory     */
#define PLX_DPM_BAR 2 /* points to the DPM -> netXPLX               */ 
#define PXA_PLX_BAR 0 /* timing config register                     */

/* index of uio_info structure's memory array */
#define DPM_INDEX     0 /* first mapping describes DPM              */
#define EXT_MEM_INDEX 1 /* second mapping describes extended memory */

#define DPM_MEM_NAME "dpm"
#define EXT_MEM_NAME "extmem"
#define DMA_MEM_NAME "dma"

struct pxa_dev_info {
	uint32_t __iomem *plx;
	uint8_t dpm_mode;
	uint32_t plx_timing;
};

struct uio_netx_priv {
	int32_t dmacount;
	int32_t memcount;
	struct pxa_dev_info *pxa_info;
};

static irqreturn_t netx_handler(int irq, struct uio_info *dev_info)
{
	if(((struct uio_netx_priv*)dev_info->priv)->pxa_info != NULL)
	{
		/* This is a PLX device and cannot produce an IRQ */
		return IRQ_NONE;
	} else
	{	
		void __iomem *int_enable_reg = dev_info->mem[0].internal_addr
						+ DPM_HOST_INT_EN0;
		void __iomem *int_status_reg = dev_info->mem[0].internal_addr
						+ DPM_HOST_INT_STAT0;
	
		/* Is one of our interrupts enabled and active ? */
		if (!(ioread32(int_enable_reg) & ioread32(int_status_reg)
			& DPM_HOST_INT_MASK))
			return IRQ_NONE;
	
		/* Disable interrupt */
		iowrite32(ioread32(int_enable_reg) & ~DPM_HOST_INT_GLOBAL_EN,
			int_enable_reg);
		return IRQ_HANDLED;
	}
}

static int netx_pxa_set_plx_timing(struct uio_info *info)
{
	struct uio_netx_priv *priv = (struct uio_netx_priv *) info->priv;
	uint32_t __iomem *plx_timing;
	if (!priv->pxa_info)
		return -ENODEV;
	plx_timing = priv->pxa_info->plx + PLX_TIMING_OFFSET;
	*plx_timing = priv->pxa_info->plx_timing;
	return 0;
}

static int netx_pxa_get_plx_timing(struct uio_info *info)
{
	struct uio_netx_priv *priv = (struct uio_netx_priv *) info->priv;
	if (!priv->pxa_info)
		return -ENODEV;
	switch (priv->pxa_info->dpm_mode) {
	case 8:
		priv->pxa_info->plx_timing = NX_PCA_PCI_8_BIT_DPM_MODE;
		break;
	case 16:
		priv->pxa_info->plx_timing = NX_PCA_PCI_16_BIT_DPM_MODE;
		break;
	case 32:
		priv->pxa_info->plx_timing = NX_PCA_PCI_32_BIT_DPM_MODE;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int netx_pxa_get_dpm_mode(struct uio_info *info)
{
	struct uio_netx_priv *priv = (struct uio_netx_priv *) info->priv;
	uint32_t __iomem *plx_gpio;
	if (!priv->pxa_info)
		return -ENODEV;
	plx_gpio = priv->pxa_info->plx + PLX_GPIO_OFFSET;
	if ((*plx_gpio & PLX_GPIO_DATA0_MASK) &&
	   ~(*plx_gpio & PLX_GPIO_DATA1_MASK))
		priv->pxa_info->dpm_mode = 8;
	else if (~(*plx_gpio & PLX_GPIO_DATA0_MASK) &&
		 (*plx_gpio & PLX_GPIO_DATA1_MASK))
		priv->pxa_info->dpm_mode = 32;
	else if (~(*plx_gpio & PLX_GPIO_DATA0_MASK) &&
		~(*plx_gpio & PLX_GPIO_DATA1_MASK))
		priv->pxa_info->dpm_mode = 16;
	else
		return -EINVAL;
	return 0;
}

#ifdef DMA_SUPPORT
int create_dma_buffer(struct pci_dev *dev, struct uio_info *info, struct uio_mem *dma_mem)
{
	int ret = 0;
	void *addr;
	dma_addr_t busaddr;

	/* Allocate DMA-capable buffer */	
	addr = pci_alloc_consistent(dev, ulDMABufferSize, &busaddr);
	if (!addr) {
		dev_info(&dev->dev, "error during dma allocation\n");
		ret = -ENOMEM;
		goto err_free_umem;
	}
	/* we need to reserve memory to satisfy pat check */
	set_memory_uc( (unsigned long)addr, (ulDMABufferSize >> PAGE_SHIFT));
	memset(addr ,0 ,ulDMABufferSize);
	dma_mem->addr = (unsigned long)busaddr;
	dma_mem->internal_addr = addr;
	dma_mem->size = ulDMABufferSize;
	dma_mem->name = DMA_MEM_NAME;
	dma_mem->memtype = UIO_MEM_PHYS;
	return 0;

err_free_umem:
	return ret;
}

int release_dma_mem(struct pci_dev *dev, struct uio_info *info)
{
	struct uio_netx_priv *priv = info->priv;
	
	while(priv->dmacount-->0) {
		priv->memcount--;
		set_memory_wb( (unsigned long)info->mem[priv->memcount].internal_addr, 
			       (info->mem[priv->memcount].size >> PAGE_SHIFT));
		pci_free_consistent(dev,
				info->mem[priv->memcount].size, 
				(void*)(info->mem[priv->memcount].internal_addr),
				(dma_addr_t) info->mem[priv->memcount].addr);
		info->mem[priv->memcount].addr          = 0;
		info->mem[priv->memcount].size          = 0;
		info->mem[priv->memcount].internal_addr = 0;
	}
	return 0;
}

static int add_dma(struct pci_dev *dev, struct uio_info *info)
{
	struct uio_netx_priv *priv = info->priv;
	int i = 0;
	int ret = 0;
	
	if (MAX_UIO_MAPS<(priv->memcount+ulDMABufferCount)) {
		dev_info(&dev->dev, "Base uio driver does not server enough memory\n"
			"regions for dma allocation (see MAX_UIO_MAPS)!\n");
		return -ENOMEM;
	}
	pci_set_dma_mask(dev, DMA_BIT_MASK(32));
	for (;i<ulDMABufferCount;i++) {
		if ((ret = create_dma_buffer(dev, info, &info->mem[i+priv->memcount])))
			goto err_dma;
		dev_info(&dev->dev, "DMA buffer allocated (addr/size:0x%lX/0x%lX)\n", 
			 (long unsigned int)info->mem[i+priv->memcount].addr, 
			info->mem[i+priv->memcount].size);
		priv->dmacount++;
	}
	priv->memcount+=ulDMABufferCount;
	return 0;
err_dma:
	release_dma_mem(dev, info);
	return ret;
}
#endif 

static int __devinit netx_pci_probe(struct pci_dev *dev,
					const struct pci_device_id *id)
{
	struct uio_info *info;
	int bar;
	info = kzalloc(sizeof(struct uio_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	if (!(info->priv = (struct uio_netx_priv *) kzalloc(sizeof(struct uio_netx_priv), GFP_KERNEL)))
		goto out_priv;
	if (pci_enable_device(dev))
		goto out_free;
	if (pci_request_regions(dev, "netx"))
		goto out_disable;
	switch (id->device) {
	case PCI_DEVICE_ID_HILSCHER_NETX:
		bar = DPM_BAR;
		info->name = "netx";
		break;
	case PCI_DEVICE_ID_HILSCHER_NETPLC:
		bar = DPM_BAR;
		info->name = "netplc";
		break;
	case PCI_DEVICE_ID_HILSCHER_NETJACK:
		bar = DPM_BAR;
		info->name = "netjack";
		break;
	default:
		bar = PLX_DPM_BAR;
		info->name = "netx_plx";
	}
	/* BAR 0 or 2 points to the card's dual port memory */
	info->mem[DPM_INDEX].addr = pci_resource_start(dev, bar);
	if (!info->mem[DPM_INDEX].addr)
		goto out_release;
 
	info->mem[DPM_INDEX].internal_addr = ioremap_nocache(
		pci_resource_start(dev, bar),
		pci_resource_len(dev, bar));
    
	if (!info->mem[DPM_INDEX].internal_addr)
		goto out_release;
  
	dev_info(&dev->dev, "DPM at 0x%lX\n", (long unsigned int)info->mem[DPM_INDEX].addr);
	info->mem[DPM_INDEX].size = pci_resource_len(dev, bar);
	info->mem[DPM_INDEX].memtype = UIO_MEM_PHYS;
	info->mem[DPM_INDEX].name = DPM_MEM_NAME;
	((struct uio_netx_priv*)(info->priv))->memcount = 1;
	
	/* map extended mem (BAR 1 points to the extended memory) */
	info->mem[EXT_MEM_INDEX].addr = pci_resource_start(dev, EXT_MEM_BAR);
 
	/* extended memory is optional, so don't care if it is not present */
	if (info->mem[EXT_MEM_INDEX].addr) {
		info->mem[EXT_MEM_INDEX].internal_addr = ioremap_nocache(
		pci_resource_start(dev, EXT_MEM_BAR),
		pci_resource_len(dev, EXT_MEM_BAR));  
    
		if (!info->mem[EXT_MEM_INDEX].internal_addr)
			goto out_unmap;
    
		dev_info(&dev->dev, "extended memory at 0x%08lX\n", (long unsigned int)info->mem[EXT_MEM_INDEX].addr);
		info->mem[EXT_MEM_INDEX].size    = pci_resource_len(dev, EXT_MEM_BAR);
		info->mem[EXT_MEM_INDEX].memtype = UIO_MEM_PHYS;
		info->mem[EXT_MEM_INDEX].name = EXT_MEM_NAME;
		((struct uio_netx_priv*)(info->priv))->memcount++;
	}
  
	info->irq = dev->irq;
	info->irq_flags = IRQF_DISABLED | IRQF_SHARED;
	info->handler = netx_handler;
	info->version = "0.0.1";
	
	if ((id->device == PCI_DEVICE_ID_HILSCHER_NETX) ||
		(id->device == PCI_DEVICE_ID_HILSCHER_NETPLC) ||
		(id->device == PCI_DEVICE_ID_HILSCHER_NETJACK)) {
		/* make sure all interrupts are disabled */
		iowrite32(0, info->mem[DPM_INDEX].internal_addr + DPM_HOST_INT_EN0);
		((struct uio_netx_priv*)(info->priv))->pxa_info = NULL;
	} else if (id->subdevice == PCI_SUBDEVICE_ID_NXPCA) {
		/* map PLX registers */
		struct pxa_dev_info *pxa_info = (struct pxa_dev_info *)
			kzalloc(sizeof(struct pxa_dev_info), GFP_KERNEL);
		if (!pxa_info)
			goto out_unmap;
		((struct uio_netx_priv*)(info->priv))->pxa_info = pxa_info;
		/* set PXA PLX Timings */
		pxa_info->plx = ioremap_nocache(
			pci_resource_start(dev, PXA_PLX_BAR),
			pci_resource_len(dev, PXA_PLX_BAR));
		if (!pxa_info->plx)
			goto out_unmap;
		if (netx_pxa_get_dpm_mode(info))
			goto out_unmap_plx;
		if (netx_pxa_get_plx_timing(info))
			goto out_unmap_plx;
		if (netx_pxa_set_plx_timing(info))
			goto out_unmap_plx;
	} else {
		struct pxa_dev_info *pxa_info = (struct pxa_dev_info *)
			kzalloc(sizeof(struct pxa_dev_info), GFP_KERNEL);
		if (!pxa_info)
			goto out_free_pxa;
		pxa_info->plx = NULL;
		pxa_info->plx_timing = 0;
		pxa_info->dpm_mode = 0;
		((struct uio_netx_priv*)info->priv)->pxa_info = pxa_info;
	}
#ifdef DMA_SUPPORT
	if ((!ulDMADisable) && (add_dma(dev, info)))
		printk("error reserving memory for dma!\n");
#endif
	if (uio_register_device(&dev->dev, info)) {
		if (id->subdevice != PCI_SUBDEVICE_ID_NXPCA)
			goto out_unmap;
		else
			goto out_unmap_plx;
	}
	pci_set_drvdata(dev, info);
	if (id->device == PCI_DEVICE_ID_HILSCHER_NETX)
		dev_info(&dev->dev,
			"registered CifX card\n");
	else if (id->device == PCI_DEVICE_ID_HILSCHER_NETPLC)
		dev_info(&dev->dev,
			"registered netPLC card\n");
	else if (id->device == PCI_DEVICE_ID_HILSCHER_NETJACK)
		dev_info(&dev->dev, "registered netJACK card\n");
	else if (id->subdevice == PCI_SUBDEVICE_ID_NXSB_PCA)
		dev_info(&dev->dev,
			"registered NXSB-PCA adapter card\n");
	else {
		struct pxa_dev_info *pxa_info = (struct pxa_dev_info *)
			((struct uio_netx_priv*)info->priv)->pxa_info;
		dev_info(&dev->dev,
			"registered NXPCA-PCI adapter card in %d bit mode\n",
			pxa_info->dpm_mode);
	}
	return 0;
out_unmap_plx:
	iounmap(((struct uio_netx_priv*)(info->priv))->pxa_info->plx);
out_free_pxa:
	kfree(((struct uio_netx_priv*)info->priv)->pxa_info);	
out_unmap:
#ifdef DMA_SUPPORT
	release_dma_mem(dev, info);
#endif
	iounmap(info->mem[DPM_INDEX].internal_addr);
	if (info->mem[EXT_MEM_INDEX].internal_addr)
		iounmap(info->mem[EXT_MEM_INDEX].internal_addr);
out_release:
	pci_release_regions(dev);
out_disable:
	pci_disable_device(dev);
out_priv:
	kfree(info->priv);
out_free:
	kfree(info);
	return -ENODEV;
}

static void netx_pci_remove(struct pci_dev *dev)
{
	struct uio_info *info = pci_get_drvdata(dev);
	struct pxa_dev_info *pxa_info = ((struct uio_netx_priv*)info->priv)->pxa_info;
	if (!pxa_info) {
		/* Disable all interrupts */
		iowrite32(0, info->mem[DPM_INDEX].internal_addr + DPM_HOST_INT_EN0);
	} 
	if ( pxa_info && pxa_info->plx)
		iounmap(pxa_info->plx);
	if (pxa_info)
		kfree(pxa_info);
	uio_unregister_device(info);
#ifdef DMA_SUPPORT
	release_dma_mem(dev, info);
#endif
	pci_release_regions(dev);
	pci_disable_device(dev);
	pci_set_drvdata(dev, NULL);
	iounmap(info->mem[DPM_INDEX].internal_addr);
	if (info->mem[EXT_MEM_INDEX].internal_addr)
		iounmap(info->mem[EXT_MEM_INDEX].internal_addr);
	kfree(info->priv);
	kfree(info);
}

static struct pci_device_id netx_pci_ids[] = {
	{
		.vendor =	PCI_VENDOR_ID_HILSCHER,
		.device =	PCI_DEVICE_ID_HILSCHER_NETX,
		.subvendor =	0,
		.subdevice =	0,
	},
	{
		.vendor =	PCI_VENDOR_ID_PLX,
		.device =	PCI_DEVICE_ID_PLX_9030,
		.subvendor =	PCI_VENDOR_ID_PLX,
		.subdevice =	PCI_SUBDEVICE_ID_NXSB_PCA,
	},
	{
		.vendor =	PCI_VENDOR_ID_PLX,
		.device =	PCI_DEVICE_ID_PLX_9030,
		.subvendor =	PCI_VENDOR_ID_PLX,
		.subdevice =	PCI_SUBDEVICE_ID_NXPCA,
	},
	{
		.vendor =	PCI_VENDOR_ID_HILSCHER,
		.device =	PCI_DEVICE_ID_HILSCHER_NETPLC,
		.subvendor =	PCI_VENDOR_ID_HILSCHER,
		.subdevice =	PCI_SUBDEVICE_ID_NETPLC_RAM,
	},
	{
		.vendor =	PCI_VENDOR_ID_HILSCHER,
		.device =	PCI_DEVICE_ID_HILSCHER_NETPLC,
		.subvendor =	PCI_VENDOR_ID_HILSCHER,
		.subdevice =	PCI_SUBDEVICE_ID_NETPLC_FLASH,
	},
	{
		.vendor = PCI_VENDOR_ID_HILSCHER,
		.device = PCI_DEVICE_ID_HILSCHER_NETJACK,
		.subvendor =  PCI_VENDOR_ID_HILSCHER,
		.subdevice =  PCI_SUBDEVICE_ID_NETJACK_RAM,
	},
	{
		.vendor = PCI_VENDOR_ID_HILSCHER,
		.device = PCI_DEVICE_ID_HILSCHER_NETJACK,
		.subvendor =  PCI_VENDOR_ID_HILSCHER,
		.subdevice =  PCI_SUBDEVICE_ID_NETJACK_FLASH,
	},
	{ 0, }
};

static struct pci_driver netx_pci_driver = {
	.name = "netx",
	.id_table = netx_pci_ids,
	.probe = netx_pci_probe,
	.remove = netx_pci_remove,
};

static int __init netx_init_module(void)
{
	return pci_register_driver(&netx_pci_driver);
}

static void __exit netx_exit_module(void)
{
	pci_unregister_driver(&netx_pci_driver);
}

module_init(netx_init_module);
module_exit(netx_exit_module);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Hans J. Koch, Manuel Traut, Sebastian Doell");
MODULE_DESCRIPTION("Device driver for netX hardware\n\t\tHilscher Gesellschaft fuer Systemautomation mbH");
