// SPDX-License-Identifier: GPL-2.0
/*
 * mt7621_pcm_raw - minimal MT7621 PCM controller driver for an FXS SLIC.
 *
 * Bring-up driver for the ZLT S12 Pro (Tozed s12-unisoc-plus) Le9641 SLIC.
 * It makes the MT7621 the PCM bus master (PCLK + FSYNC out on GPIO 9..12)
 * and moves 16-bit samples between userspace and channel 0 by PIO from an
 * hrtimer. No ASoC, no DMA: the goal is the shortest path to audible audio.
 *
 * Register map and clocking are from the 2020 "ralink pcm controller driver"
 * patch posted to openwrt-devel (Puyou Lu), tested on MT7621.
 *
 *   /dev/slicpcm           write: S16_LE mono 8 kHz samples -> PCM TX (ch0)
 *                          read:  PCM RX samples (ch0)
 *                          poll/O_NONBLOCK supported; ioctls FIONREAD and
 *                          TIOCOUTQ (queued samples * 2, i.e. bytes) and
 *                          TCFLSH (TCIFLUSH/TCOFLUSH/TCIOFLUSH: drop queued audio)
 *   debugfs slicpcm/regs   read: register dump + counters
 *                          write: "<offset> <value>" (hex) to poke a register
 *
 * Received audio passes through OSLEC, the kernel's line echo canceller
 * (module parameters ec, ec_mode, ec_pre, ec_taps). TCIFLUSH restarts it.
 */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/of.h>
#include <linux/hrtimer.h>
#include <linux/kfifo.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/gpio/driver.h>
#include <linux/poll.h>
#include <linux/termios.h>

#define SYSC_BASE		0x1E000000
#define SYSC_SYSCFG0		0x10
#define SYSC_CLKCFG1		0x30
#define SYSC_RSTCTRL		0x34
#define SYSC_GPIO_MODE		0x60
#define ANA_BASE		0x1E000F00
#define PCM_BASE		0x1E002000

#define MT7621_CLK_PCM_ID	12	/* dt-bindings/clock/mt7621-clk.h */
#define CLKCFG1_PCM		BIT(11)
#define RST_PCM			BIT(11)
#define GPIO_MODE_UART2_SHIFT	5
#define GPIO_MODE_UART2_MASK	(0x3 << GPIO_MODE_UART2_SHIFT)
#define GPIO_MODE_UART2_PCM	(0x2 << GPIO_MODE_UART2_SHIFT)

#define PCM_GLB_CFG		0x000
#define PCM_PCM_CFG		0x004
#define PCM_INT_STATUS		0x008
#define PCM_INT_EN		0x00C
#define PCM_CHA0_FF_STATUS	0x010
#define PCM_CHA0_CFG		0x020
#define PCM_CHB0_CFG		0x024
#define PCM_FSYNC_CFG		0x030
#define PCM_CHA0_CFG2		0x034
#define PCM_IP_INFO		0x040
#define PCM_DIVCOMP_CFG		0x050
#define PCM_DIVINT_CFG		0x054
#define PCM_DIGDELAY_CFG	0x060
#define PCM_CH0_FIFO		0x080

#define GLB_EN			BIT(31)
#define GLB_DFT_THRES		((4 << 20) | (4 << 16))
#define GLB_CH0			BIT(0)
#define PCMCFG_CLKOUT_EN	BIT(30)
#define PCMCFG_EXT_FSYNC	BIT(27)
#define PCMCFG_FSYNC_POL	BIT(25)
#define FSYNC_EN		BIT(31)
#define FSYNC_DRV_DT		BIT(29)
#define FSYNC_DRV_FSYNC		BIT(27)
#define DIVCOMP_CLKEN		BIT(31)
#define INT_TX_OVRUN		BIT(6)
#define INT_TX_UNRUN		BIT(5)
#define INT_RX_OVRUN		BIT(2)

/* ---- parameters: everything uncertain is a knob ---- */
static unsigned int bclk_hz = 2048000;	/* stock SLIC device profile: PCLK 2.048 MHz */
module_param(bclk_hz, uint, 0444);
/* Audio PLL output when setup_pll=1, as the divider sees it. The 2020
   driver's 294 MHz gave 8,033 frames/s (counted against NTP time over 20 s);
   this calibrated value gives divider 72 + 19/256 and 8,000.0 frames/s.
   Whether the PLL really runs at 295.2 MHz or the frame is one bit clock
   short (255) is not known; the frame rate is what matters. */
