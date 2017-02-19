/*
 * SCC on 8270
 *
 * Jan 14, 2017 SAT, ICE
 * milo <milod@163.com>
 */
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/interrupt.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/slab.h>

#include <asm/uaccess.h>
#include <asm/cpm2.h>

#define DRV_MODULE_NAME		"scc_serial"
#define DRV_MODULE_VERSION	"1.0"
#define DEV_NAME		"scc"

MODULE_AUTHOR("milo <milod@163.com>");
MODULE_DESCRIPTION("freescale 8270 scc driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_MODULE_VERSION);

#define SCC_TP_GRA	((ushort)0x0080)	/* Graceful stop complete */
#define SCC_TP_TXE	((ushort)0x0010)	/* Transmit Error */
#define SCC_TP_BSY	((ushort)0x0004)	/* Busy */
#define SCC_TP_TXB	((ushort)0x0002)	/* A buffer was transmitted */
#define SCC_TP_RXB	((ushort)0x0001)	/* A buffer was received */

#define NUM_OF_SCC_DEV	4
#define SCC_MAX_BUFZ	(64*1024-1)
#define BD_RNUM		2
#define BD_TNUM		16
#define BD_NUM		(BD_RNUM + BD_TNUM)

typedef struct {
	struct device *dev;
	void __iomem *hwreg;    /* hw registers        */
	void __iomem *sccp;     /* parameter ram       */
	dma_addr_t   ring_mem_addr;
	void __iomem *ring_base;

	unsigned int irq;

	dma_addr_t   rbuf[BD_RNUM];
	dma_addr_t   tbuf[BD_TNUM];
	void __iomem *rbufv[BD_RNUM];
	void __iomem *tbufv[BD_TNUM];
	u32          rcur;
	u32          tcur;

	u32          cmd;
	int          minor;
	char         name[32];
}scc_dev_t;

static DECLARE_WAIT_QUEUE_HEAD(scc_write_wq);
static DECLARE_WAIT_QUEUE_HEAD(scc_read_wq);

static scc_dev_t scc_dev_array[NUM_OF_SCC_DEV];
static int dev_nr = -1;

inline static u32 get_scce(scc_dev_t *scc_dev)
{
	return in_be32(scc_dev->hwreg+0x10);
}

inline static void set_scce(scc_dev_t *scc_dev, u32 val)
{
	out_be32(scc_dev->hwreg+0x10, val);
}

static irqreturn_t scc_interrupt(int irq, void *dev_id)
{
	scc_dev_t *scc_dev = (scc_dev_t *)dev_id;

	u32 int_events;
	int nr;
	int handled;

	nr = 0;
	while ((int_events = get_scce(scc_dev)) != 0) {
		nr++;

	//printk(KERN_ERR"(MILO)fcce=%08x\n", int_events);
		// clear events
		set_scce(scc_dev, int_events);

		if (int_events & SCC_TP_RXB)
			wake_up_interruptible(&scc_read_wq);

		if (int_events & SCC_TP_TXB)
			wake_up_interruptible(&scc_write_wq);
	}

	handled = nr > 0;
	return IRQ_RETVAL(handled);
}

static int scc_dev_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int scc_dev_close(struct inode *inode, struct file *file)
{
	return 0;
}

static int is_sndbuf_available(scc_dev_t *scc_dev)
{
	cbd_t __iomem *bdp;
	u32 tcur = scc_dev->tcur;
	int ret;

	bdp = scc_dev->ring_base + BD_RNUM*sizeof(cbd_t) + tcur*sizeof(cbd_t);
	ret = !(in_be16(&bdp->cbd_sc) & BD_SC_READY);
	//printk(KERN_ERR"(MILO)tcur=%d\n", tcur);
	//printk(KERN_ERR"(MILO)snd_avail=%d\n", !(bdp->cbd_sc & BD_SC_READY));

	return ret;
}

static ssize_t scc_dev_write(struct file *file, const char __user *buf, size_t count, loff_t *offset)
{
	unsigned int minor = iminor(file_inode(file));
	scc_dev_t *scc_dev = scc_dev_array + minor;

	cbd_t __iomem *bdp;
	u32 tcur = scc_dev->tcur;
	u16 wrap;

	if (count > SCC_MAX_BUFZ)
		count = SCC_MAX_BUFZ;

	wait_event_interruptible(scc_write_wq, is_sndbuf_available(scc_dev));

	if (!is_sndbuf_available(scc_dev))
		return 0;

	bdp = scc_dev->ring_base + BD_RNUM*sizeof(cbd_t) + tcur*sizeof(cbd_t);
	//sprintf(scc_dev->tbufv[0], "hello1\n");
	//bdp->cbd_bufaddr = scc_dev->tbuf[0];
	copy_from_user(scc_dev->tbufv[tcur], buf, count);
	bdp->cbd_datlen = count;
	wrap = (tcur < BD_TNUM - 1) ? 0 : BD_SC_WRAP;
	//setbits16(&bdp->cbd_sc, BD_SC_READY|BD_SC_LAST|BD_SC_INTRPT|wrap);
	out_be16(&bdp->cbd_sc, BD_SC_READY|BD_SC_LAST|BD_SC_INTRPT|wrap);
	if (wrap)
		scc_dev->tcur = 0;
	else
		scc_dev->tcur++;
	//printk(KERN_ERR"(MILO)tcount=%d\n", count);

	return count;
}

static int is_rcvdat_available(scc_dev_t *scc_dev)
{
	cbd_t __iomem *bdp;
	u32 rcur = scc_dev->rcur;
	int ret;

	bdp = scc_dev->ring_base + rcur*sizeof(cbd_t);
	ret = !(in_be16(&bdp->cbd_sc) & BD_SC_EMPTY);

#if 0
	printk(KERN_ERR"(MILO)rcur=%d\n", rcur);
	printk(KERN_ERR"(MILO)rcv_avail=%d\n", !(bdp->cbd_sc & BD_SC_EMPTY));
	printk(KERN_ERR"(MILO)cbd_sc=%04x\n", in_be16(&bdp->cbd_sc));

	printk(KERN_ERR"(MILO)-------\n");
	bdp = scc_dev->ring_base + 0*sizeof(cbd_t);
	printk(KERN_ERR"(MILO)0cbd_sc=%04x\n", in_be16(&bdp->cbd_sc));
	bdp = scc_dev->ring_base + 1*sizeof(cbd_t);
	printk(KERN_ERR"(MILO)1cbd_sc=%04x\n", in_be16(&bdp->cbd_sc));
	printk(KERN_ERR"(MILO)-------\n");
#endif

	return ret;
}

static ssize_t scc_dev_read(struct file *file, char __user *buf, size_t count, loff_t *offset)
{
	unsigned int minor = iminor(file_inode(file));
	scc_dev_t *scc_dev = scc_dev_array + minor;

	cbd_t __iomem *bdp;
	u32 rcur = scc_dev->rcur;
	u16 wrap;

	wait_event_interruptible(scc_read_wq, is_rcvdat_available(scc_dev));

	if (!is_rcvdat_available(scc_dev))
		return 0;

	bdp = scc_dev->ring_base + rcur*sizeof(cbd_t);
	if (count > bdp->cbd_datlen)
		count = bdp->cbd_datlen;
	copy_to_user(buf, scc_dev->rbufv[rcur], count);
	wrap = (rcur < BD_RNUM - 1) ? 0 : BD_SC_WRAP;
	//setbits16(&bdp->cbd_sc, BD_SC_EMPTY|BD_SC_INTRPT|wrap);
	out_be16(&bdp->cbd_sc, BD_SC_EMPTY|BD_SC_INTRPT|wrap);
	if (wrap)
		scc_dev->rcur = 0;
	else
		scc_dev->rcur++;
	//printk(KERN_ERR"(MILO)tcount=%d\n", count);

	return count;
}

static struct file_operations scc_dev_fops = {
	.owner		= THIS_MODULE,
	.open		= scc_dev_open,
	.release	= scc_dev_close,
	.llseek		= no_llseek,
	.read		= scc_dev_read,
	.write		= scc_dev_write,
};

static void init_bds(scc_dev_t *scc_dev)
{
	cbd_t __iomem *bdp;
	int i;

	/*
	 * Initialize the receive buffer descriptors.
	 */
	for (i = 0, bdp = scc_dev->ring_base; i < BD_RNUM; i++, bdp++) {
		scc_dev->rbufv[i] = (void __iomem __force *)
			dma_alloc_coherent(scc_dev->dev,
					   SCC_MAX_BUFZ,
					   &scc_dev->rbuf[i],
					   GFP_KERNEL);
		bdp->cbd_bufaddr = scc_dev->rbuf[i];
		bdp->cbd_datlen = 0;
		bdp->cbd_sc = BD_SC_EMPTY |BD_SC_INTRPT|
			((i < BD_RNUM - 1) ? 0 : BD_SC_WRAP);
	}
	scc_dev->rcur = 0;

	/*
	 * ...and the same for transmit.
	 */
	for (i = 0, bdp = scc_dev->ring_base + BD_RNUM*sizeof(cbd_t);
			i < BD_TNUM;
			i++, bdp++) {
		scc_dev->tbufv[i] = (void __iomem __force *)
			dma_alloc_coherent(scc_dev->dev,
					   SCC_MAX_BUFZ,
					   &scc_dev->tbuf[i],
					   GFP_KERNEL);
		bdp->cbd_bufaddr = scc_dev->tbuf[i];
		bdp->cbd_datlen = 0;
		bdp->cbd_sc = (i < BD_TNUM - 1) ? 0 : BD_SC_WRAP;
	}
	scc_dev->tcur = 0;
}

static struct class *scc_class = NULL;
static int scc_major = -1;
static const struct of_device_id fcc_serial_match[];

static int scc_serial_probe(struct platform_device *ofdev)
{
	//const struct of_device_id *match;
	const u32 *data;
	int len;
	unsigned int irq;
	void __iomem *cmxscr;
	int ret;
	sccp_t *gp;
	u32 clkcfg_mask, clkcfg_value;
	scc_dev_t *scc_dev;

	if (dev_nr >= NUM_OF_SCC_DEV)
		goto errout2;
	/*
	match = of_match_device(scc_serial_match, &ofdev->dev);
	if (!match)
		return -EINVAL;
	*/

	scc_dev = (scc_dev_t *)kmalloc(sizeof *scc_dev, GFP_KERNEL);
	if (!scc_dev)
		goto errout2;

	strcpy(scc_dev->name, ofdev->name);
	printk(KERN_ERR"(MILO)name=%s\n", scc_dev->name);
	scc_dev->dev = &ofdev->dev;
	scc_dev->ring_mem_addr = cpm_dpalloc(BD_NUM * sizeof(cbd_t), 8);
	if (IS_ERR_VALUE(scc_dev->ring_mem_addr))
		goto errout;

	scc_dev->ring_base = (void __iomem __force *)
		cpm_dpram_addr(scc_dev->ring_mem_addr);

	init_bds(scc_dev);

	data = of_get_property(ofdev->dev.of_node, "fsl,cpm-command", &len);
	if (!data || len != 4)
		goto errout;
	scc_dev->cmd = *data;

	data = of_get_property(ofdev->dev.of_node, "fsl,clkcfg_mask", &len);
	if (!data || len != 4)
		goto errout;
	clkcfg_mask = *data;

	data = of_get_property(ofdev->dev.of_node, "fsl,clkcfg_value", &len);
	if (!data || len != 4)
		goto errout;
	clkcfg_value = *data;

	irq = irq_of_parse_and_map(ofdev->dev.of_node, 0);
	if (irq == NO_IRQ)
		goto errout;
	scc_dev->irq = irq;

	scc_dev->hwreg = of_iomap(ofdev->dev.of_node, 0);
	if (!scc_dev->hwreg)
		goto errout;

	scc_dev->sccp =  of_iomap(ofdev->dev.of_node, 1);
	if (!scc_dev->sccp)
		goto errout;
	gp = (sccp_t *)scc_dev->sccp;

	cmxscr = of_iomap(ofdev->dev.of_node, 2);
	if (!cmxscr)
		goto errout;


	// 1/2. setup SCC pins (done in uboot)
	//      including TXD RXD RTS CTS
	// 3. setup CLK pin
	// 4. NMSI mode, connect CLK to SCC (CMXSCR)
	clrbits32(cmxscr, clkcfg_mask);
	setbits32(cmxscr, clkcfg_value);
	// 4. set GSMR except ENT or ENR
	//    GSMR_H
	out_be32(scc_dev->hwreg + 4, SCC_GSMRH_TRX|SCC_GSMRH_TTX|SCC_GSMRH_CDS|SCC_GSMRH_CTSS);
	//    GSMR_L
	out_be32(scc_dev->hwreg, 0);
	// 5. PSMR
	// (not used)
	// 6. DSR
	// (not used)
	// 7. parm RAM
	out_be16(&gp->scc_rbase, scc_dev->ring_mem_addr);
	out_be16(&gp->scc_tbase, scc_dev->ring_mem_addr+BD_RNUM*sizeof(cbd_t));

	out_8(&gp->scc_rfcr, SCC_GBL|SCC_EB);
	out_8(&gp->scc_tfcr, SCC_GBL|SCC_EB);

	out_be16(&gp->scc_mrblr, SCC_MAX_BUFZ);
	// 8. CPCR
	ret = cpm_command(scc_dev->cmd, CPM_CR_INIT_TRX);
	printk(KERN_ERR"(MILO)cpm_command ret=%d\n", ret);
	if (ret)
		goto errout;
	// 9. clear SCCE
	out_be16(scc_dev->hwreg + 0x10, 0xffff);
	// 10. SCCM
	out_be16(scc_dev->hwreg + 0x14, 0x0003);	/* TXB RXB */
	/* Install our interrupt handler. */
	ret = request_irq(scc_dev->irq, scc_interrupt, IRQF_SHARED,
			DRV_MODULE_NAME, scc_dev);
	if (ret) {
		printk(KERN_ERR"(MILO)Could not allocate IRQ=%d\n",
				scc_dev->irq);
		goto errout;
	}
	// 11. set GSMR_L[ENT] and GSMR_L[ENR]
	out_be32(scc_dev->hwreg, SCC_GSMRL_ENR|SCC_GSMRL_ENT);

	if (scc_major < 0) {
		if ((scc_major = register_chrdev(0, DEV_NAME, &scc_dev_fops)) < 0) {
			printk(KERN_ERR "scc_serial_probe: Unable to get major %d for device\n", scc_major);
			goto errout;
		}
	}
	if (!scc_class) {
		scc_class = class_create(THIS_MODULE, scc_dev->name);
	}
	scc_dev->minor = ++dev_nr;
	device_create(scc_class, NULL,
		      MKDEV(scc_major, dev_nr),
		      NULL,
		      scc_dev->name);

	printk(KERN_INFO"(milo)ppc8270 scc driver loaded\n");

	return 0;
errout:
	kfree(scc_dev);
errout2:
	printk(KERN_ERR"(MILO)scc probe failed\n");
	return -1;
}

static int scc_serial_remove(struct platform_device *ofdev)
{
	scc_dev_t *scc_dev;
	int i;

	for (i=0; i<=dev_nr; ++i) {
		scc_dev = scc_dev_array + i;
		clrbits32(scc_dev->hwreg, SCC_GSMRL_ENR|SCC_GSMRL_ENT);
		free_irq(scc_dev->irq, scc_dev);
		device_destroy(scc_class, MKDEV(scc_major, scc_dev->minor));
	}

	class_destroy(scc_class);
	unregister_chrdev(scc_major, DEV_NAME);

	return 0;
}

static const struct of_device_id scc_serial_match[] = {
	{
		.compatible = "fsl,cpm2-scc-serial",
	},
	{}
};
MODULE_DEVICE_TABLE(of, scc_serial_match);

static struct platform_driver scc_driver = {
	.driver = {
		.name = DRV_MODULE_NAME,
		.of_match_table = scc_serial_match,
	},
	.probe = scc_serial_probe,
	.remove = scc_serial_remove,
};

module_platform_driver(scc_driver);
