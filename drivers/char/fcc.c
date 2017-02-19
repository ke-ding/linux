/*
 * FCC on 8270
 *
 * May 24, 2016 SAT, ICE
 * milo <milod@163.com>
 */
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/interrupt.h>
#include <linux/of_platform.h>
#include <linux/proc_fs.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/dma-mapping.h>

#include <asm/uaccess.h>
#include <asm/cpm2.h>

#define DRV_MODULE_NAME		"fcc_serial"
#define DRV_MODULE_VERSION	"1.0"

MODULE_AUTHOR("milo <milod@163.com>");
MODULE_DESCRIPTION("freescale 8270 fcc driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_MODULE_VERSION);

#define FCC_SERIAL_GRA	((u32)0x00800000)	/* Graceful stop complete */
#define FCC_SERIAL_TXE	((u32)0x00100000)	/* Transmit Error */
#define FCC_SERIAL_RXF	((u32)0x00080000)	/* Full frame received */
#define FCC_SERIAL_BSY	((u32)0x00040000)	/* Busy.  Rx Frame dropped */
#define FCC_SERIAL_TXB	((u32)0x00020000)	/* A buffer was transmitted */
#define FCC_SERIAL_RXB	((u32)0x00010000)	/* A buffer was received */

#define FCC_MAX_BUFZ	(64*1024-1)
#define BD_RNUM		2
#define BD_TNUM		16
#define BD_NUM		(BD_RNUM + BD_TNUM)

struct {
	struct device *dev;
	void __iomem *hwreg;    /* hw registers        */
	void __iomem *fccp;     /* parameter ram       */
	u32          mem;       /* FCC DPRAM */
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
	int          major;
} fcc_dev;

static DECLARE_WAIT_QUEUE_HEAD(fcc_write_wq);
static DECLARE_WAIT_QUEUE_HEAD(fcc_read_wq);

inline static u32 get_fcce(void)
{
	return in_be32(fcc_dev.hwreg+0x10);
}

inline static void set_fcce(u32 val)
{
	out_be32(fcc_dev.hwreg+0x10, val);
}

static irqreturn_t fcc_interrupt(int irq, void *dev_id)
{
	u32 int_events;
	int nr;
	int handled;

	nr = 0;
	while ((int_events = get_fcce()) != 0) {
		nr++;

	//printk(KERN_ERR"(MILO)fcce=%08x\n", int_events);
		// clear events
		set_fcce(int_events);

		if (int_events & FCC_SERIAL_RXF)
			wake_up_interruptible(&fcc_read_wq);

		if (int_events & FCC_SERIAL_TXB)
			wake_up_interruptible(&fcc_write_wq);
	}

	handled = nr > 0;
	return IRQ_RETVAL(handled);
}

static int fcc_dev_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int fcc_dev_close(struct inode *inode, struct file *file)
{
	return 0;
}

static int is_sndbuf_available(void)
{
	cbd_t __iomem *bdp;
	u32 tcur = fcc_dev.tcur;
	int ret;

	bdp = fcc_dev.ring_base + BD_RNUM*sizeof(cbd_t) + tcur*sizeof(cbd_t);
	ret = !(in_be16(&bdp->cbd_sc) & BD_SC_READY);
	//printk(KERN_ERR"(MILO)tcur=%d\n", tcur);
	//printk(KERN_ERR"(MILO)snd_avail=%d\n", !(bdp->cbd_sc & BD_SC_READY));

	return ret;
}

static ssize_t fcc_dev_write(struct file *file, const char __user *buf, size_t count, loff_t *offset)
{
	cbd_t __iomem *bdp;
	u32 tcur = fcc_dev.tcur;
	u16 wrap;

	if (count > FCC_MAX_BUFZ)
		count = FCC_MAX_BUFZ;

	wait_event_interruptible(fcc_write_wq, is_sndbuf_available());

	if (!is_sndbuf_available())
		return 0;

	bdp = fcc_dev.ring_base + BD_RNUM*sizeof(cbd_t) + tcur*sizeof(cbd_t);
	//sprintf(fcc_dev.tbufv[0], "hello1\n");
	//bdp->cbd_bufaddr = fcc_dev.tbuf[0];
	copy_from_user(fcc_dev.tbufv[tcur], buf, count);
	bdp->cbd_datlen = count;
	wrap = (tcur < BD_TNUM - 1) ? 0 : BD_SC_WRAP;
	//setbits16(&bdp->cbd_sc, BD_SC_READY|BD_SC_LAST|BD_SC_INTRPT|wrap);
	out_be16(&bdp->cbd_sc, BD_SC_READY|BD_SC_LAST|BD_SC_INTRPT|wrap);
	if (wrap)
		fcc_dev.tcur = 0;
	else
		fcc_dev.tcur++;
	//printk(KERN_ERR"(MILO)tcount=%d\n", count);

	return count;
}

static int is_rcvdat_available(void)
{
	cbd_t __iomem *bdp;
	u32 rcur = fcc_dev.rcur;
	int ret;

	bdp = fcc_dev.ring_base + rcur*sizeof(cbd_t);
	ret = !(in_be16(&bdp->cbd_sc) & BD_SC_EMPTY);

#if 0
	printk(KERN_ERR"(MILO)rcur=%d\n", rcur);
	printk(KERN_ERR"(MILO)rcv_avail=%d\n", !(bdp->cbd_sc & BD_SC_EMPTY));
	printk(KERN_ERR"(MILO)cbd_sc=%04x\n", in_be16(&bdp->cbd_sc));

	printk(KERN_ERR"(MILO)-------\n");
	bdp = fcc_dev.ring_base + 0*sizeof(cbd_t);
	printk(KERN_ERR"(MILO)0cbd_sc=%04x\n", in_be16(&bdp->cbd_sc));
	bdp = fcc_dev.ring_base + 1*sizeof(cbd_t);
	printk(KERN_ERR"(MILO)1cbd_sc=%04x\n", in_be16(&bdp->cbd_sc));
	printk(KERN_ERR"(MILO)-------\n");
#endif

	return ret;
}

static ssize_t fcc_dev_read(struct file *file, char __user *buf, size_t count, loff_t *offset)
{
	cbd_t __iomem *bdp;
	u32 rcur = fcc_dev.rcur;
	u16 wrap;

	wait_event_interruptible(fcc_read_wq, is_rcvdat_available());

	if (!is_rcvdat_available())
		return 0;

	bdp = fcc_dev.ring_base + rcur*sizeof(cbd_t);
	if (count > bdp->cbd_datlen)
		count = bdp->cbd_datlen;
	copy_to_user(buf, fcc_dev.rbufv[rcur], count);
	wrap = (rcur < BD_RNUM - 1) ? 0 : BD_SC_WRAP;
	//setbits16(&bdp->cbd_sc, BD_SC_EMPTY|BD_SC_INTRPT|wrap);
	out_be16(&bdp->cbd_sc, BD_SC_EMPTY|BD_SC_INTRPT|wrap);
	if (wrap)
		fcc_dev.rcur = 0;
	else
		fcc_dev.rcur++;
	//printk(KERN_ERR"(MILO)tcount=%d\n", count);

	return count;
}

static struct file_operations fcc_dev_fops = {
	.owner		= THIS_MODULE,
	.open		= fcc_dev_open,
	.release	= fcc_dev_close,
	.llseek		= no_llseek,
	.read		= fcc_dev_read,
	.write		= fcc_dev_write,
};

static int fcc_proc_show(struct seq_file *seq, void *v)
{
	seq_printf(seq, "not implemented yet(%d)\n", 0);

	return  0;
}

static int fcc_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, fcc_proc_show, NULL);
}