static unsigned int freqin = 295216000;
module_param(freqin, uint, 0444);
static bool setup_pll = true;		/* program the analog audio PLL (ANA_CTRL); required: without it no PCLK reaches the SLIC */
module_param(setup_pll, bool, 0444);
MODULE_PARM_DESC(setup_pll, "Program the analog audio PLL like the 2020 driver (writes ANA_CTRL)");
static unsigned int slot_mode = 3;	/* 3: FS = BCLK/8/32 -> 8 kHz at 2.048 MHz */
module_param(slot_mode, uint, 0444);
static unsigned int ts_start = 1;	/* CHA0 data start, in bit clocks after FSYNC */
module_param(ts_start, uint, 0444);
static bool fsync_pol = true;
module_param(fsync_pol, bool, 0444);
static unsigned int tick_us = 250;
module_param(tick_us, uint, 0444);
static unsigned int ff_free_mask = 0xF;	/* CHA0_FF_STATUS: TX free count field */
module_param(ff_free_mask, uint, 0644);
static unsigned int ff_free_shift;
module_param(ff_free_shift, uint, 0644);
static unsigned int ff_rx_shift = 4;	/* CHA0_FF_STATUS: RX available count field */
module_param(ff_rx_shift, uint, 0644);
static unsigned int sample_shift;	/* pack=0 only: 0 = sample in bits 15..0, 16 = bits 31..16 */
module_param(sample_shift, uint, 0644);
/* Measured: the FIFO drains ~4000 words/s at an 8 kHz frame rate, so each
   32-bit FIFO word carries two 16-bit samples. */
static bool pack = true;
module_param(pack, bool, 0644);
MODULE_PARM_DESC(pack, "Two 16-bit samples per 32-bit FIFO word");
static bool pack_hi_first;		/* loopback-verified: first (older) sample in bits 15..0 */
module_param(pack_hi_first, bool, 0644);

/* GPIO 32 ("slic_clk_switch" in the stock DT) selects whose PCM bus the SLIC
   sees: 0 = MT7621 (this driver), 1 = modem. -1 leaves it alone. */
static int clk_switch;
module_param(clk_switch, int, 0444);
MODULE_PARM_DESC(clk_switch, "slic_clk_switch (GPIO bank1 pin 0): 0 router PCM, 1 modem, -1 untouched");
static int clk_switch_gpio = -1;

/* ---- line echo canceller: OSLEC (kmod-echo, drivers/misc/echo) ----
   The SLIC's 2-wire hybrid leaks part of what we send back into what we
   receive (measured: 20 dB down, 2.1 ms long). The canceller runs here, in
   the tick, because only here do sent and received samples share one clock
   at a fixed offset. The kernel doesn't install OSLEC's header, so its API
   is declared below. */
#define ECHO_CAN_USE_ADAPTION	0x01
#define ECHO_CAN_USE_NLP	0x02
#define ECHO_CAN_USE_CNG	0x04
#define ECHO_CAN_USE_CLIP	0x08
#define ECHO_CAN_USE_RX_HPF	0x20
struct oslec_state;
struct oslec_state *oslec_create(int len, int adaption_mode);
void oslec_free(struct oslec_state *ec);
void oslec_flush(struct oslec_state *ec);
void oslec_adaption_mode(struct oslec_state *ec, int adaption_mode);
int16_t oslec_update(struct oslec_state *ec, int16_t tx, int16_t rx);

static bool ec = true;
module_param(ec, bool, 0644);
MODULE_PARM_DESC(ec, "Cancel line echo on received audio (re-enabling starts it afresh)");
static unsigned int ec_taps = 128;	/* filter length, samples: 16 ms */
module_param(ec_taps, uint, 0444);
static unsigned int ec_mode = ECHO_CAN_USE_ADAPTION | ECHO_CAN_USE_NLP |
			      ECHO_CAN_USE_CLIP | ECHO_CAN_USE_RX_HPF;
module_param(ec_mode, uint, 0644);
MODULE_PARM_DESC(ec_mode, "OSLEC flags: 1 adapt, 2 NLP, 4 comfort noise, 8 clip, 0x20 RX high-pass");
/* Each received sample is paired with the sample written to the TX FIFO in
   the same tick (ec_pre = 0), or ec_pre samples before it. A sample's echo
   can only come back after it was written, so at 0 the echo lands at tap
   (FIFO latency + SLIC delay - ec_pre), which must stay inside
   0..ec_taps-1. Raise ec_pre only if that delay grows past ec_taps. */
