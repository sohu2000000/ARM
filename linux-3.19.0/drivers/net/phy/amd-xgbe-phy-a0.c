/*
 * AMD 10Gb Ethernet PHY driver
 *
 * This file is available to you under your choice of the following two
 * licenses:
 *
 * License 1: GPLv2
 *
 * Copyright (c) 2014 Advanced Micro Devices, Inc.
 *
 * This file is free software; you may copy, redistribute and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or (at
 * your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * License 2: Modified BSD
 *
 * Copyright (c) 2014 Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Advanced Micro Devices, Inc. nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/unistd.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mii.h>
#include <linux/ethtool.h>
#include <linux/phy.h>
#include <linux/mdio.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_device.h>
#include <linux/uaccess.h>
#include <linux/bitops.h>
#include <linux/property.h>
#include <linux/acpi.h>
#include <linux/irq.h>

MODULE_AUTHOR("Tom Lendacky <thomas.lendacky@amd.com>");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION("0.0.0-a");
MODULE_DESCRIPTION("AMD 10GbE (amd-xgbe) PHY driver");

#define XGBE_PHY_ID	0x7996ced0
#define XGBE_PHY_MASK	0xfffffff0

#define XGBE_PHY_SERDES_RETRY		32
#define XGBE_PHY_CHANNEL_PROPERTY	"amd,serdes-channel"
#define XGBE_PHY_SPEEDSET_PROPERTY	"amd,speed-set"
#define XGBE_PHY_BLWC_PROPERTY		"amd,serdes-blwc"
#define XGBE_PHY_CDR_RATE_PROPERTY	"amd,serdes-cdr-rate"
#define XGBE_PHY_PQ_SKEW_PROPERTY	"amd,serdes-pq-skew"
#define XGBE_PHY_TX_AMP_PROPERTY	"amd,serdes-tx-amp"

#define XGBE_PHY_SPEEDS			3
#define XGBE_PHY_SPEED_1000		0
#define XGBE_PHY_SPEED_2500		1
#define XGBE_PHY_SPEED_10000		2

#define XGBE_AN_INT_CMPLT		0x01
#define XGBE_AN_INC_LINK		0x02
#define XGBE_AN_PG_RCV			0x04
#define XGBE_AN_INT_MASK		0x07

#define XNP_MCF_NULL_MESSAGE		0x001
#define XNP_ACK_PROCESSED		BIT(12)
#define XNP_MP_FORMATTED		BIT(13)
#define XNP_NP_EXCHANGE			BIT(15)

#define XGBE_PHY_RATECHANGE_COUNT	500

#define XGBE_PHY_KR_TRAINING_START	0x01
#define XGBE_PHY_KR_TRAINING_ENABLE	0x02

#define XGBE_PHY_FEC_ENABLE		0x01
#define XGBE_PHY_FEC_FORWARD		0x02
#define XGBE_PHY_FEC_MASK		0x03

#ifndef MDIO_PMA_10GBR_PMD_CTRL
#define MDIO_PMA_10GBR_PMD_CTRL		0x0096
#endif

#ifndef MDIO_PMA_10GBR_FEC_ABILITY
#define MDIO_PMA_10GBR_FEC_ABILITY	0x00aa
#endif

#ifndef MDIO_PMA_10GBR_FEC_CTRL
#define MDIO_PMA_10GBR_FEC_CTRL		0x00ab
#endif

#ifndef MDIO_AN_XNP
#define MDIO_AN_XNP			0x0016
#endif

#ifndef MDIO_AN_LPX
#define MDIO_AN_LPX			0x0019
#endif

#ifndef MDIO_AN_INTMASK
#define MDIO_AN_INTMASK			0x8001
#endif

#ifndef MDIO_AN_INT
#define MDIO_AN_INT			0x8002
#endif

#ifndef MDIO_AN_KR_CTRL
#define MDIO_AN_KR_CTRL			0x8003
#endif

#ifndef MDIO_CTRL1_SPEED1G
#define MDIO_CTRL1_SPEED1G		(MDIO_CTRL1_SPEED10G & ~BMCR_SPEED100)
#endif

#ifndef MDIO_KR_CTRL_PDETECT
#define MDIO_KR_CTRL_PDETECT		0x01
#endif

#define GET_BITS(_var, _index, _width)					\
	(((_var) >> (_index)) & ((0x1 << (_width)) - 1))

#define SET_BITS(_var, _index, _width, _val)				\
do {									\
	(_var) &= ~(((0x1 << (_width)) - 1) << (_index));		\
	(_var) |= (((_val) & ((0x1 << (_width)) - 1)) << (_index));	\
} while (0)

#define XCMU_IOREAD(_priv, _reg)					\
	ioread16((_priv)->cmu_regs + _reg)

#define XCMU_IOWRITE(_priv, _reg, _val)					\
	iowrite16((_val), (_priv)->cmu_regs + _reg)

#define XRXTX_IOREAD(_priv, _reg)					\
	ioread16((_priv)->rxtx_regs + _reg)

#define XRXTX_IOREAD_BITS(_priv, _reg, _field)				\
	GET_BITS(XRXTX_IOREAD((_priv), _reg),				\
		 _reg##_##_field##_INDEX,				\
		 _reg##_##_field##_WIDTH)

#define XRXTX_IOWRITE(_priv, _reg, _val)				\
	iowrite16((_val), (_priv)->rxtx_regs + _reg)

#define XRXTX_IOWRITE_BITS(_priv, _reg, _field, _val)			\
do {									\
	u16 reg_val = XRXTX_IOREAD((_priv), _reg);			\
	SET_BITS(reg_val,						\
		 _reg##_##_field##_INDEX,				\
		 _reg##_##_field##_WIDTH, (_val));			\
	XRXTX_IOWRITE((_priv), _reg, reg_val);				\
} while (0)

/* SerDes CMU register offsets */
#define CMU_REG15			0x003c
#define CMU_REG16			0x0040

/* SerDes CMU register entry bit positions and sizes */
#define CMU_REG16_TX_RATE_CHANGE_BASE	15
#define CMU_REG16_RX_RATE_CHANGE_BASE	14
#define CMU_REG16_RATE_CHANGE_DECR	2

/* SerDes RxTx register offsets */
#define RXTX_REG2			0x0008
#define RXTX_REG3			0x000c
#define RXTX_REG5			0x0014
#define RXTX_REG6			0x0018
#define RXTX_REG20			0x0050
#define RXTX_REG53			0x00d4
#define RXTX_REG114			0x01c8
#define RXTX_REG115			0x01cc
#define RXTX_REG142			0x0238

/* SerDes RxTx register entry bit positions and sizes */
#define RXTX_REG2_RESETB_INDEX			15
#define RXTX_REG2_RESETB_WIDTH			1
#define RXTX_REG3_TX_DATA_RATE_INDEX		14
#define RXTX_REG3_TX_DATA_RATE_WIDTH		2
#define RXTX_REG3_TX_WORD_MODE_INDEX		11
#define RXTX_REG3_TX_WORD_MODE_WIDTH		3
#define RXTX_REG5_TXAMP_CNTL_INDEX		7
#define RXTX_REG5_TXAMP_CNTL_WIDTH		4
#define RXTX_REG6_RX_DATA_RATE_INDEX		9
#define RXTX_REG6_RX_DATA_RATE_WIDTH		2
#define RXTX_REG6_RX_WORD_MODE_INDEX		11
#define RXTX_REG6_RX_WORD_MODE_WIDTH		3
#define RXTX_REG20_BLWC_ENA_INDEX		2
#define RXTX_REG20_BLWC_ENA_WIDTH		1
#define RXTX_REG53_RX_PLLSELECT_INDEX		15
#define RXTX_REG53_RX_PLLSELECT_WIDTH		1
#define RXTX_REG53_TX_PLLSELECT_INDEX		14
#define RXTX_REG53_TX_PLLSELECT_WIDTH		1
#define RXTX_REG53_PI_SPD_SEL_CDR_INDEX		10
#define RXTX_REG53_PI_SPD_SEL_CDR_WIDTH		4
#define RXTX_REG114_PQ_REG_INDEX		9
#define RXTX_REG114_PQ_REG_WIDTH		7
#define RXTX_REG115_FORCE_LAT_CAL_START_INDEX	2
#define RXTX_REG115_FORCE_LAT_CAL_START_WIDTH	1
#define RXTX_REG115_FORCE_SUM_CAL_START_INDEX	1
#define RXTX_REG115_FORCE_SUM_CAL_START_WIDTH	1
#define RXTX_REG142_SUM_CALIB_DONE_INDEX	15
#define RXTX_REG142_SUM_CALIB_DONE_WIDTH	1
#define RXTX_REG142_SUM_CALIB_ERR_INDEX		14
#define RXTX_REG142_SUM_CALIB_ERR_WIDTH		1
#define RXTX_REG142_LAT_CALIB_DONE_INDEX	11
#define RXTX_REG142_LAT_CALIB_DONE_WIDTH	1