static const struct file_operations fcc_proc_fops = {
	.owner          = THIS_MODULE,
	.open           = fcc_proc_open,
	.read           = seq_read,
	.llseek         = seq_lseek,
	.release        = single_release,
};

static void init_bds(void)
{
	cbd_t __iomem *bdp;
	int i;

	/*
	fep->dirty_tx = fep->cur_tx = fep->tx_bd_base;
	fep->tx_free = fep->tx_ring;
	fep->cur_rx = fep->rx_bd_base;
	*/

	/*
	 * Initialize the receive buffer descriptors.
	 */
	for (i = 0, bdp = fcc_dev.ring_base; i < BD_RNUM; i++, bdp++) {
		fcc_dev.rbufv[i] = (void __iomem __force *)
			dma_alloc_coherent(fcc_dev.dev,
					   FCC_MAX_BUFZ,
					   &fcc_dev.rbuf[i],
					   GFP_KERNEL);
		bdp->cbd_bufaddr = fcc_dev.rbuf[i];
		bdp->cbd_datlen = 0;
		bdp->cbd_sc = BD_SC_EMPTY |BD_SC_INTRPT|
			((i < BD_RNUM - 1) ? 0 : BD_SC_WRAP);
	}
	fcc_dev.rcur = 0;

	/*
	 * ...and the same for transmit.
	 */
	for (i = 0, bdp = fcc_dev.ring_base + BD_RNUM*sizeof(cbd_t);
			i < BD_TNUM;
			i++, bdp++) {
		fcc_dev.tbufv[i] = (void __iomem __force *)
			dma_alloc_coherent(fcc_dev.dev,
					   FCC_MAX_BUFZ,
					   &fcc_dev.tbuf[i],
					   GFP_KERNEL);
		bdp->cbd_bufaddr = fcc_dev.tbuf[i];
		bdp->cbd_datlen = 0;
		bdp->cbd_sc = (i < BD_TNUM - 1) ? 0 : BD_SC_WRAP;
	}
	fcc_dev.tcur = 0;
}