static unsigned int ec_pre;
module_param(ec_pre, uint, 0644);

#define TXH_LEN 1024			/* sent-sample history, power of 2 */
static s16 txh[TXH_LEN];
static u32 tx_n, rx_n;			/* samples written to / read from the FIFOs */
static u32 ec_base;			/* tx_n - rx_n when the canceller (re)started */
static struct oslec_state *ec_st;
static bool ec_on, ec_reset;
static unsigned int ec_mode_set;

static void __iomem *sysc, *pcm, *ana;
static struct clk *pcm_clk;
static u32 saved_gpio_mode;
static struct hrtimer tick;
static DEFINE_SPINLOCK(fifo_lock);
static DECLARE_KFIFO(txq, s16, 4096);
static DECLARE_KFIFO(rxq, s16, 4096);
static DECLARE_WAIT_QUEUE_HEAD(txwait);
static DECLARE_WAIT_QUEUE_HEAD(rxwait);
static unsigned long st_ticks, st_tx_words, st_tx_silence, st_rx_words;
static unsigned long st_tx_unrun, st_tx_ovrun, st_rx_ovrun;
static struct dentry *dbg_dir;

static inline u32 pcm_rd(u32 off) { return readl(pcm + off); }
static inline void pcm_wr(u32 off, u32 v) { writel(v, pcm + off); }
static inline void pcm_rmw(u32 off, u32 mask, u32 v)
{
	pcm_wr(off, (pcm_rd(off) & ~mask) | v);
}

static void mt7621_audio_pll_setup(void)
{
	u32 xtal = (readl(sysc + SYSC_SYSCFG0) >> 6) & 0x7;
	bool x20_40 = xtal <= 5;	/* 20 or 40 MHz crystal; else 25 MHz */
	static const int bits[] = { 0, 3, 8, 6, 5, 7, 17 };
	u32 r;
	int i;

	pr_info("slicpcm: audio PLL setup, xtal field %u; ANA before: 00=%08x 14=%08x 18=%08x 1c=%08x 20=%08x\n",
		xtal, readl(ana + 0x00), readl(ana + 0x14), readl(ana + 0x18),
		readl(ana + 0x1c), readl(ana + 0x20));
	writel(0x00008000, ana + 0x00);
	writel(0x01401d61, ana + 0x14);
	writel(0x38233d0e, ana + 0x18);
	writel(0x80120004, ana + 0x1c);
	writel(0x1c7dbf48, ana + 0x20);
	writel(0x00008800, ana + 0x00);
	writel(0x00008c00, ana + 0x00);

	r = readl(ana + 0x14) & ~0x0000ffc0;
	r |= (x20_40 ? 0x1d : 0x17) << 8;
	r |= 0x1 << 6;
	writel(r, ana + 0x14);

	r = readl(ana + 0x18) & ~0xf0773f00;
	r |= (0x3 << 28) | (0x2 << 20) | ((x20_40 ? 0x3 : 0x2) << 16) |
	     (0x3 << 12) | ((x20_40 ? 0xd : 0x7) << 8);
	writel(r, ana + 0x18);
	writel(x20_40 ? 0x1c7dbf48 : 0x1697cc39, ana + 0x20);

	writel(readl(ana + 0x1c) & ~BIT(31), ana + 0x1c);
	for (i = 0; i < ARRAY_SIZE(bits); i++)
		writel(readl(ana + 0x1c) | BIT(bits[i]), ana + 0x1c);
}

static int pcm_clock_enable(void)
{
	struct of_phandle_args args = { .args_count = 1, .args = { MT7621_CLK_PCM_ID } };

	args.np = of_find_compatible_node(NULL, NULL, "mediatek,mt7621-sysc");
	if (args.np) {
		pcm_clk = of_clk_get_from_provider(&args);
		of_node_put(args.np);
		if (!IS_ERR(pcm_clk)) {
			pr_info("slicpcm: pcm clk via provider, rate %lu Hz\n",
				clk_get_rate(pcm_clk));
			return clk_prepare_enable(pcm_clk);
		}
		pcm_clk = NULL;
	}
	pr_warn("slicpcm: no clk provider, setting CLKCFG1 bit directly\n");
	writel(readl(sysc + SYSC_CLKCFG1) | CLKCFG1_PCM, sysc + SYSC_CLKCFG1);
	return 0;
}