#define RXTX_FULL_RATE				0x0
#define RXTX_HALF_RATE				0x1
#define RXTX_FIFTH_RATE				0x3
#define RXTX_66BIT_WORD				0x7
#define RXTX_10BIT_WORD				0x1
#define RXTX_10G_BLWC				0x0
#define RXTX_1G_BLWC				0x1
#define RXTX_10G_TX_AMP				0xa
#define RXTX_1G_TX_AMP				0xf
#define RXTX_10G_CDR				0x7
#define RXTX_1G_CDR				0x2
#define RXTX_10G_PLL				0x1
#define RXTX_1G_PLL				0x0
#define RXTX_10G_PQ				0x1e
#define RXTX_1G_PQ				0xa

DEFINE_SPINLOCK(cmu_lock);

static const u32 amd_xgbe_phy_serdes_blwc[] = {
	RXTX_1G_BLWC,
	RXTX_1G_BLWC,
	RXTX_10G_BLWC,
};

static const u32 amd_xgbe_phy_serdes_cdr_rate[] = {
	RXTX_1G_CDR,
	RXTX_1G_CDR,
	RXTX_10G_CDR,
};

static const u32 amd_xgbe_phy_serdes_pq_skew[] = {
	RXTX_1G_PQ,
	RXTX_1G_PQ,
	RXTX_10G_PQ,
};

static const u32 amd_xgbe_phy_serdes_tx_amp[] = {
	RXTX_1G_TX_AMP,
	RXTX_1G_TX_AMP,
	RXTX_10G_TX_AMP,
};

enum amd_xgbe_phy_an {
	AMD_XGBE_AN_READY = 0,
	AMD_XGBE_AN_PAGE_RECEIVED,
	AMD_XGBE_AN_INCOMPAT_LINK,
	AMD_XGBE_AN_COMPLETE,
	AMD_XGBE_AN_NO_LINK,
	AMD_XGBE_AN_ERROR,
};

enum amd_xgbe_phy_rx {
	AMD_XGBE_RX_BPA = 0,
	AMD_XGBE_RX_XNP,
	AMD_XGBE_RX_COMPLETE,
	AMD_XGBE_RX_ERROR,
};

enum amd_xgbe_phy_mode {
	AMD_XGBE_MODE_KR,
	AMD_XGBE_MODE_KX,
};

enum amd_xgbe_phy_speedset {
	AMD_XGBE_PHY_SPEEDSET_1000_10000 = 0,
	AMD_XGBE_PHY_SPEEDSET_2500_10000,
};

struct amd_xgbe_phy_priv {
	struct platform_device *pdev;
	struct acpi_device *adev;
	struct device *dev;

	struct phy_device *phydev;

	/* SerDes related mmio resources */
	struct resource *rxtx_res;
	struct resource *cmu_res;

	/* SerDes related mmio registers */
	void __iomem *rxtx_regs;	/* SerDes Rx/Tx CSRs */
	void __iomem *cmu_regs;		/* SerDes CMU CSRs */

	int an_irq;
	char an_irq_name[IFNAMSIZ + 32];
	struct work_struct an_irq_work;
	unsigned int an_irq_allocated;

	unsigned int serdes_channel;
	unsigned int speed_set;

	/* Maintain link status for re-starting auto-negotiation */
	unsigned int link;

	/* SerDes UEFI configurable settings.
	 *   Switching between modes/speeds requires new values for some
	 *   SerDes settings.  The values can be supplied as device
	 *   properties in array format.  The first array entry is for
	 *   1GbE, second for 2.5GbE and third for 10GbE
	 */
	u32 serdes_blwc[XGBE_PHY_SPEEDS];
	u32 serdes_cdr_rate[XGBE_PHY_SPEEDS];
	u32 serdes_pq_skew[XGBE_PHY_SPEEDS];
	u32 serdes_tx_amp[XGBE_PHY_SPEEDS];

	/* Auto-negotiation state machine support */
	struct mutex an_mutex;
	enum amd_xgbe_phy_an an_result;
	enum amd_xgbe_phy_an an_state;
	enum amd_xgbe_phy_rx kr_state;
	enum amd_xgbe_phy_rx kx_state;
	struct work_struct an_work;
	struct workqueue_struct *an_workqueue;
	unsigned int an_supported;
	unsigned int parallel_detect;
	unsigned int fec_ability;

	unsigned int lpm_ctrl;		/* CTRL1 for resume */
};

static int amd_xgbe_an_enable_kr_training(struct phy_device *phydev)
{
	int ret;

	ret = phy_read_mmd(phydev, MDIO_MMD_PMAPMD, MDIO_PMA_10GBR_PMD_CTRL);
	if (ret < 0)
		return ret;

	ret |= XGBE_PHY_KR_TRAINING_ENABLE;
	phy_write_mmd(phydev, MDIO_MMD_PMAPMD, MDIO_PMA_10GBR_PMD_CTRL, ret);

	return 0;
}

static int amd_xgbe_an_disable_kr_training(struct phy_device *phydev)
{
	int ret;

	ret = phy_read_mmd(phydev, MDIO_MMD_PMAPMD, MDIO_PMA_10GBR_PMD_CTRL);
	if (ret < 0)
		return ret;

	ret &= ~XGBE_PHY_KR_TRAINING_ENABLE;
	phy_write_mmd(phydev, MDIO_MMD_PMAPMD, MDIO_PMA_10GBR_PMD_CTRL, ret);

	return 0;
}

static int amd_xgbe_phy_pcs_power_cycle(struct phy_device *phydev)
{
	int ret;

	ret = phy_read_mmd(phydev, MDIO_MMD_PCS, MDIO_CTRL1);
	if (ret < 0)
		return ret;

	ret |= MDIO_CTRL1_LPOWER;
	phy_write_mmd(phydev, MDIO_MMD_PCS, MDIO_CTRL1, ret);

	usleep_range(75, 100);

	ret &= ~MDIO_CTRL1_LPOWER;
	phy_write_mmd(phydev, MDIO_MMD_PCS, MDIO_CTRL1, ret);

	return 0;
}

static void amd_xgbe_phy_serdes_start_ratechange(struct phy_device *phydev)
{
	struct amd_xgbe_phy_priv *priv = phydev->priv;
	u16 val, mask;

	/* Assert Rx and Tx ratechange in CMU_reg16 */
	val = XCMU_IOREAD(priv, CMU_REG16);

	mask = (1 << (CMU_REG16_TX_RATE_CHANGE_BASE -
		      (priv->serdes_channel * CMU_REG16_RATE_CHANGE_DECR))) |
	       (1 << (CMU_REG16_RX_RATE_CHANGE_BASE -
		      (priv->serdes_channel * CMU_REG16_RATE_CHANGE_DECR)));
	val |= mask;

	XCMU_IOWRITE(priv, CMU_REG16, val);
}

static void amd_xgbe_phy_serdes_complete_ratechange(struct phy_device *phydev)
{
	struct amd_xgbe_phy_priv *priv = phydev->priv;
	u16 val, mask;
	unsigned int wait;

	/* Release Rx and Tx ratechange for proper channel in CMU_reg16 */
	val = XCMU_IOREAD(priv, CMU_REG16);

	mask = (1 << (CMU_REG16_TX_RATE_CHANGE_BASE -
		      (priv->serdes_channel * CMU_REG16_RATE_CHANGE_DECR))) |
	       (1 << (CMU_REG16_RX_RATE_CHANGE_BASE -
		      (priv->serdes_channel * CMU_REG16_RATE_CHANGE_DECR)));
	val &= ~mask;

	XCMU_IOWRITE(priv, CMU_REG16, val);

	/* Wait for Rx and Tx ready in CMU_reg15 */
	mask = (1 << priv->serdes_channel) |
	       (1 << (priv->serdes_channel + 8));
	wait = XGBE_PHY_RATECHANGE_COUNT;
	while (wait--) {
		udelay(50);

		val = XCMU_IOREAD(priv, CMU_REG15);
		if ((val & mask) == mask)
			return;
	}

	netdev_dbg(phydev->attached_dev, "SerDes rx/tx not ready (%#hx)\n",
		   val);
}