struct proc_dir_entry *ent;
static struct class *fcc_class;
static const struct of_device_id fcc_serial_match[];

static int fcc_serial_probe(struct platform_device *ofdev)
{
	const struct of_device_id *match;
	const u32 *data;
	int len;
	unsigned int irq;
	void __iomem *cmxfcr;
	int ret;
	fccp_t *gp;

	match = of_match_device(fcc_serial_match, &ofdev->dev);
	if (!match)
		return -EINVAL;

	fcc_dev.dev = &ofdev->dev;
	fcc_dev.ring_base = (void __iomem __force *)
		dma_alloc_coherent(
				fcc_dev.dev,
				BD_NUM * sizeof(cbd_t),
				&fcc_dev.ring_mem_addr,
				GFP_KERNEL
				);

	init_bds();

	data = of_get_property(ofdev->dev.of_node, "fsl,cpm-command", &len);
	if (!data || len != 4)
		goto errout;
	fcc_dev.cmd = *data;

	irq = irq_of_parse_and_map(ofdev->dev.of_node, 0);
	if (irq == NO_IRQ)
		goto errout;
	fcc_dev.irq = irq;

	fcc_dev.hwreg = of_iomap(ofdev->dev.of_node, 0);
	if (!fcc_dev.hwreg)
		goto errout;

	fcc_dev.fccp =  of_iomap(ofdev->dev.of_node, 1);
	if (!fcc_dev.fccp)
		goto errout;
	gp = (fccp_t *)fcc_dev.fccp;

	cmxfcr = of_iomap(ofdev->dev.of_node, 2);
	if (!cmxfcr)
		goto errout;

	fcc_dev.mem = cpm_dpalloc(64, 32);
	if (IS_ERR_VALUE(fcc_dev.mem))
		goto errout;

	setbits32(cmxfcr, CMXFCR_RF1CS_CLK9|CMXFCR_TF1CS_CLK10);

	// 1. setup FCC io (done in uboot)
	// 2. setup CTS/CD io (done in uboot)
	// 3. SI
	// 4. GFMR
	out_be32(fcc_dev.hwreg, FCC_GFMR_TRX|FCC_GFMR_TTX|FCC_GFMR_CDS|FCC_GFMR_CTSS);
	// 5. FPSMR
	// 6. FDSR
	// 7. parm RAM
	out_be32(&gp->fcc_rbase, fcc_dev.ring_mem_addr);
	out_be32(&gp->fcc_tbase, fcc_dev.ring_mem_addr+BD_RNUM*sizeof(cbd_t));

	out_be16(&gp->fcc_mrblr, FCC_MAX_BUFZ);

	out_be32(&gp->fcc_rstate, (CPMFCR_GBL | CPMFCR_EB) << 24);
	out_be32(&gp->fcc_tstate, (CPMFCR_GBL | CPMFCR_EB) << 24);

	out_be16(&gp->fcc_riptr, fcc_dev.mem);		/* RIPTR */
	out_be16(&gp->fcc_tiptr, fcc_dev.mem+32);	/* TIPTR */

	out_be16(fcc_dev.fccp + 0x58, 0);	/* MFLR */
	out_be16(fcc_dev.fccp + 0x5a, 0);	/* RFTHR */
	out_be16(fcc_dev.fccp + 0x5c, 0);	/* RFCNT */
	out_be16(fcc_dev.fccp + 0x5e, 0);	/* HMASK */
	// 8. clear FCCE
	out_be32(fcc_dev.hwreg + 0x10, 0xffffffff);
	// 9. FCCM
	out_be32(fcc_dev.hwreg + 0x14, 0x000a0000);	/* RXF TXB */
	//10. SCPRR_H (FCC interrupt priority)
	//11. clear out SIPNR_L
	//12. SIMR_L (interrupts)
	//13. issue an INIT TX AND RX PARAMETERS command
	//14. set GFMR[ENT] and GFMR[ENR]

	ret = cpm_command(fcc_dev.cmd, CPM_CR_INIT_TRX);
	printk(KERN_ERR"(MILO)cpm_command ret=%d\n", ret);
	if (ret)
		goto errout;

	if ((fcc_dev.major = register_chrdev(0, "fcc", &fcc_dev_fops)) < 0) {
		printk(KERN_ERR "fcc_serial_probe: Unable to get major %d for device\n", fcc_dev.major);
		goto errout;
	}
	fcc_class = class_create(THIS_MODULE, "fcc");
	device_create(fcc_class, NULL,
		      MKDEV(fcc_dev.major, 0),
		      NULL,
		      "fcc");

	ent = proc_create("driver/fcc", 0, NULL, &fcc_proc_fops);
	if (!ent) {
		printk(KERN_WARNING "rtc: Failed to register with procfs.\n");
	}

	printk(KERN_INFO"(milo)ppc8270 fcc driver loaded\n");

	setbits32(fcc_dev.hwreg, FCC_GFMR_ENR|FCC_GFMR_ENT);

	/* Install our interrupt handler. */
	ret = request_irq(fcc_dev.irq, fcc_interrupt, IRQF_SHARED,
			DRV_MODULE_NAME, &fcc_dev);
	if (ret) {
		printk(KERN_ERR"(MILO)Could not allocate IRQ=%d\n",
				fcc_dev.irq);
		goto errout;
	}

	return 0;
errout:
	printk(KERN_ERR"(MILO)probe failed\n");
	return -1;
}

static int fcc_serial_remove(struct platform_device *ofdev)
{
	clrbits32(fcc_dev.hwreg, FCC_GFMR_ENR|FCC_GFMR_ENT);
	free_irq(fcc_dev.irq, &fcc_dev);

	proc_remove(ent);

	device_destroy(fcc_class, MKDEV(fcc_dev.major, 0));
	class_destroy(fcc_class);

	unregister_chrdev(fcc_dev.major, "fcc");

	return 0;
}

static const struct of_device_id fcc_serial_match[] = {
	{
		.compatible = "fsl,cpm2-fcc-serial",
	},
	{}
};
MODULE_DEVICE_TABLE(of, fcc_serial_match);

static struct platform_driver fcc_driver = {
	.driver = {
		.name = DRV_MODULE_NAME,
		.of_match_table = fcc_serial_match,
	},
	.probe = fcc_serial_probe,
	.remove = fcc_serial_remove,
};

module_platform_driver(fcc_driver);