static int claim_clk_switch(void)
{
	struct gpio_device *gdev;
	int ret;

	if (clk_switch < 0)
		return 0;
	gdev = gpio_device_find_by_label("1e000600.gpio-bank1");
	if (!gdev) {
		pr_err("slicpcm: gpio bank1 not found\n");
		return -ENODEV;
	}
	clk_switch_gpio = gpio_device_get_base(gdev);	/* bank1 pin 0 */
	gpio_device_put(gdev);
	ret = gpio_request_one(clk_switch_gpio, clk_switch ? GPIOF_OUT_INIT_HIGH : GPIOF_OUT_INIT_LOW,
			       "slic_clk_switch");
	if (ret) {
		pr_err("slicpcm: cannot claim slic_clk_switch (gpio %d): %d\n", clk_switch_gpio, ret);
		clk_switch_gpio = -1;
		return ret;
	}
	pr_info("slicpcm: slic_clk_switch (gpio %d) = %d (%s PCM)\n", clk_switch_gpio,
		clk_switch, clk_switch ? "modem" : "router");
	return 0;
}

static int pcm_hw_start(void)
{
	unsigned long half = freqin / 2;
	u32 divint, divcomp, v;
	int ret;

	ret = pcm_clock_enable();
	if (ret)
		return ret;

	/* pulse the PCM block reset */
	writel(readl(sysc + SYSC_RSTCTRL) | RST_PCM, sysc + SYSC_RSTCTRL);
	udelay(10);
	writel(readl(sysc + SYSC_RSTCTRL) & ~RST_PCM, sysc + SYSC_RSTCTRL);
	udelay(10);

	if (setup_pll)
		mt7621_audio_pll_setup();

	/* GPIO 9..12 -> PCM function */
	saved_gpio_mode = readl(sysc + SYSC_GPIO_MODE);
	writel((saved_gpio_mode & ~GPIO_MODE_UART2_MASK) | GPIO_MODE_UART2_PCM,
	       sysc + SYSC_GPIO_MODE);

	/* master: drive PCLK and FSYNC ourselves */
	v = PCMCFG_CLKOUT_EN | (fsync_pol ? PCMCFG_FSYNC_POL : 0) | (slot_mode & 0x7);
	pcm_rmw(PCM_PCM_CFG, PCMCFG_CLKOUT_EN | PCMCFG_EXT_FSYNC | PCMCFG_FSYNC_POL |
		BIT(24) | 0x7, v);
	pcm_rmw(PCM_CHA0_CFG, 0x3FF, ts_start & 0x3FF);
	pcm_rmw(PCM_GLB_CFG, (7 << 20) | (7 << 16), GLB_DFT_THRES);
	pcm_rmw(PCM_FSYNC_CFG, FSYNC_EN | BIT(30) | FSYNC_DRV_DT | BIT(28) |
		FSYNC_DRV_FSYNC | 0x3FF, FSYNC_EN | FSYNC_DRV_DT | FSYNC_DRV_FSYNC | 1);

	/* BCLK = freqin / 2 / (DIVINT + DIVCOMP/256) */
	divint = half / bclk_hz;
	divcomp = ((half % bclk_hz) * 256) / bclk_hz;
	if (divint > 0x3FF)
		return -EINVAL;
	pcm_rmw(PCM_DIVINT_CFG, 0x3FF, divint);
	pcm_rmw(PCM_DIVCOMP_CFG, 0xFF, divcomp);
	pcm_rmw(PCM_DIVCOMP_CFG, DIVCOMP_CLKEN, DIVCOMP_CLKEN);
	pr_info("slicpcm: freqin %u -> bclk %u Hz (divint %u divcomp %u), slot_mode %u\n",
		freqin, bclk_hz, divint, divcomp, slot_mode);

	pcm_wr(PCM_INT_EN, 0);			/* polled */
	pcm_wr(PCM_INT_STATUS, 0xFF);		/* clear sticky flags */
	pcm_rmw(PCM_GLB_CFG, GLB_EN | 0xF, GLB_EN | GLB_CH0);
	return 0;
}