static int amd_xgbe_phy_xgmii_mode(struct phy_device *phydev)
{
	struct amd_xgbe_phy_priv *priv = phydev->priv;
	int ret;

	/* Disable KR training */
	ret = amd_xgbe_an_disable_kr_training(phydev);
	if (ret < 0)
		return ret;

	/* Set PCS to KR/10G speed */
	ret = phy_read_mmd(phydev, MDIO_MMD_PCS, MDIO_CTRL2);
	if (ret < 0)
		return ret;

	ret &= ~MDIO_PCS_CTRL2_TYPE;
	ret |= MDIO_PCS_CTRL2_10GBR;
	phy_write_mmd(phydev, MDIO_MMD_PCS, MDIO_CTRL2, ret);

	ret = phy_read_mmd(phydev, MDIO_MMD_PCS, MDIO_CTRL1);
	if (ret < 0)
		return ret;

	ret &= ~MDIO_CTRL1_SPEEDSEL;
	ret |= MDIO_CTRL1_SPEED10G;
	phy_write_mmd(phydev, MDIO_MMD_PCS, MDIO_CTRL1, ret);

	ret = amd_xgbe_phy_pcs_power_cycle(phydev);
	if (ret < 0)
		return ret;

	/* Set SerDes to 10G speed */
	spin_lock(&cmu_lock);

	amd_xgbe_phy_serdes_start_ratechange(phydev);

	XRXTX_IOWRITE_BITS(priv, RXTX_REG3, TX_DATA_RATE, RXTX_FULL_RATE);
	XRXTX_IOWRITE_BITS(priv, RXTX_REG3, TX_WORD_MODE, RXTX_66BIT_WORD);

	XRXTX_IOWRITE_BITS(priv, RXTX_REG5, TXAMP_CNTL,
			   priv->serdes_tx_amp[XGBE_PHY_SPEED_10000]);

	XRXTX_IOWRITE_BITS(priv, RXTX_REG6, RX_DATA_RATE, RXTX_FULL_RATE);
	XRXTX_IOWRITE_BITS(priv, RXTX_REG6, RX_WORD_MODE, RXTX_66BIT_WORD);

	XRXTX_IOWRITE_BITS(priv, RXTX_REG20, BLWC_ENA,
			   priv->serdes_blwc[XGBE_PHY_SPEED_10000]);

	XRXTX_IOWRITE_BITS(priv, RXTX_REG53, RX_PLLSELECT, RXTX_10G_PLL);
	XRXTX_IOWRITE_BITS(priv, RXTX_REG53, TX_PLLSELECT, RXTX_10G_PLL);
	XRXTX_IOWRITE_BITS(priv, RXTX_REG53, PI_SPD_SEL_CDR,
			   priv->serdes_cdr_rate[XGBE_PHY_SPEED_10000]);

	XRXTX_IOWRITE_BITS(priv, RXTX_REG114, PQ_REG,
			   priv->serdes_pq_skew[XGBE_PHY_SPEED_10000]);

	amd_xgbe_phy_serdes_complete_ratechange(phydev);

	spin_unlock(&cmu_lock);

	return 0;
}

static int amd_xgbe_phy_gmii_2500_mode(struct phy_device *phydev)
{
	struct amd_xgbe_phy_priv *priv = phydev->priv;
	int ret;

	/* Disable KR training */
	ret = amd_xgbe_an_disable_kr_training(phydev);
	if (ret < 0)
		return ret;

	/* Set PCS to KX/1G speed */
	ret = phy_read_mmd(phydev, MDIO_MMD_PCS, MDIO_CTRL2);
	if (ret < 0)
		return ret;

	ret &= ~MDIO_PCS_CTRL2_TYPE;
	ret |= MDIO_PCS_CTRL2_10GBX;
	phy_write_mmd(phydev, MDIO_MMD_PCS, MDIO_CTRL2, ret);

	ret = phy_read_mmd(phydev, MDIO_MMD_PCS, MDIO_CTRL1);
	if (ret < 0)
		return ret;

	ret &= ~MDIO_CTRL1_SPEEDSEL;
	ret |= MDIO_CTRL1_SPEED1G;
	phy_write_mmd(phydev, MDIO_MMD_PCS, MDIO_CTRL1, ret);

	ret = amd_xgbe_phy_pcs_power_cycle(phydev);
	if (ret < 0)
		return ret;

	/* Set SerDes to 2.5G speed */
	spin_lock(&cmu_lock);

	amd_xgbe_phy_serdes_start_ratechange(phydev);

	XRXTX_IOWRITE_BITS(priv, RXTX_REG3, TX_DATA_RATE, RXTX_HALF_RATE);
	XRXTX_IOWRITE_BITS(priv, RXTX_REG3, TX_WORD_MODE, RXTX_10BIT_WORD);

	XRXTX_IOWRITE_BITS(priv, RXTX_REG5, TXAMP_CNTL,
			   priv->serdes_tx_amp[XGBE_PHY_SPEED_2500]);

	XRXTX_IOWRITE_BITS(priv, RXTX_REG6, RX_DATA_RATE, RXTX_HALF_RATE);
	XRXTX_IOWRITE_BITS(priv, RXTX_REG6, RX_WORD_MODE, RXTX_10BIT_WORD);

	XRXTX_IOWRITE_BITS(priv, RXTX_REG20, BLWC_ENA,
			   priv->serdes_blwc[XGBE_PHY_SPEED_2500]);

	XRXTX_IOWRITE_BITS(priv, RXTX_REG53, RX_PLLSELECT, RXTX_1G_PLL);
	XRXTX_IOWRITE_BITS(priv, RXTX_REG53, TX_PLLSELECT, RXTX_1G_PLL);
	XRXTX_IOWRITE_BITS(priv, RXTX_REG53, PI_SPD_SEL_CDR,
			   priv->serdes_cdr_rate[XGBE_PHY_SPEED_2500]);

	XRXTX_IOWRITE_BITS(priv, RXTX_REG114, PQ_REG,
			   priv->serdes_pq_skew[XGBE_PHY_SPEED_2500]);

	amd_xgbe_phy_serdes_complete_ratechange(phydev);

	spin_unlock(&cmu_lock);

	return 0;
}

static int amd_xgbe_phy_gmii_mode(struct phy_device *phydev)
{
	struct amd_xgbe_phy_priv *priv = phydev->priv;
	int ret;

	/* Disable KR training */
	ret = amd_xgbe_an_disable_kr_training(phydev);
	if (ret < 0)
		return ret;

	/* Set PCS to KX/1G speed */
	ret = phy_read_mmd(phydev, MDIO_MMD_PCS, MDIO_CTRL2);
	if (ret < 0)
		return ret;

	ret &= ~MDIO_PCS_CTRL2_TYPE;
	ret |= MDIO_PCS_CTRL2_10GBX;
	phy_write_mmd(phydev, MDIO_MMD_PCS, MDIO_CTRL2, ret);

	ret = phy_read_mmd(phydev, MDIO_MMD_PCS, MDIO_CTRL1);
	if (ret < 0)
		return ret;

	ret &= ~MDIO_CTRL1_SPEEDSEL;
	ret |= MDIO_CTRL1_SPEED1G;
	phy_write_mmd(phydev, MDIO_MMD_PCS, MDIO_CTRL1, ret);

	ret = amd_xgbe_phy_pcs_power_cycle(phydev);
	if (ret < 0)
		return ret;

	/* Set SerDes to 1G speed */
	spin_lock(&cmu_lock);

	amd_xgbe_phy_serdes_start_ratechange(phydev);

	XRXTX_IOWRITE_BITS(priv, RXTX_REG3, TX_DATA_RATE, RXTX_FIFTH_RATE);
	XRXTX_IOWRITE_BITS(priv, RXTX_REG3, TX_WORD_MODE, RXTX_10BIT_WORD);

	XRXTX_IOWRITE_BITS(priv, RXTX_REG5, TXAMP_CNTL,
			   priv->serdes_tx_amp[XGBE_PHY_SPEED_1000]);

	XRXTX_IOWRITE_BITS(priv, RXTX_REG6, RX_DATA_RATE, RXTX_FIFTH_RATE);
	XRXTX_IOWRITE_BITS(priv, RXTX_REG6, RX_WORD_MODE, RXTX_10BIT_WORD);

	XRXTX_IOWRITE_BITS(priv, RXTX_REG20, BLWC_ENA,
			   priv->serdes_blwc[XGBE_PHY_SPEED_1000]);

	XRXTX_IOWRITE_BITS(priv, RXTX_REG53, RX_PLLSELECT, RXTX_1G_PLL);
	XRXTX_IOWRITE_BITS(priv, RXTX_REG53, TX_PLLSELECT, RXTX_1G_PLL);
	XRXTX_IOWRITE_BITS(priv, RXTX_REG53, PI_SPD_SEL_CDR,
			   priv->serdes_cdr_rate[XGBE_PHY_SPEED_1000]);

	XRXTX_IOWRITE_BITS(priv, RXTX_REG114, PQ_REG,
			   priv->serdes_pq_skew[XGBE_PHY_SPEED_1000]);

	amd_xgbe_phy_serdes_complete_ratechange(phydev);

	spin_unlock(&cmu_lock);

	return 0;
}

static int amd_xgbe_phy_cur_mode(struct phy_device *phydev,
				 enum amd_xgbe_phy_mode *mode)
{
	int ret;

	ret = phy_read_mmd(phydev, MDIO_MMD_PCS, MDIO_CTRL2);
	if (ret < 0)
		return ret;

	if ((ret & MDIO_PCS_CTRL2_TYPE) == MDIO_PCS_CTRL2_10GBR)
		*mode = AMD_XGBE_MODE_KR;
	else
		*mode = AMD_XGBE_MODE_KX;

	return 0;
}

static bool amd_xgbe_phy_in_kr_mode(struct phy_device *phydev)
{
	enum amd_xgbe_phy_mode mode;

	if (amd_xgbe_phy_cur_mode(phydev, &mode))
		return false;

	return (mode == AMD_XGBE_MODE_KR);
}

static int amd_xgbe_phy_switch_mode(struct phy_device *phydev)
{
	struct amd_xgbe_phy_priv *priv = phydev->priv;
	int ret;

	/* If we are in KR switch to KX, and vice-versa */
	if (amd_xgbe_phy_in_kr_mode(phydev)) {
		if (priv->speed_set == AMD_XGBE_PHY_SPEEDSET_1000_10000)
			ret = amd_xgbe_phy_gmii_mode(phydev);
		else
			ret = amd_xgbe_phy_gmii_2500_mode(phydev);
	} else {
		ret = amd_xgbe_phy_xgmii_mode(phydev);
	}

	return ret;
}

static int amd_xgbe_phy_set_mode(struct phy_device *phydev,
				 enum amd_xgbe_phy_mode mode)
{
	enum amd_xgbe_phy_mode cur_mode;
	int ret;

	ret = amd_xgbe_phy_cur_mode(phydev, &cur_mode);
	if (ret)
		return ret;

	if (mode != cur_mode)
		ret = amd_xgbe_phy_switch_mode(phydev);

	return ret;
}

static int amd_xgbe_phy_set_an(struct phy_device *phydev, bool enable,
			       bool restart)
{
	int ret;

	ret = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_CTRL1);
	if (ret < 0)
		return ret;

	ret &= ~MDIO_AN_CTRL1_ENABLE;

	if (enable)
		ret |= MDIO_AN_CTRL1_ENABLE;

	if (restart)
		ret |= MDIO_AN_CTRL1_RESTART;

	phy_write_mmd(phydev, MDIO_MMD_AN, MDIO_CTRL1, ret);

	return 0;
}

static int amd_xgbe_phy_restart_an(struct phy_device *phydev)
{
	return amd_xgbe_phy_set_an(phydev, true, true);
}

static int amd_xgbe_phy_disable_an(struct phy_device *phydev)
{
	return amd_xgbe_phy_set_an(phydev, false, false);
}

static enum amd_xgbe_phy_an amd_xgbe_an_tx_training(struct phy_device *phydev,
						    enum amd_xgbe_phy_rx *state)
{
	struct amd_xgbe_phy_priv *priv = phydev->priv;
	int ad_reg, lp_reg, ret;

	*state = AMD_XGBE_RX_COMPLETE;

	/* If we're not in KR mode then we're done */
	if (!amd_xgbe_phy_in_kr_mode(phydev))
		return AMD_XGBE_AN_PAGE_RECEIVED;

	/* Enable/Disable FEC */
	ad_reg = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_AN_ADVERTISE + 2);
	if (ad_reg < 0)
		return AMD_XGBE_AN_ERROR;

	lp_reg = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_AN_LPA + 2);
	if (lp_reg < 0)
		return AMD_XGBE_AN_ERROR;

	ret = phy_read_mmd(phydev, MDIO_MMD_PMAPMD, MDIO_PMA_10GBR_FEC_CTRL);
	if (ret < 0)
		return AMD_XGBE_AN_ERROR;

	ret &= ~XGBE_PHY_FEC_MASK;
	if ((ad_reg & 0xc000) && (lp_reg & 0xc000))
		ret |= priv->fec_ability;

	phy_write_mmd(phydev, MDIO_MMD_PMAPMD, MDIO_PMA_10GBR_FEC_CTRL, ret);

	/* Start KR training */
	ret = phy_read_mmd(phydev, MDIO_MMD_PMAPMD, MDIO_PMA_10GBR_PMD_CTRL);
	if (ret < 0)
		return AMD_XGBE_AN_ERROR;

	if (ret & XGBE_PHY_KR_TRAINING_ENABLE) {
		ret |= XGBE_PHY_KR_TRAINING_START;
		phy_write_mmd(phydev, MDIO_MMD_PMAPMD, MDIO_PMA_10GBR_PMD_CTRL,
			      ret);
	}

	return AMD_XGBE_AN_PAGE_RECEIVED;
}

static enum amd_xgbe_phy_an amd_xgbe_an_tx_xnp(struct phy_device *phydev,
					       enum amd_xgbe_phy_rx *state)
{
	u16 msg;

	*state = AMD_XGBE_RX_XNP;

	msg = XNP_MCF_NULL_MESSAGE;
	msg |= XNP_MP_FORMATTED;

	phy_write_mmd(phydev, MDIO_MMD_AN, MDIO_AN_XNP + 2, 0);
	phy_write_mmd(phydev, MDIO_MMD_AN, MDIO_AN_XNP + 1, 0);
	phy_write_mmd(phydev, MDIO_MMD_AN, MDIO_AN_XNP, msg);

	return AMD_XGBE_AN_PAGE_RECEIVED;
}

static enum amd_xgbe_phy_an amd_xgbe_an_rx_bpa(struct phy_device *phydev,
					       enum amd_xgbe_phy_rx *state)
{
	unsigned int link_support;
	int ret, ad_reg, lp_reg;

	/* Read Base Ability register 2 first */
	ret = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_AN_LPA + 1);
	if (ret < 0)
		return AMD_XGBE_AN_ERROR;

	/* Check for a supported mode, otherwise restart in a different one */
	link_support = amd_xgbe_phy_in_kr_mode(phydev) ? 0x80 : 0x20;
	if (!(ret & link_support))
		return AMD_XGBE_AN_INCOMPAT_LINK;

	/* Check Extended Next Page support */
	ad_reg = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_AN_ADVERTISE);
	if (ad_reg < 0)
		return AMD_XGBE_AN_ERROR;

	lp_reg = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_AN_LPA);
	if (lp_reg < 0)
		return AMD_XGBE_AN_ERROR;

	return ((ad_reg & XNP_NP_EXCHANGE) || (lp_reg & XNP_NP_EXCHANGE)) ?
	       amd_xgbe_an_tx_xnp(phydev, state) :
	       amd_xgbe_an_tx_training(phydev, state);
}

static enum amd_xgbe_phy_an amd_xgbe_an_rx_xnp(struct phy_device *phydev,
					       enum amd_xgbe_phy_rx *state)
{
	int ad_reg, lp_reg;