static void pcm_hw_stop(void)
{
	pcm_rmw(PCM_GLB_CFG, GLB_EN | 0xF, 0);
	pcm_rmw(PCM_DIVCOMP_CFG, DIVCOMP_CLKEN, 0);
	writel(saved_gpio_mode, sysc + SYSC_GPIO_MODE);
	if (pcm_clk) {
		clk_disable_unprepare(pcm_clk);
		clk_put(pcm_clk);
	}
}

static inline void tx_hist(s16 s)
{
	txh[tx_n++ & (TXH_LEN - 1)] = s;
}

/* one received sample: remove the echo of what was sent */
static inline s16 rx_clean(s16 s)
{
	s16 ref = txh[(rx_n++ + ec_base - READ_ONCE(ec_pre)) & (TXH_LEN - 1)];

	return ec_on ? oslec_update(ec_st, ref, s) : s;
}

/* called with fifo_lock held: follow the ec/ec_mode knobs, restart on request */
static void ec_follow(void)
{
	bool want = READ_ONCE(ec) && ec_st;
	unsigned int mode = READ_ONCE(ec_mode);

	if (want && (!ec_on || ec_reset)) {
		oslec_flush(ec_st);
		ec_base = tx_n - rx_n;
	}
	ec_on = want;
	ec_reset = false;
	if (ec_st && mode != ec_mode_set) {
		oslec_adaption_mode(ec_st, mode);
		ec_mode_set = mode;
	}
}

static enum hrtimer_restart pcm_tick(struct hrtimer *t)
{
	u32 st = pcm_rd(PCM_CHA0_FF_STATUS);
	u32 free = (st >> ff_free_shift) & ff_free_mask;
	u32 avail = (st >> ff_rx_shift) & 0xF;
	u32 ist = pcm_rd(PCM_INT_STATUS);
	unsigned long flags;
	bool woke_tx = false, woke_rx = false;

	if (ist) {
		pcm_wr(PCM_INT_STATUS, ist);
		st_tx_unrun += !!(ist & INT_TX_UNRUN);
		st_tx_ovrun += !!(ist & INT_TX_OVRUN);
		st_rx_ovrun += !!(ist & INT_RX_OVRUN);
	}

	spin_lock_irqsave(&fifo_lock, flags);
	ec_follow();
	while (free--) {
		s16 a = 0, b = 0;
		u32 w;

		if (pack) {
			s16 pair[2];

			if (kfifo_len(&txq) >= 2 && kfifo_out(&txq, pair, 2) == 2) {
				a = pair[0];
				b = pair[1];
				st_tx_words++;
				woke_tx = true;
			} else {
				st_tx_silence++;
			}
			w = pack_hi_first ? ((u32)(u16)a << 16) | (u16)b
					  : ((u32)(u16)b << 16) | (u16)a;
			tx_hist(a);
			tx_hist(b);
		} else {
			if (kfifo_get(&txq, &a)) {
				st_tx_words++;
				woke_tx = true;
			} else {
				st_tx_silence++;
			}
			w = ((u32)(u16)a) << sample_shift;
			tx_hist(a);
		}
		pcm_wr(PCM_CH0_FIFO, w);
	}
	while (avail--) {
		u32 w = pcm_rd(PCM_CH0_FIFO);

		if (pack) {
			s16 hi = (s16)(w >> 16), lo = (s16)(w & 0xFFFF);

			kfifo_put(&rxq, rx_clean(pack_hi_first ? hi : lo));
			kfifo_put(&rxq, rx_clean(pack_hi_first ? lo : hi));
		} else {
			kfifo_put(&rxq, rx_clean((s16)(w >> sample_shift)));
		}
		st_rx_words++;
		woke_rx = true;
	}
	spin_unlock_irqrestore(&fifo_lock, flags);

	if (woke_tx)
		wake_up_interruptible(&txwait);
	if (woke_rx)
		wake_up_interruptible(&rxwait);
	st_ticks++;
	hrtimer_forward_now(t, ns_to_ktime((u64)tick_us * 1000));
	return HRTIMER_RESTART;
}