	/* Check Extended Next Page support */
	ad_reg = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_AN_XNP);
	if (ad_reg < 0)
		return AMD_XGBE_AN_ERROR;

	lp_reg = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_AN_LPX);
	if (lp_reg < 0)
		return AMD_XGBE_AN_ERROR;

	return ((ad_reg & XNP_NP_EXCHANGE) || (lp_reg & XNP_NP_EXCHANGE)) ?
	       amd_xgbe_an_tx_xnp(phydev, state) :
	       amd_xgbe_an_tx_training(phydev, state);
}

static enum amd_xgbe_phy_an amd_xgbe_an_page_received(struct phy_device *phydev)
{
	struct amd_xgbe_phy_priv *priv = phydev->priv;
	enum amd_xgbe_phy_rx *state;
	int ret;

	state = amd_xgbe_phy_in_kr_mode(phydev) ? &priv->kr_state
						: &priv->kx_state;

	switch (*state) {
	case AMD_XGBE_RX_BPA:
		ret = amd_xgbe_an_rx_bpa(phydev, state);
		break;

	case AMD_XGBE_RX_XNP:
		ret = amd_xgbe_an_rx_xnp(phydev, state);
		break;

	default:
		ret = AMD_XGBE_AN_ERROR;
	}

	return ret;
}

static enum amd_xgbe_phy_an amd_xgbe_an_incompat_link(struct phy_device *phydev)
{
	struct amd_xgbe_phy_priv *priv = phydev->priv;
	int ret;

	/* Be sure we aren't looping trying to negotiate */
	if (amd_xgbe_phy_in_kr_mode(phydev)) {
		priv->kr_state = AMD_XGBE_RX_ERROR;

		if (!(phydev->supported & SUPPORTED_1000baseKX_Full) &&
		    !(phydev->supported & SUPPORTED_2500baseX_Full))
			return AMD_XGBE_AN_NO_LINK;

		if (priv->kx_state != AMD_XGBE_RX_BPA)
			return AMD_XGBE_AN_NO_LINK;
	} else {
		priv->kx_state = AMD_XGBE_RX_ERROR;

		if (!(phydev->supported & SUPPORTED_10000baseKR_Full))
			return AMD_XGBE_AN_NO_LINK;

		if (priv->kr_state != AMD_XGBE_RX_BPA)
			return AMD_XGBE_AN_NO_LINK;
	}

	ret = amd_xgbe_phy_disable_an(phydev);
	if (ret)
		return AMD_XGBE_AN_ERROR;

	ret = amd_xgbe_phy_switch_mode(phydev);
	if (ret)
		return AMD_XGBE_AN_ERROR;

	ret = amd_xgbe_phy_restart_an(phydev);
	if (ret)
		return AMD_XGBE_AN_ERROR;

	return AMD_XGBE_AN_INCOMPAT_LINK;
}

static irqreturn_t amd_xgbe_an_isr(int irq, void *data)
{
	struct amd_xgbe_phy_priv *priv = (struct amd_xgbe_phy_priv *)data;

	/* Interrupt reason must be read and cleared outside of IRQ context */
	disable_irq_nosync(priv->an_irq);

	queue_work(priv->an_workqueue, &priv->an_irq_work);

	return IRQ_HANDLED;
}

static void amd_xgbe_an_irq_work(struct work_struct *work)
{
	struct amd_xgbe_phy_priv *priv = container_of(work,
						      struct amd_xgbe_phy_priv,
						      an_irq_work);

	/* Avoid a race between enabling the IRQ and exiting the work by
	 * waiting for the work to finish and then queueing it
	 */
	flush_work(&priv->an_work);
	queue_work(priv->an_workqueue, &priv->an_work);
}

static void amd_xgbe_an_state_machine(struct work_struct *work)
{
	struct amd_xgbe_phy_priv *priv = container_of(work,
						      struct amd_xgbe_phy_priv,
						      an_work);
	struct phy_device *phydev = priv->phydev;
	enum amd_xgbe_phy_an cur_state = priv->an_state;
	int int_reg, int_mask;

	mutex_lock(&priv->an_mutex);

	/* Read the interrupt */
	int_reg = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_AN_INT);
	if (!int_reg)
		goto out;

next_int:
	if (int_reg < 0) {
		priv->an_state = AMD_XGBE_AN_ERROR;
		int_mask = XGBE_AN_INT_MASK;
	} else if (int_reg & XGBE_AN_PG_RCV) {
		priv->an_state = AMD_XGBE_AN_PAGE_RECEIVED;
		int_mask = XGBE_AN_PG_RCV;
	} else if (int_reg & XGBE_AN_INC_LINK) {
		priv->an_state = AMD_XGBE_AN_INCOMPAT_LINK;
		int_mask = XGBE_AN_INC_LINK;
	} else if (int_reg & XGBE_AN_INT_CMPLT) {
		priv->an_state = AMD_XGBE_AN_COMPLETE;
		int_mask = XGBE_AN_INT_CMPLT;
	} else {
		priv->an_state = AMD_XGBE_AN_ERROR;
		int_mask = 0;
	}

	/* Clear the interrupt to be processed */
	int_reg &= ~int_mask;
	phy_write_mmd(phydev, MDIO_MMD_AN, MDIO_AN_INT, int_reg);

	priv->an_result = priv->an_state;

again:
	cur_state = priv->an_state;

	switch (priv->an_state) {
	case AMD_XGBE_AN_READY:
		priv->an_supported = 0;
		break;

	case AMD_XGBE_AN_PAGE_RECEIVED:
		priv->an_state = amd_xgbe_an_page_received(phydev);
		priv->an_supported++;
		break;

	case AMD_XGBE_AN_INCOMPAT_LINK:
		priv->an_supported = 0;
		priv->parallel_detect = 0;
		priv->an_state = amd_xgbe_an_incompat_link(phydev);
		break;

	case AMD_XGBE_AN_COMPLETE:
		priv->parallel_detect = priv->an_supported ? 0 : 1;
		netdev_dbg(phydev->attached_dev, "%s successful\n",
			   priv->an_supported ? "Auto negotiation"
					      : "Parallel detection");
		break;

	case AMD_XGBE_AN_NO_LINK:
		break;

	default:
		priv->an_state = AMD_XGBE_AN_ERROR;
	}

	if (priv->an_state == AMD_XGBE_AN_NO_LINK) {
		/* Disable auto-negotiation for now - it will be
		 * re-enabled once a link is established
		 */
		amd_xgbe_phy_disable_an(phydev);

		int_reg = 0;
		phy_write_mmd(phydev, MDIO_MMD_AN, MDIO_AN_INT, 0);
	} else if (priv->an_state == AMD_XGBE_AN_ERROR) {
		netdev_err(phydev->attached_dev,
			   "error during auto-negotiation, state=%u\n",
			   cur_state);

		int_reg = 0;
		phy_write_mmd(phydev, MDIO_MMD_AN, MDIO_AN_INT, 0);
	}

	if (priv->an_state >= AMD_XGBE_AN_COMPLETE) {
		priv->an_result = priv->an_state;
		priv->an_state = AMD_XGBE_AN_READY;
		priv->kr_state = AMD_XGBE_RX_BPA;
		priv->kx_state = AMD_XGBE_RX_BPA;
	}

	if (cur_state != priv->an_state)
		goto again;

	if (int_reg)
		goto next_int;

out:
	enable_irq(priv->an_irq);

	mutex_unlock(&priv->an_mutex);
}

static int amd_xgbe_an_init(struct phy_device *phydev)
{
	int ret;

	/* Set up Advertisement register 3 first */
	ret = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_AN_ADVERTISE + 2);
	if (ret < 0)
		return ret;

	if (phydev->supported & SUPPORTED_10000baseR_FEC)
		ret |= 0xc000;
	else
		ret &= ~0xc000;

	phy_write_mmd(phydev, MDIO_MMD_AN, MDIO_AN_ADVERTISE + 2, ret);

	/* Set up Advertisement register 2 next */
	ret = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_AN_ADVERTISE + 1);
	if (ret < 0)
		return ret;

	if (phydev->supported & SUPPORTED_10000baseKR_Full)
		ret |= 0x80;
	else
		ret &= ~0x80;

	if ((phydev->supported & SUPPORTED_1000baseKX_Full) ||
	    (phydev->supported & SUPPORTED_2500baseX_Full))
		ret |= 0x20;
	else
		ret &= ~0x20;

	phy_write_mmd(phydev, MDIO_MMD_AN, MDIO_AN_ADVERTISE + 1, ret);

	/* Set up Advertisement register 1 last */
	ret = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_AN_ADVERTISE);
	if (ret < 0)
		return ret;

	if (phydev->supported & SUPPORTED_Pause)
		ret |= 0x400;
	else
		ret &= ~0x400;

	if (phydev->supported & SUPPORTED_Asym_Pause)
		ret |= 0x800;
	else
		ret &= ~0x800;

	/* We don't intend to perform XNP */
	ret &= ~XNP_NP_EXCHANGE;

	phy_write_mmd(phydev, MDIO_MMD_AN, MDIO_AN_ADVERTISE, ret);

	return 0;
}