static ssize_t slicpcm_write(struct file *f, const char __user *buf,
			     size_t len, loff_t *off)
{
	size_t done = 0;

	while (done + 1 < len) {
		s16 chunk[128];
		size_t n = min_t(size_t, (len - done) / 2, ARRAY_SIZE(chunk));
		unsigned int in;
		unsigned long flags;

		if (kfifo_avail(&txq) < n && (f->f_flags & O_NONBLOCK))
			return done ? done : -EAGAIN;
		if (wait_event_interruptible(txwait, kfifo_avail(&txq) >= n))
			return done ? done : -ERESTARTSYS;
		if (copy_from_user(chunk, buf + done, n * 2))
			return -EFAULT;
		spin_lock_irqsave(&fifo_lock, flags);
		in = kfifo_in(&txq, chunk, n);
		spin_unlock_irqrestore(&fifo_lock, flags);
		done += in * 2;
	}
	return done;
}

static ssize_t slicpcm_read(struct file *f, char __user *buf,
			    size_t len, loff_t *off)
{
	s16 chunk[128];
	size_t n = min_t(size_t, len / 2, ARRAY_SIZE(chunk));
	unsigned long flags;
	unsigned int got;

	if (kfifo_is_empty(&rxq) && (f->f_flags & O_NONBLOCK))
		return -EAGAIN;
	if (wait_event_interruptible(rxwait, !kfifo_is_empty(&rxq)))
		return -ERESTARTSYS;
	spin_lock_irqsave(&fifo_lock, flags);
	got = kfifo_out(&rxq, chunk, n);
	spin_unlock_irqrestore(&fifo_lock, flags);
	if (copy_to_user(buf, chunk, got * 2))
		return -EFAULT;
	return got * 2;
}

/* readable: RX samples queued; writable: room for at least 20 ms of TX */
static __poll_t slicpcm_poll(struct file *f, poll_table *wait)
{
	__poll_t mask = 0;

	poll_wait(f, &rxwait, wait);
	poll_wait(f, &txwait, wait);
	if (!kfifo_is_empty(&rxq))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (kfifo_avail(&txq) >= 160)
		mask |= EPOLLOUT | EPOLLWRNORM;
	return mask;
}

static long slicpcm_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	unsigned long flags;
	int n;

	switch (cmd) {
	case FIONREAD:
		n = kfifo_len(&rxq) * 2;
		return put_user(n, (int __user *)arg);
	case TIOCOUTQ:
		n = kfifo_len(&txq) * 2;
		return put_user(n, (int __user *)arg);
	case TCFLSH:
		if (arg > TCIOFLUSH)
			return -EINVAL;
		spin_lock_irqsave(&fifo_lock, flags);
		if (arg == TCIFLUSH || arg == TCIOFLUSH) {
			kfifo_reset(&rxq);
			ec_reset = true;	/* a new call: the echo canceller starts afresh */
		}
		if (arg == TCOFLUSH || arg == TCIOFLUSH)
			kfifo_reset(&txq);
		spin_unlock_irqrestore(&fifo_lock, flags);
		wake_up_interruptible(&txwait);
		return 0;
	}
	return -ENOTTY;
}

static const struct file_operations slicpcm_fops = {
	.owner = THIS_MODULE,
	.write = slicpcm_write,
	.read = slicpcm_read,
	.poll = slicpcm_poll,
	.unlocked_ioctl = slicpcm_ioctl,
};

static struct miscdevice slicpcm_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "slicpcm",
	.fops = &slicpcm_fops,
};

static int regs_show(struct seq_file *m, void *v)
{
	static const struct { u32 off; const char *name; } r[] = {
		{ PCM_GLB_CFG, "GLB_CFG" }, { PCM_PCM_CFG, "PCM_CFG" },
		{ PCM_INT_STATUS, "INT_STATUS" }, { PCM_INT_EN, "INT_EN" },
		{ PCM_CHA0_FF_STATUS, "CHA0_FF_STATUS" }, { PCM_CHA0_CFG, "CHA0_CFG" },
		{ PCM_CHB0_CFG, "CHB0_CFG" }, { PCM_FSYNC_CFG, "FSYNC_CFG" },
		{ PCM_CHA0_CFG2, "CHA0_CFG2" }, { PCM_IP_INFO, "IP_INFO" },
		{ PCM_DIVCOMP_CFG, "DIVCOMP_CFG" }, { PCM_DIVINT_CFG, "DIVINT_CFG" },
		{ PCM_DIGDELAY_CFG, "DIGDELAY_CFG" },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(r); i++)
		seq_printf(m, "%03x %-15s %08x\n", r[i].off, r[i].name, pcm_rd(r[i].off));
	seq_printf(m, "sysc GPIO_MODE %08x CLKCFG1 %08x RSTCTRL %08x SYSCFG0 %08x\n",
		   readl(sysc + SYSC_GPIO_MODE), readl(sysc + SYSC_CLKCFG1),
		   readl(sysc + SYSC_RSTCTRL), readl(sysc + SYSC_SYSCFG0));
	seq_printf(m, "ana 00 %08x 14 %08x 18 %08x 1c %08x 20 %08x\n",
		   readl(ana + 0x00), readl(ana + 0x14), readl(ana + 0x18),
		   readl(ana + 0x1c), readl(ana + 0x20));
	seq_printf(m, "ticks %lu tx_words %lu tx_silence %lu rx_words %lu\n",
		   st_ticks, st_tx_words, st_tx_silence, st_rx_words);
	seq_printf(m, "tx_unrun %lu tx_ovrun %lu rx_ovrun %lu txq %u rxq %u\n",
		   st_tx_unrun, st_tx_ovrun, st_rx_ovrun, kfifo_len(&txq), kfifo_len(&rxq));
	seq_printf(m, "ec %s taps %u mode %#x pre %u base %u (tx_n - rx_n now %u)\n",
		   !ec_st ? "unavailable" : ec_on ? "on" : "off", ec_taps, ec_mode_set,
		   ec_pre, ec_base, tx_n - rx_n);
	return 0;
}

static int regs_open(struct inode *inode, struct file *file)
{
	return single_open(file, regs_show, NULL);
}

static ssize_t regs_write(struct file *f, const char __user *ubuf,
			  size_t len, loff_t *off)
{
	char buf[32];
	u32 reg, val;

	if (len >= sizeof(buf) || copy_from_user(buf, ubuf, len))
		return -EINVAL;
	buf[len] = 0;
	if (sscanf(buf, "%x %x", &reg, &val) != 2 || reg > 0x7FC || (reg & 3))
		return -EINVAL;
	pcm_wr(reg, val);
	return len;
}

static const struct file_operations regs_fops = {
	.owner = THIS_MODULE,
	.open = regs_open,
	.read = seq_read,
	.write = regs_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init slicpcm_init(void)
{
	int ret;

	INIT_KFIFO(txq);
	INIT_KFIFO(rxq);
	sysc = ioremap(SYSC_BASE, 0x100);
	pcm = ioremap(PCM_BASE, 0x800);
	ana = ioremap(ANA_BASE, 0x100);
	if (!sysc || !pcm || !ana) {
		ret = -ENOMEM;
		goto err_unmap;
	}

	ec_st = oslec_create(ec_taps, ec_mode);
	if (ec_st)
		ec_mode_set = ec_mode;
	else
		pr_warn("slicpcm: no memory for the echo canceller, running without\n");

	ret = claim_clk_switch();
	if (ret)
		goto err_unmap;

	ret = pcm_hw_start();
	if (ret)
		goto err_gpio;

	ret = misc_register(&slicpcm_dev);
	if (ret)
		goto err_stop;

	dbg_dir = debugfs_create_dir("slicpcm", NULL);
	debugfs_create_file("regs", 0600, dbg_dir, NULL, &regs_fops);

	hrtimer_init(&tick, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	tick.function = pcm_tick;
	hrtimer_start(&tick, ns_to_ktime((u64)tick_us * 1000), HRTIMER_MODE_REL);
	pr_info("slicpcm: running, /dev/slicpcm ready, IP_INFO %08x\n", pcm_rd(PCM_IP_INFO));
	return 0;

err_stop:
	pcm_hw_stop();
err_gpio:
	if (clk_switch_gpio >= 0)
		gpio_free(clk_switch_gpio);
err_unmap:
	if (ec_st)
		oslec_free(ec_st);
	if (ana)
		iounmap(ana);
	if (pcm)
		iounmap(pcm);
	if (sysc)
		iounmap(sysc);
	return ret;
}

static void __exit slicpcm_exit(void)
{
	hrtimer_cancel(&tick);
	debugfs_remove_recursive(dbg_dir);
	misc_deregister(&slicpcm_dev);
	pcm_hw_stop();
	if (clk_switch_gpio >= 0)
		gpio_free(clk_switch_gpio);
	if (ec_st)
		oslec_free(ec_st);
	iounmap(ana);
	iounmap(pcm);
	iounmap(sysc);
}

module_init(slicpcm_init);
module_exit(slicpcm_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Minimal MT7621 PCM master + PIO sample pipe for an FXS SLIC");