static int amd_xgbe_phy_soft_reset(struct phy_device *phydev)
{
	int count, ret;

	ret = phy_read_mmd(phydev, MDIO_MMD_PCS, MDIO_CTRL1);
	if (ret < 0)
		return ret;

	ret |= MDIO_CTRL1_RESET;
	phy_write_mmd(phydev, MDIO_MMD_PCS, MDIO_CTRL1, ret);

	count = 50;
	do {
		msleep(20);
		ret = phy_read_mmd(phydev, MDIO_MMD_PCS, MDIO_CTRL1);
		if (ret < 0)
			return ret;
	} while ((ret & MDIO_CTRL1_RESET) && --count);

	if (ret & MDIO_CTRL1_RESET)
		return -ETIMEDOUT;

	/* Disable auto-negotiation for now */
	ret = amd_xgbe_phy_disable_an(phydev);
	if (ret < 0)
		return ret;

	/* Clear auto-negotiation interrupts */
	phy_write_mmd(phydev, MDIO_MMD_AN, MDIO_AN_INT, 0);

	return 0;
}

static int amd_xgbe_phy_config_init(struct phy_device *phydev)
{
	struct amd_xgbe_phy_priv *priv = phydev->priv;
	struct net_device *netdev = phydev->attached_dev;
	int ret;

	if (!priv->an_irq_allocated) {
		/* Allocate the auto-negotiation workqueue and interrupt */
		snprintf(priv->an_irq_name, sizeof(priv->an_irq_name) - 1,
			 "%s-pcs", netdev_name(netdev));

		priv->an_workqueue =
			create_singlethread_workqueue(priv->an_irq_name);
		if (!priv->an_workqueue) {
			netdev_err(netdev, "phy workqueue creation failed\n");
			return -ENOMEM;
		}

		ret = devm_request_irq(priv->dev, priv->an_irq,
				       amd_xgbe_an_isr, 0, priv->an_irq_name,
				       priv);
		if (ret) {
			netdev_err(netdev, "phy irq request failed\n");
			destroy_workqueue(priv->an_workqueue);
			return ret;
		}

		priv->an_irq_allocated = 1;
	}

	ret = phy_read_mmd(phydev, MDIO_MMD_PMAPMD, MDIO_PMA_10GBR_FEC_ABILITY);
	if (ret < 0)
		return ret;
	priv->fec_ability = ret & XGBE_PHY_FEC_MASK;

	/* Initialize supported features */
	phydev->supported = SUPPORTED_Autoneg;
	phydev->supported |= SUPPORTED_Pause | SUPPORTED_Asym_Pause;
	phydev->supported |= SUPPORTED_Backplane;
	phydev->supported |= SUPPORTED_10000baseKR_Full;
	switch (priv->speed_set) {
	case AMD_XGBE_PHY_SPEEDSET_1000_10000:
		phydev->supported |= SUPPORTED_1000baseKX_Full;
		break;
	case AMD_XGBE_PHY_SPEEDSET_2500_10000:
		phydev->supported |= SUPPORTED_2500baseX_Full;
		break;
	}

	if (priv->fec_ability & XGBE_PHY_FEC_ENABLE)
		phydev->supported |= SUPPORTED_10000baseR_FEC;

	phydev->advertising = phydev->supported;

	/* Set initial mode - call the mode setting routines
	 * directly to insure we are properly configured
	 */
	if (phydev->supported & SUPPORTED_10000baseKR_Full)
		ret = amd_xgbe_phy_xgmii_mode(phydev);
	else if (phydev->supported & SUPPORTED_1000baseKX_Full)
		ret = amd_xgbe_phy_gmii_mode(phydev);
	else if (phydev->supported & SUPPORTED_2500baseX_Full)
		ret = amd_xgbe_phy_gmii_2500_mode(phydev);
	else
		ret = -EINVAL;
	if (ret < 0)
		return ret;

	/* Set up advertisement registers based on current settings */
	ret = amd_xgbe_an_init(phydev);
	if (ret)
		return ret;

	/* Enable auto-negotiation interrupts */
	phy_write_mmd(phydev, MDIO_MMD_AN, MDIO_AN_INTMASK, 0x07);

	return 0;
}

static int amd_xgbe_phy_setup_forced(struct phy_device *phydev)
{
	int ret;

	/* Disable auto-negotiation */
	ret = amd_xgbe_phy_disable_an(phydev);
	if (ret < 0)
		return ret;

	/* Validate/Set specified speed */
	switch (phydev->speed) {
	case SPEED_10000:
		ret = amd_xgbe_phy_set_mode(phydev, AMD_XGBE_MODE_KR);
		break;

	case SPEED_2500:
	case SPEED_1000:
		ret = amd_xgbe_phy_set_mode(phydev, AMD_XGBE_MODE_KX);
		break;

	default:
		ret = -EINVAL;
	}

	if (ret < 0)
		return ret;

	/* Validate duplex mode */
	if (phydev->duplex != DUPLEX_FULL)
		return -EINVAL;

	phydev->pause = 0;
	phydev->asym_pause = 0;

	return 0;
}

static int __amd_xgbe_phy_config_aneg(struct phy_device *phydev)
{
	struct amd_xgbe_phy_priv *priv = phydev->priv;
	u32 mmd_mask = phydev->c45_ids.devices_in_package;
	int ret;

	if (phydev->autoneg != AUTONEG_ENABLE)
		return amd_xgbe_phy_setup_forced(phydev);

	/* Make sure we have the AN MMD present */
	if (!(mmd_mask & MDIO_DEVS_AN))
		return -EINVAL;

	/* Disable auto-negotiation interrupt */
	disable_irq(priv->an_irq);

	/* Start auto-negotiation in a supported mode */
	if (phydev->supported & SUPPORTED_10000baseKR_Full)
		ret = amd_xgbe_phy_set_mode(phydev, AMD_XGBE_MODE_KR);
	else if ((phydev->supported & SUPPORTED_1000baseKX_Full) ||
		 (phydev->supported & SUPPORTED_2500baseX_Full))
		ret = amd_xgbe_phy_set_mode(phydev, AMD_XGBE_MODE_KX);
	else
		ret = -EINVAL;
	if (ret < 0) {
		enable_irq(priv->an_irq);
		return ret;
	}

	/* Disable and stop any in progress auto-negotiation */
	ret = amd_xgbe_phy_disable_an(phydev);
	if (ret < 0)
		return ret;

	/* Clear any auto-negotitation interrupts */
	phy_write_mmd(phydev, MDIO_MMD_AN, MDIO_AN_INT, 0);

	priv->an_result = AMD_XGBE_AN_READY;
	priv->an_state = AMD_XGBE_AN_READY;
	priv->kr_state = AMD_XGBE_RX_BPA;
	priv->kx_state = AMD_XGBE_RX_BPA;

	/* Re-enable auto-negotiation interrupt */
	enable_irq(priv->an_irq);

	/* Set up advertisement registers based on current settings */
	ret = amd_xgbe_an_init(phydev);
	if (ret)
		return ret;

	/* Enable and start auto-negotiation */
	ret = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_AN_KR_CTRL);
	if (ret < 0)
		return ret;

	ret |= MDIO_KR_CTRL_PDETECT;
	phy_write_mmd(phydev, MDIO_MMD_AN, MDIO_AN_KR_CTRL, ret);

	return amd_xgbe_phy_restart_an(phydev);
}

static int amd_xgbe_phy_config_aneg(struct phy_device *phydev)
{
	struct amd_xgbe_phy_priv *priv = phydev->priv;
	int ret;

	mutex_lock(&priv->an_mutex);

	ret = __amd_xgbe_phy_config_aneg(phydev);

	mutex_unlock(&priv->an_mutex);

	return ret;
}

static int amd_xgbe_phy_aneg_done(struct phy_device *phydev)
{
	struct amd_xgbe_phy_priv *priv = phydev->priv;

	return (priv->an_result == AMD_XGBE_AN_COMPLETE);
}

static int amd_xgbe_phy_update_link(struct phy_device *phydev)
{
	struct amd_xgbe_phy_priv *priv = phydev->priv;
	unsigned int check_again, autoneg;
	int ret;

	/* If we're doing auto-negotiation don't report link down */
	if (priv->an_state != AMD_XGBE_AN_READY) {
		phydev->link = 1;
		return 0;
	}

	/* Since the device can be in the wrong mode when a link is
	 * (re-)established (cable connected after the interface is
	 * up, etc.), the link status may report no link. If there
	 * is no link, try switching modes and checking the status
	 * again if auto negotiation is enabled.
	 */
	check_again = (phydev->autoneg == AUTONEG_ENABLE) ? 1 : 0;
again:
	/* Link status is latched low, so read once to clear
	 * and then read again to get current state
	 */
	ret = phy_read_mmd(phydev, MDIO_MMD_PCS, MDIO_STAT1);
	if (ret < 0)
		return ret;

	ret = phy_read_mmd(phydev, MDIO_MMD_PCS, MDIO_STAT1);
	if (ret < 0)
		return ret;

	phydev->link = (ret & MDIO_STAT1_LSTATUS) ? 1 : 0;

	if (!phydev->link) {
		if (check_again) {
			ret = amd_xgbe_phy_switch_mode(phydev);
			if (ret < 0)
				return ret;
			check_again = 0;
			goto again;
		}
	}

	autoneg = (phydev->link && !priv->link) ? 1 : 0;
	priv->link = phydev->link;
	if (autoneg) {
		/* Link is (back) up, re-start auto-negotiation */
		ret = amd_xgbe_phy_config_aneg(phydev);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int amd_xgbe_phy_read_status(struct phy_device *phydev)
{
	struct amd_xgbe_phy_priv *priv = phydev->priv;
	u32 mmd_mask = phydev->c45_ids.devices_in_package;
	int ret, ad_ret, lp_ret;

	ret = amd_xgbe_phy_update_link(phydev);
	if (ret)
		return ret;

	if ((phydev->autoneg == AUTONEG_ENABLE) &&
	    !priv->parallel_detect) {
		if (!(mmd_mask & MDIO_DEVS_AN))
			return -EINVAL;

		if (!amd_xgbe_phy_aneg_done(phydev))
			return 0;

		/* Compare Advertisement and Link Partner register 1 */
		ad_ret = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_AN_ADVERTISE);
		if (ad_ret < 0)
			return ad_ret;
		lp_ret = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_AN_LPA);
		if (lp_ret < 0)
			return lp_ret;

		ad_ret &= lp_ret;
		phydev->pause = (ad_ret & 0x400) ? 1 : 0;
		phydev->asym_pause = (ad_ret & 0x800) ? 1 : 0;

		/* Compare Advertisement and Link Partner register 2 */
		ad_ret = phy_read_mmd(phydev, MDIO_MMD_AN,
				      MDIO_AN_ADVERTISE + 1);
		if (ad_ret < 0)
			return ad_ret;
		lp_ret = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_AN_LPA + 1);
		if (lp_ret < 0)
			return lp_ret;

		ad_ret &= lp_ret;
		if (ad_ret & 0x80) {
			phydev->speed = SPEED_10000;
			ret = amd_xgbe_phy_set_mode(phydev, AMD_XGBE_MODE_KR);
			if (ret)
				return ret;
		} else {
			switch (priv->speed_set) {
			case AMD_XGBE_PHY_SPEEDSET_1000_10000:
				phydev->speed = SPEED_1000;
				break;

			case AMD_XGBE_PHY_SPEEDSET_2500_10000:
				phydev->speed = SPEED_2500;
				break;
			}

			ret = amd_xgbe_phy_set_mode(phydev, AMD_XGBE_MODE_KX);
			if (ret)
				return ret;
		}

		phydev->duplex = DUPLEX_FULL;
	} else {
		if (amd_xgbe_phy_in_kr_mode(phydev)) {
			phydev->speed = SPEED_10000;
		} else {
			switch (priv->speed_set) {
			case AMD_XGBE_PHY_SPEEDSET_1000_10000:
				phydev->speed = SPEED_1000;
				break;

			case AMD_XGBE_PHY_SPEEDSET_2500_10000:
				phydev->speed = SPEED_2500;
				break;
			}
		}
		phydev->duplex = DUPLEX_FULL;
		phydev->pause = 0;
		phydev->asym_pause = 0;
	}

	return 0;
}

static int amd_xgbe_phy_suspend(struct phy_device *phydev)
{
	struct amd_xgbe_phy_priv *priv = phydev->priv;
	int ret;

	mutex_lock(&phydev->lock);

	ret = phy_read_mmd(phydev, MDIO_MMD_PCS, MDIO_CTRL1);
	if (ret < 0)
		goto unlock;

	priv->lpm_ctrl = ret;

	ret |= MDIO_CTRL1_LPOWER;
	phy_write_mmd(phydev, MDIO_MMD_PCS, MDIO_CTRL1, ret);

	ret = 0;

unlock:
	mutex_unlock(&phydev->lock);

	return ret;
}

static int amd_xgbe_phy_resume(struct phy_device *phydev)
{
	struct amd_xgbe_phy_priv *priv = phydev->priv;

	mutex_lock(&phydev->lock);

	priv->lpm_ctrl &= ~MDIO_CTRL1_LPOWER;
	phy_write_mmd(phydev, MDIO_MMD_PCS, MDIO_CTRL1, priv->lpm_ctrl);

	mutex_unlock(&phydev->lock);

	return 0;
}

static unsigned int amd_xgbe_phy_resource_count(struct platform_device *pdev,
						unsigned int type)
{
	unsigned int count;
	int i;

	for (i = 0, count = 0; i < pdev->num_resources; i++) {
		struct resource *r = &pdev->resource[i];

		if (type == resource_type(r))
			count++;
	}

	return count;
}

static int amd_xgbe_phy_probe(struct phy_device *phydev)
{
	struct amd_xgbe_phy_priv *priv;
	struct platform_device *phy_pdev;
	struct device *dev, *phy_dev;
	unsigned int phy_resnum, phy_irqnum;
	int ret;

	if (!phydev->bus || !phydev->bus->parent)
		return -EINVAL;

	dev = phydev->bus->parent;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->pdev = to_platform_device(dev);
	priv->adev = ACPI_COMPANION(dev);
	priv->dev = dev;
	priv->phydev = phydev;
	mutex_init(&priv->an_mutex);
	INIT_WORK(&priv->an_irq_work, amd_xgbe_an_irq_work);
	INIT_WORK(&priv->an_work, amd_xgbe_an_state_machine);

	if (!priv->adev || acpi_disabled) {
		struct device_node *bus_node;
		struct device_node *phy_node;

		bus_node = priv->dev->of_node;
		phy_node = of_parse_phandle(bus_node, "phy-handle", 0);
		if (!phy_node) {
			dev_err(dev, "unable to parse phy-handle\n");
			ret = -EINVAL;
			goto err_priv;
		}

		phy_pdev = of_find_device_by_node(phy_node);
		of_node_put(phy_node);

		if (!phy_pdev) {
			dev_err(dev, "unable to obtain phy device\n");
			ret = -EINVAL;
			goto err_priv;
		}

		phy_resnum = 0;
		phy_irqnum = 0;
	} else {
		/* In ACPI, the XGBE and PHY resources are the grouped
		 * together with the PHY resources at the end
		 */
		phy_pdev = priv->pdev;
		phy_resnum = amd_xgbe_phy_resource_count(phy_pdev,
							 IORESOURCE_MEM) - 2;
		phy_irqnum = amd_xgbe_phy_resource_count(phy_pdev,
							 IORESOURCE_IRQ) - 1;
	}
	phy_dev = &phy_pdev->dev;

	/* Get the device mmio areas */
	priv->rxtx_res = platform_get_resource(phy_pdev, IORESOURCE_MEM,
					       phy_resnum++);
	priv->rxtx_regs = devm_ioremap_resource(dev, priv->rxtx_res);
	if (IS_ERR(priv->rxtx_regs)) {
		dev_err(dev, "rxtx ioremap failed\n");
		ret = PTR_ERR(priv->rxtx_regs);
		goto err_put;
	}

	/* All xgbe phy devices share the CMU registers so retrieve
	 * the resource and do the ioremap directly rather than
	 * the devm_ioremap_resource call
	 */
	priv->cmu_res = platform_get_resource(phy_pdev, IORESOURCE_MEM,
					      phy_resnum++);
	if (!priv->cmu_res) {
		dev_err(dev, "cmu invalid resource\n");
		ret = -EINVAL;
		goto err_rxtx;
	}
	priv->cmu_regs = devm_ioremap_nocache(dev, priv->cmu_res->start,
					      resource_size(priv->cmu_res));
	if (!priv->cmu_regs) {
		dev_err(dev, "cmu ioremap failed\n");
		ret = -ENOMEM;
		goto err_rxtx;
	}

	/* Get the auto-negotiation interrupt */
	ret = platform_get_irq(phy_pdev, phy_irqnum);
	if (ret < 0) {
		dev_err(dev, "platform_get_irq failed\n");
		goto err_cmu;
	}
	if (!phy_irqnum) {
		struct irq_data *d = irq_get_irq_data(ret);
		if (!d) {
			dev_err(dev, "unable to set AN interrupt\n");
			ret = -EINVAL;
			goto err_put;
		}

#ifdef CONFIG_ACPI
		ret = acpi_register_gsi(dev, d->hwirq - 2,
					ACPI_LEVEL_SENSITIVE, ACPI_ACTIVE_HIGH);
#else
		ret = -EINVAL;
#endif
		if (ret < 0) {
			dev_err(dev, "unable to set AN interrupt\n");
			ret = -EINVAL;
			goto err_put;
		}
	}
	priv->an_irq = ret;

	/* Get the device serdes channel property */
	ret = device_property_read_u32(phy_dev, XGBE_PHY_CHANNEL_PROPERTY,
				       &priv->serdes_channel);
	if (ret) {
		dev_err(dev, "invalid %s property\n",
			XGBE_PHY_CHANNEL_PROPERTY);
		goto err_cmu;
	}

	/* Get the device speed set property */
	ret = device_property_read_u32(phy_dev, XGBE_PHY_SPEEDSET_PROPERTY,
				       &priv->speed_set);
	if (ret) {
		dev_err(dev, "invalid %s property\n",
			XGBE_PHY_SPEEDSET_PROPERTY);
		goto err_cmu;
	}

	switch (priv->speed_set) {
	case AMD_XGBE_PHY_SPEEDSET_1000_10000:
	case AMD_XGBE_PHY_SPEEDSET_2500_10000:
		break;
	default:
		dev_err(dev, "invalid %s property\n",
			XGBE_PHY_SPEEDSET_PROPERTY);
		ret = -EINVAL;
		goto err_cmu;
	}

	if (device_property_present(phy_dev, XGBE_PHY_BLWC_PROPERTY)) {
		ret = device_property_read_u32_array(phy_dev,
						     XGBE_PHY_BLWC_PROPERTY,
						     priv->serdes_blwc,
						     XGBE_PHY_SPEEDS);
		if (ret) {
			dev_err(dev, "invalid %s property\n",
				XGBE_PHY_BLWC_PROPERTY);
			goto err_cmu;
		}
	} else {
		memcpy(priv->serdes_blwc, amd_xgbe_phy_serdes_blwc,
		       sizeof(priv->serdes_blwc));
	}

	if (device_property_present(phy_dev, XGBE_PHY_CDR_RATE_PROPERTY)) {
		ret = device_property_read_u32_array(phy_dev,
						     XGBE_PHY_CDR_RATE_PROPERTY,
						     priv->serdes_cdr_rate,
						     XGBE_PHY_SPEEDS);
		if (ret) {
			dev_err(dev, "invalid %s property\n",
				XGBE_PHY_CDR_RATE_PROPERTY);
			goto err_cmu;
		}
	} else {
		memcpy(priv->serdes_cdr_rate, amd_xgbe_phy_serdes_cdr_rate,
		       sizeof(priv->serdes_cdr_rate));
	}

	if (device_property_present(phy_dev, XGBE_PHY_PQ_SKEW_PROPERTY)) {
		ret = device_property_read_u32_array(phy_dev,
						     XGBE_PHY_PQ_SKEW_PROPERTY,
						     priv->serdes_pq_skew,
						     XGBE_PHY_SPEEDS);
		if (ret) {
			dev_err(dev, "invalid %s property\n",
				XGBE_PHY_PQ_SKEW_PROPERTY);
			goto err_cmu;
		}
	} else {
		memcpy(priv->serdes_pq_skew, amd_xgbe_phy_serdes_pq_skew,
		       sizeof(priv->serdes_pq_skew));
	}

	if (device_property_present(phy_dev, XGBE_PHY_TX_AMP_PROPERTY)) {
		ret = device_property_read_u32_array(phy_dev,
						     XGBE_PHY_TX_AMP_PROPERTY,
						     priv->serdes_tx_amp,
						     XGBE_PHY_SPEEDS);
		if (ret) {
			dev_err(dev, "invalid %s property\n",
				XGBE_PHY_TX_AMP_PROPERTY);
			goto err_cmu;
		}
	} else {
		memcpy(priv->serdes_tx_amp, amd_xgbe_phy_serdes_tx_amp,
		       sizeof(priv->serdes_tx_amp));
	}

	priv->link = 1;

	phydev->priv = priv;

	if (!priv->adev || acpi_disabled)
		platform_device_put(phy_pdev);

	return 0;

err_cmu:
	devm_iounmap(dev, priv->cmu_regs);

err_rxtx:
	devm_iounmap(dev, priv->rxtx_regs);
	devm_release_mem_region(dev, priv->rxtx_res->start,
				resource_size(priv->rxtx_res));

err_put:
	if (!priv->adev || acpi_disabled)
		platform_device_put(phy_pdev);

err_priv:
	devm_kfree(dev, priv);

	return ret;
}

static void amd_xgbe_phy_remove(struct phy_device *phydev)
{
	struct amd_xgbe_phy_priv *priv = phydev->priv;
	struct device *dev = priv->dev;

	if (priv->an_irq_allocated) {
		devm_free_irq(dev, priv->an_irq, priv);

		flush_workqueue(priv->an_workqueue);
		destroy_workqueue(priv->an_workqueue);
	}

	devm_iounmap(dev, priv->cmu_regs);

	devm_iounmap(dev, priv->rxtx_regs);
	devm_release_mem_region(dev, priv->rxtx_res->start,
				resource_size(priv->rxtx_res));

	devm_kfree(dev, priv);
}

static int amd_xgbe_match_phy_device(struct phy_device *phydev)
{
	return phydev->c45_ids.device_ids[MDIO_MMD_PCS] == XGBE_PHY_ID;
}

static struct phy_driver amd_xgbe_phy_a0_driver[] = {
	{
		.phy_id			= XGBE_PHY_ID,
		.phy_id_mask		= XGBE_PHY_MASK,
		.name			= "AMD XGBE PHY A0",
		.features		= 0,
		.probe			= amd_xgbe_phy_probe,
		.remove			= amd_xgbe_phy_remove,
		.soft_reset		= amd_xgbe_phy_soft_reset,
		.config_init		= amd_xgbe_phy_config_init,
		.suspend		= amd_xgbe_phy_suspend,
		.resume			= amd_xgbe_phy_resume,
		.config_aneg		= amd_xgbe_phy_config_aneg,
		.aneg_done		= amd_xgbe_phy_aneg_done,
		.read_status		= amd_xgbe_phy_read_status,
		.match_phy_device	= amd_xgbe_match_phy_device,
		.driver			= {
			.owner = THIS_MODULE,
		},
	},
};

module_phy_driver(amd_xgbe_phy_a0_driver);

static struct mdio_device_id __maybe_unused amd_xgbe_phy_ids_a0[] = {
	{ XGBE_PHY_ID, XGBE_PHY_MASK },
	{ }
};
MODULE_DEVICE_TABLE(mdio, amd_xgbe_phy_ids_a0);
