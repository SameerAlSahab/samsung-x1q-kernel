/*
 * Copyright (c) 2013-2020 The Linux Foundation. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/if_arp.h>
#ifdef CONFIG_PCI_MSM
#include <linux/msm_pcie.h>
#endif
#include "hif_io32.h"
#include "if_pci.h"
#include "hif.h"
#include "target_type.h"
#include "hif_main.h"
#include "ce_main.h"
#include "ce_api.h"
#include "ce_internal.h"
#include "ce_reg.h"
#include "ce_bmi.h"
#include "regtable.h"
#include "hif_hw_version.h"
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include "qdf_status.h"
#include "qdf_atomic.h"
#include "pld_common.h"
#include "mp_dev.h"
#include "hif_debug.h"

#include "if_pci_internal.h"
#include "ce_tasklet.h"
#include "targaddrs.h"
#include "hif_exec.h"

#include "pci_api.h"
#include "ahb_api.h"
#include "qdf_hang_event_notifier.h"
#include "qdf_platform.h"

/* Maximum ms timeout for host to wake up target */
#define PCIE_WAKE_TIMEOUT 1000
#define RAMDUMP_EVENT_TIMEOUT 2500

/* Setting SOC_GLOBAL_RESET during driver unload causes intermittent
 * PCIe data bus error
 * As workaround for this issue - changing the reset sequence to
 * use TargetCPU warm reset * instead of SOC_GLOBAL_RESET
 */
#define CPU_WARM_RESET_WAR

/*
 * Top-level interrupt handler for all PCI interrupts from a Target.
 * When a block of MSI interrupts is allocated, this top-level handler
 * is not used; instead, we directly call the correct sub-handler.
 */
struct ce_irq_reg_table {
	uint32_t irq_enable;
	uint32_t irq_status;
};

#ifndef QCA_WIFI_3_0_ADRASTEA
static inline void hif_pci_route_adrastea_interrupt(struct hif_pci_softc *sc)
{
}
#else
static void hif_pci_route_adrastea_interrupt(struct hif_pci_softc *sc)
{
	struct hif_softc *scn = HIF_GET_SOFTC(sc);
	unsigned int target_enable0, target_enable1;
	unsigned int target_cause0, target_cause1;

	target_enable0 = hif_read32_mb(sc, sc->mem + Q6_ENABLE_REGISTER_0);
	target_enable1 = hif_read32_mb(sc, sc->mem + Q6_ENABLE_REGISTER_1);
	target_cause0 = hif_read32_mb(sc, sc->mem + Q6_CAUSE_REGISTER_0);
	target_cause1 = hif_read32_mb(sc, sc->mem + Q6_CAUSE_REGISTER_1);

	if ((target_enable0 & target_cause0) ||
	    (target_enable1 & target_cause1)) {
		hif_write32_mb(sc, sc->mem + Q6_ENABLE_REGISTER_0, 0);
		hif_write32_mb(sc, sc->mem + Q6_ENABLE_REGISTER_1, 0);

		if (scn->notice_send)
			pld_intr_notify_q6(sc->dev);
	}
}
#endif


/**
 * pci_dispatch_ce_irq() - pci_dispatch_ce_irq
 * @scn: scn
 *
 * Return: N/A
 */
static void pci_dispatch_interrupt(struct hif_softc *scn)
{
	uint32_t intr_summary;
	int id;
	struct HIF_CE_state *hif_state = HIF_GET_CE_STATE(scn);

	if (scn->hif_init_done != true)
		return;

	if (Q_TARGET_ACCESS_BEGIN(scn) < 0)
		return;

	intr_summary = CE_INTERRUPT_SUMMARY(scn);

	if (intr_summary == 0) {
		if ((scn->target_status != TARGET_STATUS_RESET) &&
			(!qdf_atomic_read(&scn->link_suspended))) {

			hif_write32_mb(scn, scn->mem +
				(SOC_CORE_BASE_ADDRESS |
				PCIE_INTR_ENABLE_ADDRESS),
				HOST_GROUP0_MASK);

			hif_read32_mb(scn, scn->mem +
					(SOC_CORE_BASE_ADDRESS |
					PCIE_INTR_ENABLE_ADDRESS));
		}
		Q_TARGET_ACCESS_END(scn);
		return;
	}
	Q_TARGET_ACCESS_END(scn);

	scn->ce_irq_summary = intr_summary;
	for (id = 0; intr_summary && (id < scn->ce_count); id++) {
		if (intr_summary & (1 << id)) {
			intr_summary &= ~(1 << id);
			ce_dispatch_interrupt(id,  &hif_state->tasklets[id]);
		}
	}
}

irqreturn_t hif_pci_legacy_ce_interrupt_handler(int irq, void *arg)
{
	struct hif_pci_softc *sc = (struct hif_pci_softc *)arg;
	struct hif_softc *scn = HIF_GET_SOFTC(sc);
	struct HIF_CE_state *hif_state = HIF_GET_CE_STATE(arg);

	volatile int tmp;
	uint16_t val = 0;
	uint32_t bar0 = 0;
	uint32_t fw_indicator_address, fw_indicator;
	bool ssr_irq = false;
	unsigned int host_cause, host_enable;

	if (LEGACY_INTERRUPTS(sc)) {
		if (Q_TARGET_ACCESS_BEGIN(scn) < 0)
			return IRQ_HANDLED;

		if (ADRASTEA_BU) {
			host_enable = hif_read32_mb(sc, sc->mem +
						    PCIE_INTR_ENABLE_ADDRESS);
			host_cause = hif_read32_mb(sc, sc->mem +
						   PCIE_INTR_CAUSE_ADDRESS);
			if (!(host_enable & host_cause)) {
				hif_pci_route_adrastea_interrupt(sc);
				return IRQ_HANDLED;
			}
		}

		/* Clear Legacy PCI line interrupts
		 * IMPORTANT: INTR_CLR regiser has to be set
		 * after INTR_ENABLE is set to 0,
		 * otherwise interrupt can not be really cleared
		 */
		hif_write32_mb(sc, sc->mem +
			      (SOC_CORE_BASE_ADDRESS |
			       PCIE_INTR_ENABLE_ADDRESS), 0);

		hif_write32_mb(sc, sc->mem +
			      (SOC_CORE_BASE_ADDRESS | PCIE_INTR_CLR_ADDRESS),
			       ADRASTEA_BU ?
			       (host_enable & host_cause) :
			      HOST_GROUP0_MASK);

		if (ADRASTEA_BU)
			hif_write32_mb(sc, sc->mem + 0x2f100c,
				       (host_cause >> 1));

		/* IMPORTANT: this extra read transaction is required to
		 * flush the posted write buffer
		 */
		if (!ADRASTEA_BU) {
		tmp =
			hif_read32_mb(sc, sc->mem +
				     (SOC_CORE_BASE_ADDRESS |
				      PCIE_INTR_ENABLE_ADDRESS));

		if (tmp == 0xdeadbeef) {
			HIF_ERROR("BUG(%s): SoC returns 0xdeadbeef!!",
			       __func__);

			pci_read_config_word(sc->pdev, PCI_VENDOR_ID, &val);
			HIF_ERROR("%s: PCI Vendor ID = 0x%04x",
			       __func__, val);

			pci_read_config_word(sc->pdev, PCI_DEVICE_ID, &val);
			HIF_ERROR("%s: PCI Device ID = 0x%04x",
			       __func__, val);

			pci_read_config_word(sc->pdev, PCI_COMMAND, &val);
			HIF_ERROR("%s: PCI Command = 0x%04x", __func__,
			       val);

			pci_read_config_word(sc->pdev, PCI_STATUS, &val);
			HIF_ERROR("%s: PCI Status = 0x%04x", __func__,
			       val);

			pci_read_config_dword(sc->pdev, PCI_BASE_ADDRESS_0,
					      &bar0);
			HIF_ERROR("%s: PCI BAR0 = 0x%08x", __func__,
			       bar0);

			HIF_ERROR("%s: RTC_STATE_ADDRESS = 0x%08x",
				  __func__,
				  hif_read32_mb(sc, sc->mem +
						PCIE_LOCAL_BASE_ADDRESS
						+ RTC_STATE_ADDRESS));
			HIF_ERROR("%s: PCIE_SOC_WAKE_ADDRESS = 0x%08x",
				  __func__,
				  hif_read32_mb(sc, sc->mem +
						PCIE_LOCAL_BASE_ADDRESS
						+ PCIE_SOC_WAKE_ADDRESS));
			HIF_ERROR("%s: 0x80008 = 0x%08x, 0x8000c = 0x%08x",
				  __func__,
				  hif_read32_mb(sc, sc->mem + 0x80008),
				  hif_read32_mb(sc, sc->mem + 0x8000c));
			HIF_ERROR("%s: 0x80010 = 0x%08x, 0x80014 = 0x%08x",
				  __func__,
				  hif_read32_mb(sc, sc->mem + 0x80010),
				  hif_read32_mb(sc, sc->mem + 0x80014));
			HIF_ERROR("%s: 0x80018 = 0x%08x, 0x8001c = 0x%08x",
				  __func__,
				  hif_read32_mb(sc, sc->mem + 0x80018),
				  hif_read32_mb(sc, sc->mem + 0x8001c));
			QDF_BUG(0);
		}

		PCI_CLR_CAUSE0_REGISTER(sc);
		}

		if (HAS_FW_INDICATOR) {
			fw_indicator_address = hif_state->fw_indicator_address;
			fw_indicator = A_TARGET_READ(scn, fw_indicator_address);
			if ((fw_indicator != ~0) &&
			   (fw_indicator & FW_IND_EVENT_PENDING))
				ssr_irq = true;
		}

		if (Q_TARGET_ACCESS_END(scn) < 0)
			return IRQ_HANDLED;
	}
	/* TBDXXX: Add support for WMAC */

	if (ssr_irq) {
		sc->irq_event = irq;
		qdf_atomic_set(&scn->tasklet_from_intr, 1);

		qdf_atomic_inc(&scn->active_tasklet_cnt);
		tasklet_schedule(&sc->intr_tq);
	} else {
		pci_dispatch_interrupt(scn);
	}

	return IRQ_HANDLED;
}

bool hif_pci_targ_is_present(struct hif_softc *scn, void *__iomem *mem)
{
	return 1;               /* FIX THIS */
}

int hif_get_irq_num(struct hif_opaque_softc *scn, int *irq, uint32_t size)
{
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(scn);
	int i = 0;

	if (!irq || !size) {
		return -EINVAL;
	}

	if (!sc->num_msi_intrs || sc->num_msi_intrs == 1) {
		irq[0] = sc->irq;
		return 1;
	}

	if (sc->num_msi_intrs > size) {
		qdf_print("Not enough space in irq buffer to return irqs");
		return -EINVAL;
	}

	for (i = 0; i < sc->num_msi_intrs; i++) {
		irq[i] = sc->irq +  i + MSI_ASSIGN_CE_INITIAL;
	}

	return sc->num_msi_intrs;
}


/**
 * hif_pci_cancel_deferred_target_sleep() - cancels the defered target sleep
 * @scn: hif_softc
 *
 * Return: void
 */
#if CONFIG_ATH_PCIE_MAX_PERF == 0
void hif_pci_cancel_deferred_target_sleep(struct hif_softc *scn)
{
	struct HIF_CE_state *hif_state = HIF_GET_CE_STATE(scn);
	A_target_id_t pci_addr = scn->mem;

	qdf_spin_lock_irqsave(&hif_state->keep_awake_lock);
	/*
	 * If the deferred sleep timer is running cancel it
	 * and put the soc into sleep.
	 */
	if (hif_state->fake_sleep == true) {
		qdf_timer_stop(&hif_state->sleep_timer);
		if (hif_state->verified_awake == false) {
			hif_write32_mb(scn, pci_addr + PCIE_LOCAL_BASE_ADDRESS +
				      PCIE_SOC_WAKE_ADDRESS,
				      PCIE_SOC_WAKE_RESET);
		}
		hif_state->fake_sleep = false;
	}
	qdf_spin_unlock_irqrestore(&hif_state->keep_awake_lock);
}
#else
inline void hif_pci_cancel_deferred_target_sleep(struct hif_softc *scn)
{
}
#endif

#define A_PCIE_LOCAL_REG_READ(sc, mem, addr) \
	hif_read32_mb(sc, (char *)(mem) + \
	PCIE_LOCAL_BASE_ADDRESS + (uint32_t)(addr))

#define A_PCIE_LOCAL_REG_WRITE(sc, mem, addr, val) \
	hif_write32_mb(sc, ((char *)(mem) + \
	PCIE_LOCAL_BASE_ADDRESS + (uint32_t)(addr)), (val))

#ifdef QCA_WIFI_3_0
/**
 * hif_targ_is_awake() - check to see if the target is awake
 * @hif_ctx: hif context
 *
 * emulation never goes to sleep
 *
 * Return: true if target is awake
 */
static bool hif_targ_is_awake(struct hif_softc *hif_ctx, void *__iomem *mem)
{
	return true;
}
#else
/**
 * hif_targ_is_awake() - check to see if the target is awake
 * @hif_ctx: hif context
 *
 * Return: true if the targets clocks are on
 */
static bool hif_targ_is_awake(struct hif_softc *scn, void *__iomem *mem)
{
	uint32_t val;

	if (scn->recovery)
		return false;
	val = hif_read32_mb(scn, mem + PCIE_LOCAL_BASE_ADDRESS
		+ RTC_STATE_ADDRESS);
	return (RTC_STATE_V_GET(val) & RTC_STATE_V_ON) == RTC_STATE_V_ON;
}
#endif

#define ATH_PCI_RESET_WAIT_MAX 10       /* Ms */
static void hif_pci_device_reset(struct hif_pci_softc *sc)
{
	void __iomem *mem = sc->mem;
	int i;
	uint32_t val;
	struct hif_softc *scn = HIF_GET_SOFTC(sc);

	if (!scn->hostdef)
		return;

	/* NB: Don't check resetok here.  This form of reset
	 * is integral to correct operation.
	 */

	if (!SOC_GLOBAL_RESET_ADDRESS)
		return;

	if (!mem)
		return;

	HIF_ERROR("%s: Reset Device", __func__);

	/*
	 * NB: If we try to write SOC_GLOBAL_RESET_ADDRESS without first
	 * writing WAKE_V, the Target may scribble over Host memory!
	 */
	A_PCIE_LOCAL_REG_WRITE(sc, mem, PCIE_SOC_WAKE_ADDRESS,
			       PCIE_SOC_WAKE_V_MASK);
	for (i = 0; i < ATH_PCI_RESET_WAIT_MAX; i++) {
		if (hif_targ_is_awake(scn, mem))
			break;

		qdf_mdelay(1);
	}

	/* Put Target, including PCIe, into RESET. */
	val = A_PCIE_LOCAL_REG_READ(sc, mem, SOC_GLOBAL_RESET_ADDRESS);
	val |= 1;
	A_PCIE_LOCAL_REG_WRITE(sc, mem, SOC_GLOBAL_RESET_ADDRESS, val);
	for (i = 0; i < ATH_PCI_RESET_WAIT_MAX; i++) {
		if (A_PCIE_LOCAL_REG_READ(sc, mem, RTC_STATE_ADDRESS) &
		    RTC_STATE_COLD_RESET_MASK)
			break;

		qdf_mdelay(1);
	}

	/* Pull Target, including PCIe, out of RESET. */
	val &= ~1;
	A_PCIE_LOCAL_REG_WRITE(sc, mem, SOC_GLOBAL_RESET_ADDRESS, val);
	for (i = 0; i < ATH_PCI_RESET_WAIT_MAX; i++) {
		if (!
		    (A_PCIE_LOCAL_REG_READ(sc, mem, RTC_STATE_ADDRESS) &
		     RTC_STATE_COLD_RESET_MASK))
			break;

		qdf_mdelay(1);
	}

	A_PCIE_LOCAL_REG_WRITE(sc, mem, PCIE_SOC_WAKE_ADDRESS,
			       PCIE_SOC_WAKE_RESET);
}

/* CPU warm reset function
 * Steps:
 * 1. Disable all pending interrupts - so no pending interrupts on WARM reset
 * 2. Clear the FW_INDICATOR_ADDRESS -so Traget CPU initializes FW
 *    correctly on WARM reset
 * 3. Clear TARGET CPU LF timer interrupt
 * 4. Reset all CEs to clear any pending CE tarnsactions
 * 5. Warm reset CPU
 */
static void hif_pci_device_warm_reset(struct hif_pci_softc *sc)
{
	void __iomem *mem = sc->mem;
	int i;
	uint32_t val;
	uint32_t fw_indicator;
	struct hif_softc *scn = HIF_GET_SOFTC(sc);

	/* NB: Don't check resetok here.  This form of reset is
	 * integral to correct operation.
	 */

	if (!mem)
		return;

	HIF_INFO_MED("%s: Target Warm Reset", __func__);

	/*
	 * NB: If we try to write SOC_GLOBAL_RESET_ADDRESS without first
	 * writing WAKE_V, the Target may scribble over Host memory!
	 */
	A_PCIE_LOCAL_REG_WRITE(sc, mem, PCIE_SOC_WAKE_ADDRESS,
			       PCIE_SOC_WAKE_V_MASK);
	for (i = 0; i < ATH_PCI_RESET_WAIT_MAX; i++) {
		if (hif_targ_is_awake(scn, mem))
			break;
		qdf_mdelay(1);
	}

	/*
	 * Disable Pending interrupts
	 */
	val =
		hif_read32_mb(sc, mem +
			     (SOC_CORE_BASE_ADDRESS |
			      PCIE_INTR_CAUSE_ADDRESS));
	HIF_INFO_MED("%s: Host Intr Cause reg 0x%x : value : 0x%x", __func__,
		    (SOC_CORE_BASE_ADDRESS | PCIE_INTR_CAUSE_ADDRESS), val);
	/* Target CPU Intr Cause */
	val = hif_read32_mb(sc, mem +
			    (SOC_CORE_BASE_ADDRESS | CPU_INTR_ADDRESS));
	HIF_INFO_MED("%s: Target CPU Intr Cause 0x%x", __func__, val);

	val =
		hif_read32_mb(sc, mem +
			     (SOC_CORE_BASE_ADDRESS |
			      PCIE_INTR_ENABLE_ADDRESS));
	hif_write32_mb(sc, (mem +
		       (SOC_CORE_BASE_ADDRESS | PCIE_INTR_ENABLE_ADDRESS)), 0);
	hif_write32_mb(sc, (mem +
		       (SOC_CORE_BASE_ADDRESS + PCIE_INTR_CLR_ADDRESS)),
		       HOST_GROUP0_MASK);

	qdf_mdelay(100);

	/* Clear FW_INDICATOR_ADDRESS */
	if (HAS_FW_INDICATOR) {
		fw_indicator = hif_read32_mb(sc, mem + FW_INDICATOR_ADDRESS);
		hif_write32_mb(sc, mem + FW_INDICATOR_ADDRESS, 0);
	}

	/* Clear Target LF Timer interrupts */
	val =
		hif_read32_mb(sc, mem +
			     (RTC_SOC_BASE_ADDRESS +
			      SOC_LF_TIMER_CONTROL0_ADDRESS));
	HIF_INFO_MED("%s: addr 0x%x :  0x%x", __func__,
	       (RTC_SOC_BASE_ADDRESS + SOC_LF_TIMER_CONTROL0_ADDRESS), val);
	val &= ~SOC_LF_TIMER_CONTROL0_ENABLE_MASK;
	hif_write32_mb(sc, mem +
		      (RTC_SOC_BASE_ADDRESS + SOC_LF_TIMER_CONTROL0_ADDRESS),
		      val);

	/* Reset CE */
	val =
		hif_read32_mb(sc, mem +
			     (RTC_SOC_BASE_ADDRESS |
			      SOC_RESET_CONTROL_ADDRESS));
	val |= SOC_RESET_CONTROL_CE_RST_MASK;
	hif_write32_mb(sc, (mem +
		       (RTC_SOC_BASE_ADDRESS | SOC_RESET_CONTROL_ADDRESS)),
		      val);
	val =
		hif_read32_mb(sc, mem +
			     (RTC_SOC_BASE_ADDRESS |
			      SOC_RESET_CONTROL_ADDRESS));
	qdf_mdelay(10);

	/* CE unreset */
	val &= ~SOC_RESET_CONTROL_CE_RST_MASK;
	hif_write32_mb(sc, mem + (RTC_SOC_BASE_ADDRESS |
		       SOC_RESET_CONTROL_ADDRESS), val);
	val =
		hif_read32_mb(sc, mem +
			     (RTC_SOC_BASE_ADDRESS |
			      SOC_RESET_CONTROL_ADDRESS));
	qdf_mdelay(10);

	/* Read Target CPU Intr Cause */
	val = hif_read32_mb(sc, mem +
			    (SOC_CORE_BASE_ADDRESS | CPU_INTR_ADDRESS));
	HIF_INFO_MED("%s: Target CPU Intr Cause after CE reset 0x%x",
		    __func__, val);

	/* CPU warm RESET */
	val =
		hif_read32_mb(sc, mem +
			     (RTC_SOC_BASE_ADDRESS |
			      SOC_RESET_CONTROL_ADDRESS));
	val |= SOC_RESET_CONTROL_CPU_WARM_RST_MASK;
	hif_write32_mb(sc, mem + (RTC_SOC_BASE_ADDRESS |
		       SOC_RESET_CONTROL_ADDRESS), val);
	val =
		hif_read32_mb(sc, mem +
			     (RTC_SOC_BASE_ADDRESS |
			      SOC_RESET_CONTROL_ADDRESS));
	HIF_INFO_MED("%s: RESET_CONTROL after cpu warm reset 0x%x",
		    __func__, val);

	qdf_mdelay(100);
	HIF_INFO_MED("%s: Target Warm reset complete", __func__);

}

#ifndef QCA_WIFI_3_0
/* only applicable to legacy ce */
int hif_check_fw_reg(struct hif_opaque_softc *hif_ctx)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(scn);
	void __iomem *mem = sc->mem;
	uint32_t val;

	if (Q_TARGET_ACCESS_BEGIN(scn) < 0)
		return ATH_ISR_NOSCHED;
	val = hif_read32_mb(sc, mem + FW_INDICATOR_ADDRESS);
	if (Q_TARGET_ACCESS_END(scn) < 0)
		return ATH_ISR_SCHED;

	HIF_INFO_MED("%s: FW_INDICATOR register is 0x%x", __func__, val);

	if (val & FW_IND_HELPER)
		return 0;

	return 1;
}
#endif

int hif_check_soc_status(struct hif_opaque_softc *hif_ctx)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);
	uint16_t device_id = 0;
	uint32_t val;
	uint16_t timeout_count = 0;
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(scn);

	/* Check device ID from PCIe configuration space for link status */
	pfrm_read_config_word(sc->pdev, PCI_DEVICE_ID, &device_id);
	if (device_id != sc->devid) {
		HIF_ERROR("%s: device ID does match (read 0x%x, expect 0x%x)",
			  __func__, device_id, sc->devid);
		return -EACCES;
	}

	/* Check PCIe local register for bar/memory access */
	val = hif_read32_mb(sc, sc->mem + PCIE_LOCAL_BASE_ADDRESS +
			   RTC_STATE_ADDRESS);
	HIF_INFO_MED("%s: RTC_STATE_ADDRESS is %08x", __func__, val);

	/* Try to wake up taget if it sleeps */
	hif_write32_mb(sc, sc->mem + PCIE_LOCAL_BASE_ADDRESS +
		PCIE_SOC_WAKE_ADDRESS, PCIE_SOC_WAKE_V_MASK);
	HIF_INFO_MED("%s: PCIE_SOC_WAKE_ADDRESS is %08x", __func__,
		hif_read32_mb(sc, sc->mem + PCIE_LOCAL_BASE_ADDRESS +
		PCIE_SOC_WAKE_ADDRESS));

	/* Check if taget can be woken up */
	while (!hif_targ_is_awake(scn, sc->mem)) {
		if (timeout_count >= PCIE_WAKE_TIMEOUT) {
			HIF_ERROR("%s: wake up timeout, %08x, %08x",
				__func__,
				hif_read32_mb(sc, sc->mem +
					     PCIE_LOCAL_BASE_ADDRESS +
					     RTC_STATE_ADDRESS),
				hif_read32_mb(sc, sc->mem +
					     PCIE_LOCAL_BASE_ADDRESS +
					PCIE_SOC_WAKE_ADDRESS));
			return -EACCES;
		}

		hif_write32_mb(sc, sc->mem + PCIE_LOCAL_BASE_ADDRESS +
			      PCIE_SOC_WAKE_ADDRESS, PCIE_SOC_WAKE_V_MASK);

		qdf_mdelay(100);
		timeout_count += 100;
	}

	/* Check Power register for SoC internal bus issues */
	val =
		hif_read32_mb(sc, sc->mem + RTC_SOC_BASE_ADDRESS +
			     SOC_POWER_REG_OFFSET);
	HIF_INFO_MED("%s: Power register is %08x", __func__, val);

	return 0;
}

/**
 * __hif_pci_dump_registers(): dump other PCI debug registers
 * @scn: struct hif_softc
 *
 * This function dumps pci debug registers.  The parrent function
 * dumps the copy engine registers before calling this function.
 *
 * Return: void
 */
static void __hif_pci_dump_registers(struct hif_softc *scn)
{
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(scn);
	void __iomem *mem = sc->mem;
	uint32_t val, i, j;
	uint32_t wrapper_idx[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
	uint32_t ce_base;

	if (Q_TARGET_ACCESS_BEGIN(scn) < 0)
		return;

	/* DEBUG_INPUT_SEL_SRC = 0x6 */
	val =
		hif_read32_mb(sc, mem + GPIO_BASE_ADDRESS +
			     WLAN_DEBUG_INPUT_SEL_OFFSET);
	val &= ~WLAN_DEBUG_INPUT_SEL_SRC_MASK;
	val |= WLAN_DEBUG_INPUT_SEL_SRC_SET(0x6);
	hif_write32_mb(sc, mem + GPIO_BASE_ADDRESS +
		       WLAN_DEBUG_INPUT_SEL_OFFSET, val);

	/* DEBUG_CONTROL_ENABLE = 0x1 */
	val = hif_read32_mb(sc, mem + GPIO_BASE_ADDRESS +
			   WLAN_DEBUG_CONTROL_OFFSET);
	val &= ~WLAN_DEBUG_CONTROL_ENABLE_MASK;
	val |= WLAN_DEBUG_CONTROL_ENABLE_SET(0x1);
	hif_write32_mb(sc, mem + GPIO_BASE_ADDRESS +
		      WLAN_DEBUG_CONTROL_OFFSET, val);

	HIF_INFO_MED("%s: Debug: inputsel: %x dbgctrl: %x", __func__,
	       hif_read32_mb(sc, mem + GPIO_BASE_ADDRESS +
			    WLAN_DEBUG_INPUT_SEL_OFFSET),
	       hif_read32_mb(sc, mem + GPIO_BASE_ADDRESS +
			    WLAN_DEBUG_CONTROL_OFFSET));

	HIF_INFO_MED("%s: Debug CE", __func__);
	/* Loop CE debug output */
	/* AMBA_DEBUG_BUS_SEL = 0xc */
	val = hif_read32_mb(sc, mem + GPIO_BASE_ADDRESS +
			    AMBA_DEBUG_BUS_OFFSET);
	val &= ~AMBA_DEBUG_BUS_SEL_MASK;
	val |= AMBA_DEBUG_BUS_SEL_SET(0xc);
	hif_write32_mb(sc, mem + GPIO_BASE_ADDRESS + AMBA_DEBUG_BUS_OFFSET,
		       val);

	for (i = 0; i < sizeof(wrapper_idx) / sizeof(uint32_t); i++) {
		/* For (i=1,2,3,4,8,9) write CE_WRAPPER_DEBUG_SEL = i */
		val = hif_read32_mb(sc, mem + CE_WRAPPER_BASE_ADDRESS +
				   CE_WRAPPER_DEBUG_OFFSET);
		val &= ~CE_WRAPPER_DEBUG_SEL_MASK;
		val |= CE_WRAPPER_DEBUG_SEL_SET(wrapper_idx[i]);
		hif_write32_mb(sc, mem + CE_WRAPPER_BASE_ADDRESS +
			      CE_WRAPPER_DEBUG_OFFSET, val);

		HIF_INFO_MED("%s: ce wrapper: %d amdbg: %x cewdbg: %x",
			    __func__, wrapper_idx[i],
			    hif_read32_mb(sc, mem + GPIO_BASE_ADDRESS +
				AMBA_DEBUG_BUS_OFFSET),
			    hif_read32_mb(sc, mem + CE_WRAPPER_BASE_ADDRESS +
				CE_WRAPPER_DEBUG_OFFSET));

		if (wrapper_idx[i] <= 7) {
			for (j = 0; j <= 5; j++) {
				ce_base = CE_BASE_ADDRESS(wrapper_idx[i]);
				/* For (j=0~5) write CE_DEBUG_SEL = j */
				val =
					hif_read32_mb(sc, mem + ce_base +
						     CE_DEBUG_OFFSET);
				val &= ~CE_DEBUG_SEL_MASK;
				val |= CE_DEBUG_SEL_SET(j);
				hif_write32_mb(sc, mem + ce_base +
					       CE_DEBUG_OFFSET, val);

				/* read (@gpio_athr_wlan_reg)
				 * WLAN_DEBUG_OUT_DATA
				 */
				val = hif_read32_mb(sc, mem + GPIO_BASE_ADDRESS
						    + WLAN_DEBUG_OUT_OFFSET);
				val = WLAN_DEBUG_OUT_DATA_GET(val);

				HIF_INFO_MED("%s: module%d: cedbg: %x out: %x",
					    __func__, j,
					    hif_read32_mb(sc, mem + ce_base +
						    CE_DEBUG_OFFSET), val);
			}
		} else {
			/* read (@gpio_athr_wlan_reg) WLAN_DEBUG_OUT_DATA */
			val =
				hif_read32_mb(sc, mem + GPIO_BASE_ADDRESS +
					     WLAN_DEBUG_OUT_OFFSET);
			val = WLAN_DEBUG_OUT_DATA_GET(val);

			HIF_INFO_MED("%s: out: %x", __func__, val);
		}
	}

	HIF_INFO_MED("%s: Debug PCIe:", __func__);
	/* Loop PCIe debug output */
	/* Write AMBA_DEBUG_BUS_SEL = 0x1c */
	val = hif_read32_mb(sc, mem + GPIO_BASE_ADDRESS +
			    AMBA_DEBUG_BUS_OFFSET);
	val &= ~AMBA_DEBUG_BUS_SEL_MASK;
	val |= AMBA_DEBUG_BUS_SEL_SET(0x1c);
	hif_write32_mb(sc, mem + GPIO_BASE_ADDRESS +
		       AMBA_DEBUG_BUS_OFFSET, val);

	for (i = 0; i <= 8; i++) {
		/* For (i=1~8) write AMBA_DEBUG_BUS_PCIE_DEBUG_SEL = i */
		val =
			hif_read32_mb(sc, mem + GPIO_BASE_ADDRESS +
				     AMBA_DEBUG_BUS_OFFSET);
		val &= ~AMBA_DEBUG_BUS_PCIE_DEBUG_SEL_MASK;
		val |= AMBA_DEBUG_BUS_PCIE_DEBUG_SEL_SET(i);
		hif_write32_mb(sc, mem + GPIO_BASE_ADDRESS +
			       AMBA_DEBUG_BUS_OFFSET, val);

		/* read (@gpio_athr_wlan_reg) WLAN_DEBUG_OUT_DATA */
		val =
			hif_read32_mb(sc, mem + GPIO_BASE_ADDRESS +
				     WLAN_DEBUG_OUT_OFFSET);
		val = WLAN_DEBUG_OUT_DATA_GET(val);

		HIF_INFO_MED("%s: amdbg: %x out: %x %x", __func__,
		       hif_read32_mb(sc, mem + GPIO_BASE_ADDRESS +
				    WLAN_DEBUG_OUT_OFFSET), val,
		       hif_read32_mb(sc, mem + GPIO_BASE_ADDRESS +
				    WLAN_DEBUG_OUT_OFFSET));
	}

	Q_TARGET_ACCESS_END(scn);
}

/**
 * hif_dump_registers(): dump bus debug registers
 * @scn: struct hif_opaque_softc
 *
 * This function dumps hif bus debug registers
 *
 * Return: 0 for success or error code
 */
int hif_pci_dump_registers(struct hif_softc *hif_ctx)
{
	int status;
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	status = hif_dump_ce_registers(scn);

	if (status)
		HIF_ERROR("%s: Dump CE Registers Failed", __func__);

	/* dump non copy engine pci registers */
	__hif_pci_dump_registers(scn);

	return 0;
}

#ifdef HIF_CONFIG_SLUB_DEBUG_ON

/* worker thread to schedule wlan_tasklet in SLUB debug build */
static void reschedule_tasklet_work_handler(void *arg)
{
	struct hif_pci_softc *sc = arg;
	struct hif_softc *scn = HIF_GET_SOFTC(sc);

	if (!scn) {
		HIF_ERROR("%s: hif_softc is NULL\n", __func__);
		return;
	}

	if (scn->hif_init_done == false) {
		HIF_ERROR("%s: wlan driver is unloaded", __func__);
		return;
	}

	tasklet_schedule(&sc->intr_tq);
}

/**
 * hif_init_reschedule_tasklet_work() - API to initialize reschedule tasklet
 * work
 * @sc: HIF PCI Context
 *
 * Return: void
 */
static void hif_init_reschedule_tasklet_work(struct hif_pci_softc *sc)
{
	qdf_create_work(0, &sc->reschedule_tasklet_work,
				reschedule_tasklet_work_handler, NULL);
}
#else
static void hif_init_reschedule_tasklet_work(struct hif_pci_softc *sc) { }
#endif /* HIF_CONFIG_SLUB_DEBUG_ON */

void wlan_tasklet(unsigned long data)
{
	struct hif_pci_softc *sc = (struct hif_pci_softc *)data;
	struct hif_softc *scn = HIF_GET_SOFTC(sc);

	if (scn->hif_init_done == false)
		goto end;

	if (qdf_atomic_read(&scn->link_suspended))
		goto end;

	if (!ADRASTEA_BU) {
		hif_fw_interrupt_handler(sc->irq_event, scn);
		if (scn->target_status == TARGET_STATUS_RESET)
			goto end;
	}

end:
	qdf_atomic_set(&scn->tasklet_from_intr, 0);
	qdf_atomic_dec(&scn->active_tasklet_cnt);
}

#ifdef FEATURE_RUNTIME_PM
static bool hif_pci_pm_runtime_enabled(struct hif_pci_softc *sc)
{
	struct hif_softc *scn = HIF_GET_SOFTC(sc);

	if (scn->hif_config.enable_runtime_pm)
		return true;

	return pm_runtime_enabled(sc->dev);
}

static const char *hif_pm_runtime_state_to_string(uint32_t state)
{
	switch (state) {
	case HIF_PM_RUNTIME_STATE_NONE:
		return "INIT_STATE";
	case HIF_PM_RUNTIME_STATE_ON:
		return "ON";
	case HIF_PM_RUNTIME_STATE_RESUMING:
		return "RESUMING";
	case HIF_PM_RUNTIME_STATE_SUSPENDING:
		return "SUSPENDING";
	case HIF_PM_RUNTIME_STATE_SUSPENDED:
		return "SUSPENDED";
	default:
		return "INVALID STATE";
	}
}

#define HIF_PCI_RUNTIME_PM_STATS(_s, _sc, _name) \
	seq_printf(_s, "%30s: %u\n", #_name, _sc->pm_stats._name)
/**
 * hif_pci_runtime_pm_warn() - Runtime PM Debugging API
 * @sc: hif_pci_softc context
 * @msg: log message
 *
 * log runtime pm stats when something seems off.
 *
 * Return: void
 */
static void hif_pci_runtime_pm_warn(struct hif_pci_softc *sc, const char *msg)
{
	struct hif_pm_runtime_lock *ctx;
	int i;

	hif_nofl_debug("%s: usage_count: %d, pm_state: %s, prevent_suspend_cnt: %d",
		       msg, atomic_read(&sc->dev->power.usage_count),
		       hif_pm_runtime_state_to_string(
				atomic_read(&sc->pm_state)),
		       sc->prevent_suspend_cnt);

	hif_nofl_debug("runtime_status: %d, runtime_error: %d, disable_depth: %d autosuspend_delay: %d",
		       sc->dev->power.runtime_status,
		       sc->dev->power.runtime_error,
		       sc->dev->power.disable_depth,
		       sc->dev->power.autosuspend_delay);

	hif_nofl_debug("runtime_get: %u, runtime_put: %u, request_resume: %u",
		       qdf_atomic_read(&sc->pm_stats.runtime_get),
		       qdf_atomic_read(&sc->pm_stats.runtime_put),
		       sc->pm_stats.request_resume);

	hif_nofl_debug("get     put     get-timestamp put-timestamp :DBGID_NAME");
	for (i = 0; i < RTPM_ID_MAX; i++) {
		hif_nofl_debug("%-10d %-10d  0x%-10llx  0x%-10llx :%-30s",
			       qdf_atomic_read(
				       &sc->pm_stats.runtime_get_dbgid[i]),
			       qdf_atomic_read(
				       &sc->pm_stats.runtime_put_dbgid[i]),
			       sc->pm_stats.runtime_get_timestamp_dbgid[i],
			       sc->pm_stats.runtime_put_timestamp_dbgid[i],
			       rtpm_string_from_dbgid(i));
	}

	hif_nofl_debug("allow_suspend: %u, prevent_suspend: %u",
		       qdf_atomic_read(&sc->pm_stats.allow_suspend),
		       qdf_atomic_read(&sc->pm_stats.prevent_suspend));

	hif_nofl_debug("prevent_suspend_timeout: %u, allow_suspend_timeout: %u",
		       sc->pm_stats.prevent_suspend_timeout,
		       sc->pm_stats.allow_suspend_timeout);

	hif_nofl_debug("Suspended: %u, resumed: %u count",
		       sc->pm_stats.suspended,
		       sc->pm_stats.resumed);

	hif_nofl_debug("suspend_err: %u, runtime_get_err: %u",
		       sc->pm_stats.suspend_err,
		       sc->pm_stats.runtime_get_err);

	hif_nofl_debug("Active Wakeup Sources preventing Runtime Suspend: ");

	list_for_each_entry(ctx, &sc->prevent_suspend_list, list) {
		hif_nofl_debug("source %s; timeout %d ms",
			       ctx->name, ctx->timeout);
	}

	if (qdf_is_fw_down()) {
		hif_err("fw is down");
		return;
	}

	QDF_ASSERT(0);
}

/**
 * hif_pci_pm_runtime_debugfs_show(): show debug stats for runtimepm
 * @s: file to print to
 * @data: unused
 *
 * debugging tool added to the debug fs for displaying runtimepm stats
 *
 * Return: 0
 */
static int hif_pci_pm_runtime_debugfs_show(struct seq_file *s, void *data)
{
	struct hif_pci_softc *sc = s->private;
	static const char * const autopm_state[] = {"NONE", "ON", "RESUMING",
		"SUSPENDING", "SUSPENDED"};
	unsigned int msecs_age;
	qdf_time_t usecs_age;
	int pm_state = atomic_read(&sc->pm_state);
	unsigned long timer_expires;
	struct hif_pm_runtime_lock *ctx;
	int i;

	seq_printf(s, "%30s: %s\n", "Runtime PM state",
		   autopm_state[pm_state]);
	seq_printf(s, "%30s: %pf\n", "Last Resume Caller",
		   sc->pm_stats.last_resume_caller);
	seq_printf(s, "%30s: %pf\n", "Last Busy Marker",
		   sc->pm_stats.last_busy_marker);

	usecs_age = qdf_get_log_timestamp_usecs() -
		sc->pm_stats.last_busy_timestamp;
	seq_printf(s, "%30s: %lu.%06lus\n", "Last Busy Timestamp",
		   sc->pm_stats.last_busy_timestamp / 1000000,
		   sc->pm_stats.last_busy_timestamp % 1000000);
	seq_printf(s, "%30s: %lu.%06lus\n", "Last Busy Since",
		   usecs_age / 1000000, usecs_age % 1000000);

	if (pm_state == HIF_PM_RUNTIME_STATE_SUSPENDED) {
		msecs_age = jiffies_to_msecs(jiffies -
					     sc->pm_stats.suspend_jiffies);
		seq_printf(s, "%30s: %d.%03ds\n", "Suspended Since",
			   msecs_age / 1000, msecs_age % 1000);
	}

	seq_printf(s, "%30s: %d\n", "PM Usage count",
		   atomic_read(&sc->dev->power.usage_count));

	seq_printf(s, "%30s: %u\n", "prevent_suspend_cnt",
		   sc->prevent_suspend_cnt);

	HIF_PCI_RUNTIME_PM_STATS(s, sc, suspended);
	HIF_PCI_RUNTIME_PM_STATS(s, sc, suspend_err);
	HIF_PCI_RUNTIME_PM_STATS(s, sc, resumed);

	HIF_PCI_RUNTIME_PM_STATS(s, sc, request_resume);
	seq_printf(s, "%30s: %u\n", "prevent_suspend",
		   qdf_atomic_read(&sc->pm_stats.prevent_suspend));
	seq_printf(s, "%30s: %u\n", "allow_suspend",
		   qdf_atomic_read(&sc->pm_stats.allow_suspend));

	HIF_PCI_RUNTIME_PM_STATS(s, sc, prevent_suspend_timeout);
	HIF_PCI_RUNTIME_PM_STATS(s, sc, allow_suspend_timeout);
	HIF_PCI_RUNTIME_PM_STATS(s, sc, runtime_get_err);

	seq_printf(s, "%30s: %u\n", "runtime_get",
		   qdf_atomic_read(&sc->pm_stats.runtime_get));
	seq_printf(s, "%30s: %u\n", "runtime_put",
		   qdf_atomic_read(&sc->pm_stats.runtime_put));
	seq_printf(s, "get     put     get-timestamp put-timestamp :DBGID_NAME\n");
	for (i = 0; i < RTPM_ID_MAX; i++) {
		seq_printf(s, "%-10d ",
			   qdf_atomic_read(&sc->pm_stats.runtime_get_dbgid[i]));
		seq_printf(s, "%-10d ",
			   qdf_atomic_read(&sc->pm_stats.runtime_put_dbgid[i]));
		seq_printf(s, "0x%-10llx ",
			   sc->pm_stats.runtime_get_timestamp_dbgid[i]);
		seq_printf(s, "0x%-10llx ",
			   sc->pm_stats.runtime_put_timestamp_dbgid[i]);
		seq_printf(s, ":%-30s\n", rtpm_string_from_dbgid(i));
	}

	timer_expires = sc->runtime_timer_expires;
	if (timer_expires > 0) {
		msecs_age = jiffies_to_msecs(timer_expires - jiffies);
		seq_printf(s, "%30s: %d.%03ds\n", "Prevent suspend timeout",
			   msecs_age / 1000, msecs_age % 1000);
	}

	spin_lock_bh(&sc->runtime_lock);
	if (list_empty(&sc->prevent_suspend_list)) {
		spin_unlock_bh(&sc->runtime_lock);
		return 0;
	}

	seq_printf(s, "%30s: ", "Active Wakeup_Sources");
	list_for_each_entry(ctx, &sc->prevent_suspend_list, list) {
		seq_printf(s, "%s", ctx->name);
		if (ctx->timeout)
			seq_printf(s, "(%d ms)", ctx->timeout);
		seq_puts(s, " ");
	}
	seq_puts(s, "\n");
	spin_unlock_bh(&sc->runtime_lock);

	return 0;
}
#undef HIF_PCI_RUNTIME_PM_STATS

/**
 * hif_pci_autopm_open() - open a debug fs file to access the runtime pm stats
 * @inode
 * @file
 *
 * Return: linux error code of single_open.
 */
static int hif_pci_runtime_pm_open(struct inode *inode, struct file *file)
{
	return single_open(file, hif_pci_pm_runtime_debugfs_show,
			inode->i_private);
}

static const struct file_operations hif_pci_runtime_pm_fops = {
	.owner          = THIS_MODULE,
	.open           = hif_pci_runtime_pm_open,
	.release        = single_release,
	.read           = seq_read,
	.llseek         = seq_lseek,
};

#ifdef CONFIG_WLAN_DEBUGFS
/**
 * hif_runtime_pm_debugfs_create() - creates runtimepm debugfs entry
 * @sc: pci context
 *
 * creates a debugfs entry to debug the runtime pm feature.
 */
static void hif_runtime_pm_debugfs_create(struct hif_pci_softc *sc)
{
	sc->pm_dentry = debugfs_create_file("cnss_runtime_pm",
					0400, NULL, sc,
					&hif_pci_runtime_pm_fops);
}

/**
 * hif_runtime_pm_debugfs_remove() - removes runtimepm debugfs entry
 * @sc: pci context
 *
 * removes the debugfs entry to debug the runtime pm feature.
 */
static void hif_runtime_pm_debugfs_remove(struct hif_pci_softc *sc)
{
	debugfs_remove(sc->pm_dentry);
}
#endif

static void hif_runtime_init(struct device *dev, int delay)
{
	pm_runtime_set_autosuspend_delay(dev, delay);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_allow(dev);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_noidle(dev);
	pm_suspend_ignore_children(dev, true);
}

static void hif_runtime_exit(struct device *dev)
{
	pm_runtime_get_noresume(dev);
	pm_runtime_set_active(dev);
}

static void hif_pm_runtime_lock_timeout_fn(void *data);

/**
 * hif_pm_runtime_start(): start the runtime pm
 * @sc: pci context
 *
 * After this call, runtime pm will be active.
 */
static void hif_pm_runtime_start(struct hif_pci_softc *sc)
{
	struct hif_softc *ol_sc = HIF_GET_SOFTC(sc);
	uint32_t mode = hif_get_conparam(ol_sc);

	if (!ol_sc->hif_config.enable_runtime_pm) {
		HIF_INFO("%s: RUNTIME PM is disabled in ini\n", __func__);
		return;
	}

	if (mode == QDF_GLOBAL_FTM_MODE || QDF_IS_EPPING_ENABLED(mode) ||
	    mode == QDF_GLOBAL_MONITOR_MODE) {
		HIF_INFO("%s: RUNTIME PM is disabled for FTM/EPPING mode\n",
				__func__);
		return;
	}

	qdf_timer_init(NULL, &sc->runtime_timer,
		       hif_pm_runtime_lock_timeout_fn,
		       sc, QDF_TIMER_TYPE_WAKE_APPS);

	HIF_INFO("%s: Enabling RUNTIME PM, Delay: %d ms", __func__,
			ol_sc->hif_config.runtime_pm_delay);

	qdf_atomic_set(&sc->pm_state, HIF_PM_RUNTIME_STATE_ON);
	hif_runtime_init(sc->dev, ol_sc->hif_config.runtime_pm_delay);
#ifdef CONFIG_WLAN_DEBUGFS
	hif_runtime_pm_debugfs_create(sc);
#endif
}

/**
 * hif_pm_runtime_stop(): stop runtime pm
 * @sc: pci context
 *
 * Turns off runtime pm and frees corresponding resources
 * that were acquired by hif_runtime_pm_start().
 */
static void hif_pm_runtime_stop(struct hif_pci_softc *sc)
{
	struct hif_softc *ol_sc = HIF_GET_SOFTC(sc);
	uint32_t mode = hif_get_conparam(ol_sc);

	if (!ol_sc->hif_config.enable_runtime_pm)
		return;

	if (mode == QDF_GLOBAL_FTM_MODE || QDF_IS_EPPING_ENABLED(mode) ||
	    mode == QDF_GLOBAL_MONITOR_MODE)
		return;

	hif_runtime_exit(sc->dev);

	hif_pm_runtime_sync_resume(GET_HIF_OPAQUE_HDL(sc));

	qdf_atomic_set(&sc->pm_state, HIF_PM_RUNTIME_STATE_NONE);

#ifdef CONFIG_WLAN_DEBUGFS
	hif_runtime_pm_debugfs_remove(sc);
#endif

	qdf_timer_free(&sc->runtime_timer);
	/* doesn't wait for penting trafic unlike cld-2.0 */
}

/**
 * hif_pm_runtime_open(): initialize runtime pm
 * @sc: pci data structure
 *
 * Early initialization
 */
static void hif_pm_runtime_open(struct hif_pci_softc *sc)
{
	int i;
	spin_lock_init(&sc->runtime_lock);

	qdf_atomic_init(&sc->pm_state);
	qdf_runtime_lock_init(&sc->prevent_linkdown_lock);
	qdf_atomic_set(&sc->pm_state, HIF_PM_RUNTIME_STATE_NONE);
	qdf_atomic_init(&sc->pm_stats.runtime_get);
	qdf_atomic_init(&sc->pm_stats.runtime_put);
	qdf_atomic_init(&sc->pm_stats.allow_suspend);
	qdf_atomic_init(&sc->pm_stats.prevent_suspend);
	for (i = 0; i < RTPM_ID_MAX; i++) {
		qdf_atomic_init(&sc->pm_stats.runtime_get_dbgid[i]);
		qdf_atomic_init(&sc->pm_stats.runtime_put_dbgid[i]);
	}
	INIT_LIST_HEAD(&sc->prevent_suspend_list);
}

/**
 * hif_pm_runtime_sanitize_on_exit(): sanitize the pm usage count and state
 * @sc: pci context
 *
 * Ensure we have only one vote against runtime suspend before closing
 * the runtime suspend feature.
 *
 * all gets by the wlan driver should have been returned
 * one vote should remain as part of cnss_runtime_exit
 *
 * needs to be revisited if we share the root complex.
 */
static void hif_pm_runtime_sanitize_on_exit(struct hif_pci_softc *sc)
{
	struct hif_pm_runtime_lock *ctx, *tmp;

	if (atomic_read(&sc->dev->power.usage_count) != 1)
		hif_pci_runtime_pm_warn(sc, "Driver UnLoaded");
	else
		return;

	spin_lock_bh(&sc->runtime_lock);
	list_for_each_entry_safe(ctx, tmp, &sc->prevent_suspend_list, list) {
		spin_unlock_bh(&sc->runtime_lock);
		hif_runtime_lock_deinit(GET_HIF_OPAQUE_HDL(sc), ctx);
		spin_lock_bh(&sc->runtime_lock);
	}
	spin_unlock_bh(&sc->runtime_lock);

	/* ensure 1 and only 1 usage count so that when the wlan
	 * driver is re-insmodded runtime pm won't be
	 * disabled also ensures runtime pm doesn't get
	 * broken on by being less than 1.
	 */
	if (atomic_read(&sc->dev->power.usage_count) <= 0)
		atomic_set(&sc->dev->power.usage_count, 1);
	while (atomic_read(&sc->dev->power.usage_count) > 1)
		hif_pm_runtime_put_auto(sc->dev);
}

static int __hif_pm_runtime_allow_suspend(struct hif_pci_softc *hif_sc,
					  struct hif_pm_runtime_lock *lock);

/**
 * hif_pm_runtime_sanitize_on_ssr_exit() - Empty the suspend list on SSR
 * @sc: PCIe Context
 *
 * API is used to empty the runtime pm prevent suspend list.
 *
 * Return: void
 */
static void hif_pm_runtime_sanitize_on_ssr_exit(struct hif_pci_softc *sc)
{
	struct hif_pm_runtime_lock *ctx, *tmp;

	spin_lock_bh(&sc->runtime_lock);
	list_for_each_entry_safe(ctx, tmp, &sc->prevent_suspend_list, list) {
		__hif_pm_runtime_allow_suspend(sc, ctx);
	}
	spin_unlock_bh(&sc->runtime_lock);
}

/**
 * hif_pm_runtime_close(): close runtime pm
 * @sc: pci bus handle
 *
 * ensure runtime_pm is stopped before closing the driver
 */
static void hif_pm_runtime_close(struct hif_pci_softc *sc)
{
	struct hif_softc *scn = HIF_GET_SOFTC(sc);

	qdf_runtime_lock_deinit(&sc->prevent_linkdown_lock);
	if (qdf_atomic_read(&sc->pm_state) == HIF_PM_RUNTIME_STATE_NONE)
		return;

	hif_pm_runtime_stop(sc);

	hif_is_recovery_in_progress(scn) ?
		hif_pm_runtime_sanitize_on_ssr_exit(sc) :
		hif_pm_runtime_sanitize_on_exit(sc);
}

int hif_pm_runtime_sync_resume(struct hif_opaque_softc *hif_ctx)
{
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(hif_ctx);
	int pm_state;

	if (!sc)
		return -EINVAL;

	if (!hif_pci_pm_runtime_enabled(sc))
		return 0;

	pm_state = qdf_atomic_read(&sc->pm_state);
	if (pm_state == HIF_PM_RUNTIME_STATE_SUSPENDED ||
	    pm_state == HIF_PM_RUNTIME_STATE_SUSPENDING)
		HIF_INFO("Runtime PM resume is requested by %ps",
			 (void *)_RET_IP_);

	sc->pm_stats.request_resume++;
	sc->pm_stats.last_resume_caller = (void *)_RET_IP_;

	return pm_runtime_resume(sc->dev);
}
#else
static void hif_pm_runtime_close(struct hif_pci_softc *sc) {}
static void hif_pm_runtime_open(struct hif_pci_softc *sc) {}
static void hif_pm_runtime_start(struct hif_pci_softc *sc) {}
static void hif_pm_runtime_stop(struct hif_pci_softc *sc) {}
#endif

/**
 * hif_disable_power_gating() - disable HW power gating
 * @hif_ctx: hif context
 *
 * disables pcie L1 power states
 */
static void hif_disable_power_gating(struct hif_opaque_softc *hif_ctx)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(hif_ctx);

	if (!scn) {
		HIF_ERROR("%s: Could not disable ASPM scn is null",
		       __func__);
		return;
	}

	/* Disable ASPM when pkt log is enabled */
	pfrm_read_config_dword(sc->pdev, 0x80, &sc->lcr_val);
	pfrm_write_config_dword(sc->pdev, 0x80, (sc->lcr_val & 0xffffff00));
}

/**
 * hif_enable_power_gating() - enable HW power gating
 * @hif_ctx: hif context
 *
 * enables pcie L1 power states
 */
static void hif_enable_power_gating(struct hif_pci_softc *sc)
{
	if (!sc) {
		HIF_ERROR("%s: Could not disable ASPM scn is null",
		       __func__);
		return;
	}

	/* Re-enable ASPM after firmware/OTP download is complete */
	pfrm_write_config_dword(sc->pdev, 0x80, sc->lcr_val);
}

/**
 * hif_enable_power_management() - enable power management
 * @hif_ctx: hif context
 *
 * Enables runtime pm, aspm(PCI.. hif_enable_power_gating) and re-enabling
 * soc-sleep after driver load (hif_pci_target_sleep_state_adjust).
 *
 * note: epping mode does not call this function as it does not
 *       care about saving power.
 */
void hif_pci_enable_power_management(struct hif_softc *hif_sc,
				 bool is_packet_log_enabled)
{
	struct hif_pci_softc *pci_ctx = HIF_GET_PCI_SOFTC(hif_sc);
	uint32_t mode;

	if (!pci_ctx) {
		HIF_ERROR("%s, hif_ctx null", __func__);
		return;
	}

	mode = hif_get_conparam(hif_sc);
	if (mode == QDF_GLOBAL_FTM_MODE) {
		HIF_INFO("%s: Enable power gating for FTM mode", __func__);
		hif_enable_power_gating(pci_ctx);
		return;
	}

	hif_pm_runtime_start(pci_ctx);

	if (!is_packet_log_enabled)
		hif_enable_power_gating(pci_ctx);

	if (!CONFIG_ATH_PCIE_MAX_PERF &&
	    CONFIG_ATH_PCIE_AWAKE_WHILE_DRIVER_LOAD &&
	    !ce_srng_based(hif_sc)) {
		/* allow sleep for PCIE_AWAKE_WHILE_DRIVER_LOAD feature */
		if (hif_pci_target_sleep_state_adjust(hif_sc, true, false) < 0)
			HIF_ERROR("%s, failed to set target to sleep",
				  __func__);
	}
}

/**
 * hif_disable_power_management() - disable power management
 * @hif_ctx: hif context
 *
 * Currently disables runtime pm. Should be updated to behave
 * if runtime pm is not started. Should be updated to take care
 * of aspm and soc sleep for driver load.
 */
void hif_pci_disable_power_management(struct hif_softc *hif_ctx)
{
	struct hif_pci_softc *pci_ctx = HIF_GET_PCI_SOFTC(hif_ctx);

	if (!pci_ctx) {
		HIF_ERROR("%s, hif_ctx null", __func__);
		return;
	}

	hif_pm_runtime_stop(pci_ctx);
}

void hif_pci_display_stats(struct hif_softc *hif_ctx)
{
	struct hif_pci_softc *pci_ctx = HIF_GET_PCI_SOFTC(hif_ctx);

	if (!pci_ctx) {
		HIF_ERROR("%s, hif_ctx null", __func__);
		return;
	}
	hif_display_ce_stats(hif_ctx);

	hif_print_pci_stats(pci_ctx);
}

void hif_pci_clear_stats(struct hif_softc *hif_ctx)
{
	struct hif_pci_softc *pci_ctx = HIF_GET_PCI_SOFTC(hif_ctx);

	if (!pci_ctx) {
		HIF_ERROR("%s, hif_ctx null", __func__);
		return;
	}
	hif_clear_ce_stats(&pci_ctx->ce_sc);
}

#define ATH_PCI_PROBE_RETRY_MAX 3
/**
 * hif_bus_open(): hif_bus_open
 * @scn: scn
 * @bus_type: bus type
 *
 * Return: n/a
 */
QDF_STATUS hif_pci_open(struct hif_softc *hif_ctx, enum qdf_bus_type bus_type)
{
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(hif_ctx);

	hif_ctx->bus_type = bus_type;
	hif_pm_runtime_open(sc);

	qdf_spinlock_create(&sc->irq_lock);

	return hif_ce_open(hif_ctx);
}

/**
 * hif_wake_target_cpu() - wake the target's cpu
 * @scn: hif context
 *
 * Send an interrupt to the device to wake up the Target CPU
 * so it has an opportunity to notice any changed state.
 */
static void hif_wake_target_cpu(struct hif_softc *scn)
{
	QDF_STATUS rv;
	uint32_t core_ctrl;
	struct hif_opaque_softc *hif_hdl = GET_HIF_OPAQUE_HDL(scn);

	rv = hif_diag_read_access(hif_hdl,
				  SOC_CORE_BASE_ADDRESS | CORE_CTRL_ADDRESS,
				  &core_ctrl);
	QDF_ASSERT(rv == QDF_STATUS_SUCCESS);
	/* A_INUM_FIRMWARE interrupt to Target CPU */
	core_ctrl |= CORE_CTRL_CPU_INTR_MASK;

	rv = hif_diag_write_access(hif_hdl,
				   SOC_CORE_BASE_ADDRESS | CORE_CTRL_ADDRESS,
				   core_ctrl);
	QDF_ASSERT(rv == QDF_STATUS_SUCCESS);
}

/**
 * soc_wake_reset() - allow the target to go to sleep
 * @scn: hif_softc
 *
 * Clear the force wake register.  This is done by
 * hif_sleep_entry and cancel defered timer sleep.
 */
static void soc_wake_reset(struct hif_softc *scn)
{
	hif_write32_mb(scn, scn->mem +
		PCIE_LOCAL_BASE_ADDRESS +
		PCIE_SOC_WAKE_ADDRESS,
		PCIE_SOC_WAKE_RESET);
}

/**
 * hif_sleep_entry() - gate target sleep
 * @arg: hif context
 *
 * This function is the callback for the sleep timer.
 * Check if last force awake critical section was at least
 * HIF_MIN_SLEEP_INACTIVITY_TIME_MS time ago.  if it was,
 * allow the target to go to sleep and cancel the sleep timer.
 * otherwise reschedule the sleep timer.
 */
static void hif_sleep_entry(void *arg)
{
	struct HIF_CE_state *hif_state = (struct HIF_CE_state *)arg;
	struct hif_softc *scn = HIF_GET_SOFTC(hif_state);
	uint32_t idle_ms;

	if (scn->recovery)
		return;

	if (hif_is_driver_unloading(scn))
		return;

	qdf_spin_lock_irqsave(&hif_state->keep_awake_lock);
	if (hif_state->fake_sleep) {
		idle_ms = qdf_system_ticks_to_msecs(qdf_system_ticks()
						    - hif_state->sleep_ticks);
		if (!hif_state->verified_awake &&
		    idle_ms >= HIF_MIN_SLEEP_INACTIVITY_TIME_MS) {
			if (!qdf_atomic_read(&scn->link_suspended)) {
				soc_wake_reset(scn);
				hif_state->fake_sleep = false;
			}
		} else {
			qdf_timer_stop(&hif_state->sleep_timer);
			qdf_timer_start(&hif_state->sleep_timer,
					HIF_SLEEP_INACTIVITY_TIMER_PERIOD_MS);
		}
	}
	qdf_spin_unlock_irqrestore(&hif_state->keep_awake_lock);
}

#define HIF_HIA_MAX_POLL_LOOP    1000000
#define HIF_HIA_POLLING_DELAY_MS 10

#ifdef QCA_HIF_HIA_EXTND

static void hif_set_hia_extnd(struct hif_softc *scn)
{
	struct hif_opaque_softc *hif_hdl = GET_HIF_OPAQUE_HDL(scn);
	struct hif_target_info *tgt_info = hif_get_target_info_handle(hif_hdl);
	uint32_t target_type = tgt_info->target_type;

	HIF_TRACE("%s: E", __func__);

	if ((target_type == TARGET_TYPE_AR900B) ||
			target_type == TARGET_TYPE_QCA9984 ||
			target_type == TARGET_TYPE_QCA9888) {
		/* CHIP revision is 8-11 bits of the CHIP_ID register 0xec
		 * in RTC space
		 */
		tgt_info->target_revision
			= CHIP_ID_REVISION_GET(hif_read32_mb(scn, scn->mem
					+ CHIP_ID_ADDRESS));
		qdf_print("chip_id 0x%x chip_revision 0x%x",
			  target_type, tgt_info->target_revision);
	}

	{
		uint32_t flag2_value = 0;
		uint32_t flag2_targ_addr =
			host_interest_item_address(target_type,
			offsetof(struct host_interest_s, hi_skip_clock_init));

		if ((ar900b_20_targ_clk != -1) &&
			(frac != -1) && (intval != -1)) {
			hif_diag_read_access(hif_hdl, flag2_targ_addr,
				&flag2_value);
			qdf_print("\n Setting clk_override");
			flag2_value |= CLOCK_OVERRIDE;

			hif_diag_write_access(hif_hdl, flag2_targ_addr,
					flag2_value);
			qdf_print("\n CLOCK PLL val set %d", flag2_value);
		} else {
			qdf_print("\n CLOCK PLL skipped");
		}
	}

	if (target_type == TARGET_TYPE_AR900B
			|| target_type == TARGET_TYPE_QCA9984
			|| target_type == TARGET_TYPE_QCA9888) {

		/* for AR9980_2.0, 300 mhz clock is used, right now we assume
		 * this would be supplied through module parameters,
		 * if not supplied assumed default or same behavior as 1.0.
		 * Assume 1.0 clock can't be tuned, reset to defaults
		 */

		qdf_print(KERN_INFO
			  "%s: setting the target pll frac %x intval %x",
			  __func__, frac, intval);

		/* do not touch frac, and int val, let them be default -1,
		 * if desired, host can supply these through module params
		 */
		if (frac != -1 || intval != -1) {
			uint32_t flag2_value = 0;
			uint32_t flag2_targ_addr;

			flag2_targ_addr =
				host_interest_item_address(target_type,
				offsetof(struct host_interest_s,
					hi_clock_info));
			hif_diag_read_access(hif_hdl,
				flag2_targ_addr, &flag2_value);
			qdf_print("\n ====> FRAC Val %x Address %x", frac,
				  flag2_value);
			hif_diag_write_access(hif_hdl, flag2_value, frac);
			qdf_print("\n INT Val %x  Address %x",
				  intval, flag2_value + 4);
			hif_diag_write_access(hif_hdl,
					flag2_value + 4, intval);
		} else {
			qdf_print(KERN_INFO
				  "%s: no frac provided, skipping pre-configuring PLL",
				  __func__);
		}

		/* for 2.0 write 300 mhz into hi_desired_cpu_speed_hz */
		if ((target_type == TARGET_TYPE_AR900B)
			&& (tgt_info->target_revision == AR900B_REV_2)
			&& ar900b_20_targ_clk != -1) {
			uint32_t flag2_value = 0;
			uint32_t flag2_targ_addr;

			flag2_targ_addr
				= host_interest_item_address(target_type,
					offsetof(struct host_interest_s,
					hi_desired_cpu_speed_hz));
			hif_diag_read_access(hif_hdl, flag2_targ_addr,
							&flag2_value);
			qdf_print("\n ==> hi_desired_cpu_speed_hz Address %x",
				  flag2_value);
			hif_diag_write_access(hif_hdl, flag2_value,
				ar900b_20_targ_clk/*300000000u*/);
		} else if (target_type == TARGET_TYPE_QCA9888) {
			uint32_t flag2_targ_addr;

			if (200000000u != qca9888_20_targ_clk) {
				qca9888_20_targ_clk = 300000000u;
				/* Setting the target clock speed to 300 mhz */
			}

			flag2_targ_addr
				= host_interest_item_address(target_type,
					offsetof(struct host_interest_s,
					hi_desired_cpu_speed_hz));
			hif_diag_write_access(hif_hdl, flag2_targ_addr,
				qca9888_20_targ_clk);
		} else {
			qdf_print("%s: targ_clk is not provided, skipping pre-configuring PLL",
				  __func__);
		}
	} else {
		if (frac != -1 || intval != -1) {
			uint32_t flag2_value = 0;
			uint32_t flag2_targ_addr =
				host_interest_item_address(target_type,
					offsetof(struct host_interest_s,
							hi_clock_info));
			hif_diag_read_access(hif_hdl, flag2_targ_addr,
						&flag2_value);
			qdf_print("\n ====> FRAC Val %x Address %x", frac,
				  flag2_value);
			hif_diag_write_access(hif_hdl, flag2_value, frac);
			qdf_print("\n INT Val %x  Address %x", intval,
				  flag2_value + 4);
			hif_diag_write_access(hif_hdl, flag2_value + 4,
					      intval);
		}
	}
}

#else

static void hif_set_hia_extnd(struct hif_softc *scn)
{
}

#endif

/**
 * hif_set_hia() - fill out the host interest area
 * @scn: hif context
 *
 * This is replaced by hif_wlan_enable for integrated targets.
 * This fills out the host interest area.  The firmware will
 * process these memory addresses when it is first brought out
 * of reset.
 *
 * Return: 0 for success.
 */
static int hif_set_hia(struct hif_softc *scn)
{
	QDF_STATUS rv;
	uint32_t interconnect_targ_addr = 0;
	uint32_t pcie_state_targ_addr = 0;
	uint32_t pipe_cfg_targ_addr = 0;
	uint32_t svc_to_pipe_map = 0;
	uint32_t pcie_config_flags = 0;
	uint32_t flag2_value = 0;
	uint32_t flag2_targ_addr = 0;
#ifdef QCA_WIFI_3_0
	uint32_t host_interest_area = 0;
	uint8_t i;
#else
	uint32_t ealloc_value = 0;
	uint32_t ealloc_targ_addr = 0;
	uint8_t banks_switched = 1;
	uint32_t chip_id;
#endif
	uint32_t pipe_cfg_addr;
	struct hif_opaque_softc *hif_hdl = GET_HIF_OPAQUE_HDL(scn);
	struct hif_target_info *tgt_info = hif_get_target_info_handle(hif_hdl);
	uint32_t target_type = tgt_info->target_type;
	uint32_t target_ce_config_sz, target_service_to_ce_map_sz;
	static struct CE_pipe_config *target_ce_config;
	struct service_to_pipe *target_service_to_ce_map;

	HIF_TRACE("%s: E", __func__);

	hif_get_target_ce_config(scn,
				 &target_ce_config, &target_ce_config_sz,
				 &target_service_to_ce_map,
				 &target_service_to_ce_map_sz,
				 NULL, NULL);

	if (ADRASTEA_BU)
		return QDF_STATUS_SUCCESS;

#ifdef QCA_WIFI_3_0
	i = 0;
	while (i < HIF_HIA_MAX_POLL_LOOP) {
		host_interest_area = hif_read32_mb(scn, scn->mem +
						A_SOC_CORE_SCRATCH_0_ADDRESS);
		if ((host_interest_area & 0x01) == 0) {
			qdf_mdelay(HIF_HIA_POLLING_DELAY_MS);
			host_interest_area = 0;
			i++;
			if (i > HIF_HIA_MAX_POLL_LOOP && (i % 1000 == 0))
				HIF_ERROR("%s: poll timeout(%d)", __func__, i);
		} else {
			host_interest_area &= (~0x01);
			hif_write32_mb(scn, scn->mem + 0x113014, 0);
			break;
		}
	}

	if (i >= HIF_HIA_MAX_POLL_LOOP) {
		HIF_ERROR("%s: hia polling timeout", __func__);
		return -EIO;
	}

	if (host_interest_area == 0) {
		HIF_ERROR("%s: host_interest_area = 0", __func__);
		return -EIO;
	}

	interconnect_targ_addr = host_interest_area +
			offsetof(struct host_interest_area_t,
			hi_interconnect_state);

	flag2_targ_addr = host_interest_area +
			offsetof(struct host_interest_area_t, hi_option_flag2);

#else
	interconnect_targ_addr = hif_hia_item_address(target_type,
		offsetof(struct host_interest_s, hi_interconnect_state));
	ealloc_targ_addr = hif_hia_item_address(target_type,
		offsetof(struct host_interest_s, hi_early_alloc));
	flag2_targ_addr = hif_hia_item_address(target_type,
		offsetof(struct host_interest_s, hi_option_flag2));
#endif
	/* Supply Target-side CE configuration */
	rv = hif_diag_read_access(hif_hdl, interconnect_targ_addr,
			  &pcie_state_targ_addr);
	if (rv != QDF_STATUS_SUCCESS) {
		HIF_ERROR("%s: interconnect_targ_addr = 0x%0x, ret = %d",
			  __func__, interconnect_targ_addr, rv);
		goto done;
	}
	if (pcie_state_targ_addr == 0) {
		rv = QDF_STATUS_E_FAILURE;
		HIF_ERROR("%s: pcie state addr is 0", __func__);
		goto done;
	}
	pipe_cfg_addr = pcie_state_targ_addr +
			  offsetof(struct pcie_state_s,
			  pipe_cfg_addr);
	rv = hif_diag_read_access(hif_hdl,
			  pipe_cfg_addr,
			  &pipe_cfg_targ_addr);
	if (rv != QDF_STATUS_SUCCESS) {
		HIF_ERROR("%s: pipe_cfg_addr = 0x%0x, ret = %d",
			__func__, pipe_cfg_addr, rv);
		goto done;
	}
	if (pipe_cfg_targ_addr == 0) {
		rv = QDF_STATUS_E_FAILURE;
		HIF_ERROR("%s: pipe cfg addr is 0", __func__);
		goto done;
	}

	rv = hif_diag_write_mem(hif_hdl, pipe_cfg_targ_addr,
			(uint8_t *) target_ce_config,
			target_ce_config_sz);

	if (rv != QDF_STATUS_SUCCESS) {
		HIF_ERROR("%s: write pipe cfg (%d)", __func__, rv);
		goto done;
	}

	rv = hif_diag_read_access(hif_hdl,
			  pcie_state_targ_addr +
			  offsetof(struct pcie_state_s,
			   svc_to_pipe_map),
			  &svc_to_pipe_map);
	if (rv != QDF_STATUS_SUCCESS) {
		HIF_ERROR("%s: get svc/pipe map (%d)", __func__, rv);
		goto done;
	}
	if (svc_to_pipe_map == 0) {
		rv = QDF_STATUS_E_FAILURE;
		HIF_ERROR("%s: svc_to_pipe map is 0", __func__);
		goto done;
	}

	rv = hif_diag_write_mem(hif_hdl,
			svc_to_pipe_map,
			(uint8_t *) target_service_to_ce_map,
			target_service_to_ce_map_sz);
	if (rv != QDF_STATUS_SUCCESS) {
		HIF_ERROR("%s: write svc/pipe map (%d)", __func__, rv);
		goto done;
	}

	rv = hif_diag_read_access(hif_hdl,
			pcie_state_targ_addr +
			offsetof(struct pcie_state_s,
			config_flags),
			&pcie_config_flags);
	if (rv != QDF_STATUS_SUCCESS) {
		HIF_ERROR("%s: get pcie config_flags (%d)", __func__, rv);
		goto done;
	}
#if (CONFIG_PCIE_ENABLE_L1_CLOCK_GATE)
	pcie_config_flags |= PCIE_CONFIG_FLAG_ENABLE_L1;
#else
	pcie_config_flags &= ~PCIE_CONFIG_FLAG_ENABLE_L1;
#endif /* CONFIG_PCIE_ENABLE_L1_CLOCK_GATE */
	pcie_config_flags |= PCIE_CONFIG_FLAG_CLK_SWITCH_WAIT;
#if (CONFIG_PCIE_ENABLE_AXI_CLK_GATE)
	pcie_config_flags |= PCIE_CONFIG_FLAG_AXI_CLK_GATE;
#endif
	rv = hif_diag_write_mem(hif_hdl,
			pcie_state_targ_addr +
			offsetof(struct pcie_state_s,
			config_flags),
			(uint8_t *) &pcie_config_flags,
			sizeof(pcie_config_flags));
	if (rv != QDF_STATUS_SUCCESS) {
		HIF_ERROR("%s: write pcie config_flags (%d)", __func__, rv);
		goto done;
	}

#ifndef QCA_WIFI_3_0
	/* configure early allocation */
	ealloc_targ_addr = hif_hia_item_address(target_type,
						offsetof(
						struct host_interest_s,
						hi_early_alloc));

	rv = hif_diag_read_access(hif_hdl, ealloc_targ_addr,
			&ealloc_value);
	if (rv != QDF_STATUS_SUCCESS) {
		HIF_ERROR("%s: get early alloc val (%d)", __func__, rv);
		goto done;
	}

	/* 1 bank is switched to IRAM, except ROME 1.0 */
	ealloc_value |=
		((HI_EARLY_ALLOC_MAGIC << HI_EARLY_ALLOC_MAGIC_SHIFT) &
		 HI_EARLY_ALLOC_MAGIC_MASK);

	rv = hif_diag_read_access(hif_hdl,
			  CHIP_ID_ADDRESS |
			  RTC_SOC_BASE_ADDRESS, &chip_id);
	if (rv != QDF_STATUS_SUCCESS) {
		HIF_ERROR("%s: get chip id val (%d)", __func__, rv);
		goto done;
	}
	if (CHIP_ID_VERSION_GET(chip_id) == 0xD) {
		tgt_info->target_revision = CHIP_ID_REVISION_GET(chip_id);
		switch (CHIP_ID_REVISION_GET(chip_id)) {
		case 0x2:       /* ROME 1.3 */
			/* 2 banks are switched to IRAM */
			banks_switched = 2;
			break;
		case 0x4:       /* ROME 2.1 */
		case 0x5:       /* ROME 2.2 */
			banks_switched = 6;
			break;
		case 0x8:       /* ROME 3.0 */
		case 0x9:       /* ROME 3.1 */
		case 0xA:       /* ROME 3.2 */
			banks_switched = 9;
			break;
		case 0x0:       /* ROME 1.0 */
		case 0x1:       /* ROME 1.1 */
		default:
			/* 3 banks are switched to IRAM */
			banks_switched = 3;
			break;
		}
	}

	ealloc_value |=
		((banks_switched << HI_EARLY_ALLOC_IRAM_BANKS_SHIFT)
		 & HI_EARLY_ALLOC_IRAM_BANKS_MASK);

	rv = hif_diag_write_access(hif_hdl,
				ealloc_targ_addr,
				ealloc_value);
	if (rv != QDF_STATUS_SUCCESS) {
		HIF_ERROR("%s: set early alloc val (%d)", __func__, rv);
		goto done;
	}
#endif
	if ((target_type == TARGET_TYPE_AR900B)
			|| (target_type == TARGET_TYPE_QCA9984)
			|| (target_type == TARGET_TYPE_QCA9888)
			|| (target_type == TARGET_TYPE_AR9888)) {
		hif_set_hia_extnd(scn);
	}

	/* Tell Target to proceed with initialization */
	flag2_targ_addr = hif_hia_item_address(target_type,
						offsetof(
						struct host_interest_s,
						hi_option_flag2));

	rv = hif_diag_read_access(hif_hdl, flag2_targ_addr,
			  &flag2_value);
	if (rv != QDF_STATUS_SUCCESS) {
		HIF_ERROR("%s: get option val (%d)", __func__, rv);
		goto done;
	}

	flag2_value |= HI_OPTION_EARLY_CFG_DONE;
	rv = hif_diag_write_access(hif_hdl, flag2_targ_addr,
			   flag2_value);
	if (rv != QDF_STATUS_SUCCESS) {
		HIF_ERROR("%s: set option val (%d)", __func__, rv);
		goto done;
	}

	hif_wake_target_cpu(scn);

done:

	return rv;
}

/**
 * hif_bus_configure() - configure the pcie bus
 * @hif_sc: pointer to the hif context.
 *
 * return: 0 for success. nonzero for failure.
 */
int hif_pci_bus_configure(struct hif_softc *hif_sc)
{
	int status = 0;
	struct HIF_CE_state *hif_state = HIF_GET_CE_STATE(hif_sc);
	struct hif_opaque_softc *hif_osc = GET_HIF_OPAQUE_HDL(hif_sc);

	hif_ce_prepare_config(hif_sc);

	/* initialize sleep state adjust variables */
	hif_state->sleep_timer_init = true;
	hif_state->keep_awake_count = 0;
	hif_state->fake_sleep = false;
	hif_state->sleep_ticks = 0;

	qdf_timer_init(NULL, &hif_state->sleep_timer,
			       hif_sleep_entry, (void *)hif_state,
			       QDF_TIMER_TYPE_WAKE_APPS);
	hif_state->sleep_timer_init = true;

	status = hif_wlan_enable(hif_sc);
	if (status) {
		HIF_ERROR("%s: hif_wlan_enable error = %d",
			  __func__, status);
		goto timer_free;
	}

	A_TARGET_ACCESS_LIKELY(hif_sc);

	if ((CONFIG_ATH_PCIE_MAX_PERF ||
	     CONFIG_ATH_PCIE_AWAKE_WHILE_DRIVER_LOAD) &&
	    !ce_srng_based(hif_sc)) {
		/*
		 * prevent sleep for PCIE_AWAKE_WHILE_DRIVER_LOAD feature
		 * prevent sleep when we want to keep firmware always awake
		 * note: when we want to keep firmware always awake,
		 *       hif_target_sleep_state_adjust will point to a dummy
		 *       function, and hif_pci_target_sleep_state_adjust must
		 *       be called instead.
		 * note: bus type check is here because AHB bus is reusing
		 *       hif_pci_bus_configure code.
		 */
		if (hif_sc->bus_type == QDF_BUS_TYPE_PCI) {
			if (hif_pci_target_sleep_state_adjust(hif_sc,
					false, true) < 0) {
				status = -EACCES;
				goto disable_wlan;
			}
		}
	}

	/* todo: consider replacing this with an srng field */
	if (((hif_sc->target_info.target_type == TARGET_TYPE_QCA8074) ||
	     (hif_sc->target_info.target_type == TARGET_TYPE_QCA8074V2) ||
	     (hif_sc->target_info.target_type == TARGET_TYPE_QCA6018)) &&
	    (hif_sc->bus_type == QDF_BUS_TYPE_AHB)) {
		hif_sc->per_ce_irq = true;
	}

	status = hif_config_ce(hif_sc);
	if (status)
		goto disable_wlan;

	if (hif_needs_bmi(hif_osc)) {
		status = hif_set_hia(hif_sc);
		if (status)
			goto unconfig_ce;

		HIF_INFO_MED("%s: hif_set_hia done", __func__);

	}

	if (((hif_sc->target_info.target_type == TARGET_TYPE_QCA8074) ||
	     (hif_sc->target_info.target_type == TARGET_TYPE_QCA8074V2) ||
	     (hif_sc->target_info.target_type == TARGET_TYPE_QCA6018)) &&
	    (hif_sc->bus_type == QDF_BUS_TYPE_PCI))
		HIF_INFO_MED("%s: Skip irq config for PCI based 8074 target",
						__func__);
	else {
		status = hif_configure_irq(hif_sc);
		if (status < 0)
			goto unconfig_ce;
	}

	A_TARGET_ACCESS_UNLIKELY(hif_sc);

	return status;

unconfig_ce:
	hif_unconfig_ce(hif_sc);
disable_wlan:
	A_TARGET_ACCESS_UNLIKELY(hif_sc);
	hif_wlan_disable(hif_sc);

timer_free:
	qdf_timer_stop(&hif_state->sleep_timer);
	qdf_timer_free(&hif_state->sleep_timer);
	hif_state->sleep_timer_init = false;

	HIF_ERROR("%s: failed, status = %d", __func__, status);
	return status;
}

/**
 * hif_bus_close(): hif_bus_close
 *
 * Return: n/a
 */
void hif_pci_close(struct hif_softc *hif_sc)
{
	struct hif_pci_softc *hif_pci_sc = HIF_GET_PCI_SOFTC(hif_sc);

	hif_pm_runtime_close(hif_pci_sc);
	hif_ce_close(hif_sc);
}

#define BAR_NUM 0

static int hif_enable_pci_nopld(struct hif_pci_softc *sc,
				struct pci_dev *pdev,
				const struct pci_device_id *id)
{
	void __iomem *mem;
	int ret = 0;
	uint16_t device_id = 0;
	struct hif_softc *ol_sc = HIF_GET_SOFTC(sc);

	pci_read_config_word(pdev, PCI_DEVICE_ID, &device_id);
	if (device_id != id->device)  {
		HIF_ERROR(
		   "%s: dev id mismatch, config id = 0x%x, probing id = 0x%x",
		   __func__, device_id, id->device);
		/* pci link is down, so returing with error code */
		return -EIO;
	}

	/* FIXME: temp. commenting out assign_resource
	 * call for dev_attach to work on 2.6.38 kernel
	 */
#if (!defined(__LINUX_ARM_ARCH__))
	if (pci_assign_resource(pdev, BAR_NUM)) {
		HIF_ERROR("%s: pci_assign_resource error", __func__);
		return -EIO;
	}
#endif
	if (pci_enable_device(pdev)) {
		HIF_ERROR("%s: pci_enable_device error",
			   __func__);
		return -EIO;
	}

	/* Request MMIO resources */
	ret = pci_request_region(pdev, BAR_NUM, "ath");
	if (ret) {
		HIF_ERROR("%s: PCI MMIO reservation error", __func__);
		ret = -EIO;
		goto err_region;
	}

#ifdef CONFIG_ARM_LPAE
	/* if CONFIG_ARM_LPAE is enabled, we have to set 64 bits mask
	 * for 32 bits device also.
	 */
	ret =  pci_set_dma_mask(pdev, DMA_BIT_MASK(64));
	if (ret) {
		HIF_ERROR("%s: Cannot enable 64-bit pci DMA", __func__);
		goto err_dma;
	}
	ret = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(64));
	if (ret) {
		HIF_ERROR("%s: Cannot enable 64-bit DMA", __func__);
		goto err_dma;
	}
#else
	ret = pci_set_dma_mask(pdev, DMA_BIT_MASK(32));
	if (ret) {
		HIF_ERROR("%s: Cannot enable 32-bit pci DMA", __func__);
		goto err_dma;
	}
	ret = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(32));
	if (ret) {
		HIF_ERROR("%s: Cannot enable 32-bit consistent DMA!",
			   __func__);
		goto err_dma;
	}
#endif

	PCI_CFG_TO_DISABLE_L1SS_STATES(pdev, 0x188);

	/* Set bus master bit in PCI_COMMAND to enable DMA */
	pci_set_master(pdev);

	/* Arrange for access to Target SoC registers. */
	mem = pci_iomap(pdev, BAR_NUM, 0);
	if (!mem) {
		HIF_ERROR("%s: PCI iomap error", __func__);
		ret = -EIO;
		goto err_iomap;
	}

	HIF_INFO("*****BAR is %pK\n", (void *)mem);

	sc->mem = mem;

	/* Hawkeye emulation specific change */
	if ((device_id == RUMIM2M_DEVICE_ID_NODE0) ||
		(device_id == RUMIM2M_DEVICE_ID_NODE1) ||
		(device_id == RUMIM2M_DEVICE_ID_NODE2) ||
		(device_id == RUMIM2M_DEVICE_ID_NODE3) ||
		(device_id == RUMIM2M_DEVICE_ID_NODE4) ||
		(device_id == RUMIM2M_DEVICE_ID_NODE5)) {
		mem = mem + 0x0c000000;
		sc->mem = mem;
		HIF_INFO("%s: Changing PCI mem base to %pK\n",
			__func__, sc->mem);
	}

	sc->mem_len = pci_resource_len(pdev, BAR_NUM);
	ol_sc->mem = mem;
	ol_sc->mem_pa = pci_resource_start(pdev, BAR_NUM);
	sc->pci_enabled = true;
	return ret;

err_iomap:
	pci_clear_master(pdev);
err_dma:
	pci_release_region(pdev, BAR_NUM);
err_region:
	pci_disable_device(pdev);
	return ret;
}

static int hif_enable_pci_pld(struct hif_pci_softc *sc,
			      struct pci_dev *pdev,
			      const struct pci_device_id *id)
{
	PCI_CFG_TO_DISABLE_L1SS_STATES(pdev, 0x188);
	sc->pci_enabled = true;
	return 0;
}


static void hif_pci_deinit_nopld(struct hif_pci_softc *sc)
{
	pci_disable_msi(sc->pdev);
	pci_iounmap(sc->pdev, sc->mem);
	pci_clear_master(sc->pdev);
	pci_release_region(sc->pdev, BAR_NUM);
	pci_disable_device(sc->pdev);
}

static void hif_pci_deinit_pld(struct hif_pci_softc *sc) {}

static void hif_disable_pci(struct hif_pci_softc *sc)
{
	struct hif_softc *ol_sc = HIF_GET_SOFTC(sc);

	if (!ol_sc) {
		HIF_ERROR("%s: ol_sc = NULL", __func__);
		return;
	}
	hif_pci_device_reset(sc);
	sc->hif_pci_deinit(sc);

	sc->mem = NULL;
	ol_sc->mem = NULL;
}

static int hif_pci_probe_tgt_wakeup(struct hif_pci_softc *sc)
{
	int ret = 0;
	int targ_awake_limit = 500;
#ifndef QCA_WIFI_3_0
	uint32_t fw_indicator;
#endif
	struct hif_softc *scn = HIF_GET_SOFTC(sc);

	/*
	 * Verify that the Target was started cleanly.*
	 * The case where this is most likely is with an AUX-powered
	 * Target and a Host in WoW mode. If the Host crashes,
	 * loses power, or is restarted (without unloading the driver)
	 * then the Target is left (aux) powered and running.  On a
	 * subsequent driver load, the Target is in an unexpected state.
	 * We try to catch that here in order to reset the Target and
	 * retry the probe.
	 */
	hif_write32_mb(sc, sc->mem + PCIE_LOCAL_BASE_ADDRESS +
				  PCIE_SOC_WAKE_ADDRESS, PCIE_SOC_WAKE_V_MASK);
	while (!hif_targ_is_awake(scn, sc->mem)) {
		if (0 == targ_awake_limit) {
			HIF_ERROR("%s: target awake timeout", __func__);
			ret = -EAGAIN;
			goto end;
		}
		qdf_mdelay(1);
		targ_awake_limit--;
	}

#if PCIE_BAR0_READY_CHECKING
	{
		int wait_limit = 200;
		/* Synchronization point: wait the BAR0 is configured */
		while (wait_limit-- &&
			   !(hif_read32_mb(sc, c->mem +
					  PCIE_LOCAL_BASE_ADDRESS +
					  PCIE_SOC_RDY_STATUS_ADDRESS)
					  & PCIE_SOC_RDY_STATUS_BAR_MASK)) {
			qdf_mdelay(10);
		}
		if (wait_limit < 0) {
			/* AR6320v1 doesn't support checking of BAR0
			 * configuration, takes one sec to wait BAR0 ready
			 */
			HIF_INFO_MED("%s: AR6320v1 waits two sec for BAR0",
				    __func__);
		}
	}
#endif

#ifndef QCA_WIFI_3_0
	fw_indicator = hif_read32_mb(sc, sc->mem + FW_INDICATOR_ADDRESS);
	hif_write32_mb(sc, sc->mem + PCIE_LOCAL_BASE_ADDRESS +
				  PCIE_SOC_WAKE_ADDRESS, PCIE_SOC_WAKE_RESET);

	if (fw_indicator & FW_IND_INITIALIZED) {
		HIF_ERROR("%s: Target is in an unknown state. EAGAIN",
			   __func__);
		ret = -EAGAIN;
		goto end;
	}
#endif

end:
	return ret;
}

static int hif_pci_configure_legacy_irq(struct hif_pci_softc *sc)
{
	int ret = 0;
	struct hif_softc *scn = HIF_GET_SOFTC(sc);
	uint32_t target_type = scn->target_info.target_type;

	HIF_TRACE("%s: E", __func__);

	/* do notn support MSI or MSI IRQ failed */
	tasklet_init(&sc->intr_tq, wlan_tasklet, (unsigned long)sc);
	ret = request_irq(sc->pdev->irq,
			  hif_pci_legacy_ce_interrupt_handler, IRQF_SHARED,
			  "wlan_pci", sc);
	if (ret) {
		HIF_ERROR("%s: request_irq failed, ret = %d", __func__, ret);
		goto end;
	}
	scn->wake_irq = sc->pdev->irq;
	/* Use sc->irq instead of sc->pdev-irq
	 * platform_device pdev doesn't have an irq field
	 */
	sc->irq = sc->pdev->irq;
	/* Use Legacy PCI Interrupts */
	hif_write32_mb(sc, sc->mem + (SOC_CORE_BASE_ADDRESS |
		  PCIE_INTR_ENABLE_ADDRESS),
		  HOST_GROUP0_MASK);
	hif_read32_mb(sc, sc->mem + (SOC_CORE_BASE_ADDRESS |
			       PCIE_INTR_ENABLE_ADDRESS));
	hif_write32_mb(sc, sc->mem + PCIE_LOCAL_BASE_ADDRESS +
		      PCIE_SOC_WAKE_ADDRESS, PCIE_SOC_WAKE_RESET);

	if ((target_type == TARGET_TYPE_IPQ4019) ||
			(target_type == TARGET_TYPE_AR900B)  ||
			(target_type == TARGET_TYPE_QCA9984) ||
			(target_type == TARGET_TYPE_AR9888) ||
			(target_type == TARGET_TYPE_QCA9888) ||
			(target_type == TARGET_TYPE_AR6320V1) ||
			(target_type == TARGET_TYPE_AR6320V2) ||
			(target_type == TARGET_TYPE_AR6320V3)) {
		hif_write32_mb(scn, scn->mem + PCIE_LOCAL_BASE_ADDRESS +
				PCIE_SOC_WAKE_ADDRESS, PCIE_SOC_WAKE_V_MASK);
	}
end:
	QDF_TRACE(QDF_MODULE_ID_HIF, QDF_TRACE_LEVEL_ERROR,
			  "%s: X, ret = %d", __func__, ret);
	return ret;
}

static int hif_ce_srng_msi_free_irq(struct hif_softc *scn)
{
	int ret;
	int ce_id, irq;
	uint32_t msi_data_start;
	uint32_t msi_data_count;
	uint32_t msi_irq_start;
	struct HIF_CE_state *ce_sc = HIF_GET_CE_STATE(scn);
	struct CE_attr *host_ce_conf = ce_sc->host_ce_config;

	ret = pld_get_user_msi_assignment(scn->qdf_dev->dev, "CE",
					    &msi_data_count, &msi_data_start,
					    &msi_irq_start);
	if (ret)
		return ret;

	/* needs to match the ce_id -> irq data mapping
	 * used in the srng parameter configuration
	 */
	for (ce_id = 0; ce_id < scn->ce_count; ce_id++) {
		unsigned int msi_data;

		if (host_ce_conf[ce_id].flags & CE_ATTR_DISABLE_INTR)
			continue;

		if (!ce_sc->tasklets[ce_id].inited)
			continue;

		msi_data = (ce_id % msi_data_count) + msi_irq_start;
		irq = pld_get_msi_irq(scn->qdf_dev->dev, msi_data);

		hif_pci_ce_irq_remove_affinity_hint(irq);

		hif_debug("%s: (ce_id %d, msi_data %d, irq %d)", __func__,
			  ce_id, msi_data, irq);

		pfrm_free_irq(scn->qdf_dev->dev, irq, &ce_sc->tasklets[ce_id]);
	}

	return ret;
}

void hif_pci_deconfigure_grp_irq(struct hif_softc *scn)
{
	int i, j, irq;
	struct HIF_CE_state *hif_state = HIF_GET_CE_STATE(scn);
	struct hif_exec_context *hif_ext_group;

	for (i = 0; i < hif_state->hif_num_extgroup; i++) {
		hif_ext_group = hif_state->hif_ext_group[i];
		if (hif_ext_group->irq_requested) {
			hif_ext_group->irq_requested = false;
			for (j = 0; j < hif_ext_group->numirq; j++) {
				irq = hif_ext_group->os_irq[j];
				pfrm_free_irq(scn->qdf_dev->dev,
					      irq, hif_ext_group);
			}
			hif_ext_group->numirq = 0;
		}
	}
}

#ifdef HIF_BUS_LOG_INFO
bool hif_log_pcie_info(struct hif_softc *scn, uint8_t *data,
		       unsigned int *offset)
{
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(scn);
	struct hang_event_bus_info info = {0};
	size_t size;

	if (!sc) {
		hif_err("HIF Bus Context is Invalid");
		return false;
	}

	pfrm_read_config_word(sc->pdev, PCI_DEVICE_ID, &info.dev_id);

	size = sizeof(info);
	QDF_HANG_EVT_SET_HDR(&info.tlv_header, HANG_EVT_TAG_BUS_INFO,
			     size - QDF_HANG_EVENT_TLV_HDR_SIZE);

	if (*offset + size > QDF_WLAN_HANG_FW_OFFSET)
		return false;

	qdf_mem_copy(data + *offset, &info, size);
	*offset = *offset + size;

	if (info.dev_id == sc->devid)
		return false;

	qdf_recovery_reason_update(QCA_HANG_BUS_FAILURE);
	qdf_get_bus_reg_dump(scn->qdf_dev->dev, data,
			     (QDF_WLAN_HANG_FW_OFFSET - size));
	return true;
}
#endif

/**
 * hif_nointrs(): disable IRQ
 *
 * This function stops interrupt(s)
 *
 * @scn: struct hif_softc
 *
 * Return: none
 */
void hif_pci_nointrs(struct hif_softc *scn)
{
	int i, ret;
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(scn);
	struct HIF_CE_state *hif_state = HIF_GET_CE_STATE(scn);

	ce_unregister_irq(hif_state, CE_ALL_BITMAP);

	if (scn->request_irq_done == false)
		return;

	hif_pci_deconfigure_grp_irq(scn);

	ret = hif_ce_srng_msi_free_irq(scn);
	if (ret != -EINVAL) {
		/* ce irqs freed in hif_ce_srng_msi_free_irq */

		if (scn->wake_irq)
			pfrm_free_irq(scn->qdf_dev->dev, scn->wake_irq, scn);
		scn->wake_irq = 0;
	} else if (sc->num_msi_intrs > 0) {
		/* MSI interrupt(s) */
		for (i = 0; i < sc->num_msi_intrs; i++)
			free_irq(sc->irq + i, sc);
		sc->num_msi_intrs = 0;
	} else {
		/* Legacy PCI line interrupt
		 * Use sc->irq instead of sc->pdev-irq
		 * platform_device pdev doesn't have an irq field
		 */
		free_irq(sc->irq, sc);
	}
	scn->request_irq_done = false;
}

/**
 * hif_disable_bus(): hif_disable_bus
 *
 * This function disables the bus
 *
 * @bdev: bus dev
 *
 * Return: none
 */
void hif_pci_disable_bus(struct hif_softc *scn)
{
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(scn);
	struct pci_dev *pdev;
	void __iomem *mem;
	struct hif_target_info *tgt_info = &scn->target_info;

	/* Attach did not succeed, all resources have been
	 * freed in error handler
	 */
	if (!sc)
		return;

	pdev = sc->pdev;
	if (ADRASTEA_BU) {
		hif_vote_link_down(GET_HIF_OPAQUE_HDL(scn));

		hif_write32_mb(sc, sc->mem + PCIE_INTR_ENABLE_ADDRESS, 0);
		hif_write32_mb(sc, sc->mem + PCIE_INTR_CLR_ADDRESS,
			       HOST_GROUP0_MASK);
	}

#if defined(CPU_WARM_RESET_WAR)
	/* Currently CPU warm reset sequence is tested only for AR9888_REV2
	 * Need to enable for AR9888_REV1 once CPU warm reset sequence is
	 * verified for AR9888_REV1
	 */
	if ((tgt_info->target_version == AR9888_REV2_VERSION) ||
	    (tgt_info->target_version == AR9887_REV1_VERSION))
		hif_pci_device_warm_reset(sc);
	else
		hif_pci_device_reset(sc);
#else
	hif_pci_device_reset(sc);
#endif
	mem = (void __iomem *)sc->mem;
	if (mem) {
		hif_dump_pipe_debug_count(scn);
#ifdef CONFIG_ATH_PROCFS_DIAG_SUPPORT
		if (scn->athdiag_procfs_inited) {
			athdiag_procfs_remove();
			scn->athdiag_procfs_inited = false;
		}
#endif
		sc->hif_pci_deinit(sc);
		scn->mem = NULL;
	}
	HIF_INFO("%s: X", __func__);
}

#define OL_ATH_PCI_PM_CONTROL 0x44

#ifdef FEATURE_RUNTIME_PM
/**
 * hif_runtime_prevent_linkdown() - prevent or allow a runtime pm from occurring
 * @scn: hif context
 * @flag: prevent linkdown if true otherwise allow
 *
 * this api should only be called as part of bus prevent linkdown
 */
static void hif_runtime_prevent_linkdown(struct hif_softc *scn, bool flag)
{
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(scn);

	if (flag)
		qdf_runtime_pm_prevent_suspend(&sc->prevent_linkdown_lock);
	else
		qdf_runtime_pm_allow_suspend(&sc->prevent_linkdown_lock);
}
#else
static void hif_runtime_prevent_linkdown(struct hif_softc *scn, bool flag)
{
}
#endif

#if defined(CONFIG_PCI_MSM)
/**
 * hif_bus_prevent_linkdown(): allow or permit linkdown
 * @flag: true prevents linkdown, false allows
 *
 * Calls into the platform driver to vote against taking down the
 * pcie link.
 *
 * Return: n/a
 */
void hif_pci_prevent_linkdown(struct hif_softc *scn, bool flag)
{
	int errno;

	HIF_INFO("wlan: %s pcie power collapse", flag ? "disable" : "enable");
	hif_runtime_prevent_linkdown(scn, flag);

	errno = pld_wlan_pm_control(scn->qdf_dev->dev, flag);
	if (errno)
		HIF_ERROR("%s: Failed pld_wlan_pm_control; errno %d",
			  __func__, errno);
}
#else
void hif_pci_prevent_linkdown(struct hif_softc *scn, bool flag)
{
	HIF_INFO("wlan: %s pcie power collapse", (flag ? "disable" : "enable"));
	hif_runtime_prevent_linkdown(scn, flag);
}
#endif

/**
 * hif_pci_bus_suspend(): prepare hif for suspend
 *
 * Return: Errno
 */
int hif_pci_bus_suspend(struct hif_softc *scn)
{
	QDF_STATUS ret;

	hif_apps_irqs_disable(GET_HIF_OPAQUE_HDL(scn));

	ret = hif_try_complete_tasks(scn);
	if (QDF_IS_STATUS_ERROR(ret)) {
		hif_apps_irqs_enable(GET_HIF_OPAQUE_HDL(scn));
		return -EBUSY;
	}

	/* Stop the HIF Sleep Timer */
	hif_cancel_deferred_target_sleep(scn);

	return 0;
}

#ifdef PCI_LINK_STATUS_SANITY
/**
 * __hif_check_link_status() - API to check if PCIe link is active/not
 * @scn: HIF Context
 *
 * API reads the PCIe config space to verify if PCIe link training is
 * successful or not.
 *
 * Return: Success/Failure
 */
static int __hif_check_link_status(struct hif_softc *scn)
{
	uint16_t dev_id = 0;
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(scn);
	struct hif_driver_state_callbacks *cbk = hif_get_callbacks_handle(scn);

	if (!sc) {
		HIF_ERROR("%s: HIF Bus Context is Invalid", __func__);
		return -EINVAL;
	}

	pfrm_read_config_word(sc->pdev, PCI_DEVICE_ID, &dev_id);

	if (dev_id == sc->devid)
		return 0;

	HIF_ERROR("%s: Invalid PCIe Config Space; PCIe link down dev_id:0x%04x",
	       __func__, dev_id);

	scn->recovery = true;

	if (cbk && cbk->set_recovery_in_progress)
		cbk->set_recovery_in_progress(cbk->context, true);
	else
		HIF_ERROR("%s: Driver Global Recovery is not set", __func__);

	pld_is_pci_link_down(sc->dev);
	return -EACCES;
}
#else
static inline int __hif_check_link_status(struct hif_softc *scn)
{
	return 0;
}
#endif


/**
 * hif_pci_bus_resume(): prepare hif for resume
 *
 * Return: Errno
 */
int hif_pci_bus_resume(struct hif_softc *scn)
{
	int errno;

	errno = __hif_check_link_status(scn);
	if (errno)
		return errno;

	hif_apps_irqs_enable(GET_HIF_OPAQUE_HDL(scn));

	return 0;
}

/**
 * hif_pci_bus_suspend_noirq() - ensure there are no pending transactions
 * @scn: hif context
 *
 * Ensure that if we received the wakeup message before the irq
 * was disabled that the message is pocessed before suspending.
 *
 * Return: -EBUSY if we fail to flush the tasklets.
 */
int hif_pci_bus_suspend_noirq(struct hif_softc *scn)
{
	if (hif_can_suspend_link(GET_HIF_OPAQUE_HDL(scn)))
		qdf_atomic_set(&scn->link_suspended, 1);

	hif_apps_wake_irq_enable(GET_HIF_OPAQUE_HDL(scn));

	return 0;
}

/**
 * hif_pci_bus_resume_noirq() - ensure there are no pending transactions
 * @scn: hif context
 *
 * Ensure that if we received the wakeup message before the irq
 * was disabled that the message is pocessed before suspending.
 *
 * Return: -EBUSY if we fail to flush the tasklets.
 */
int hif_pci_bus_resume_noirq(struct hif_softc *scn)
{
	hif_apps_wake_irq_disable(GET_HIF_OPAQUE_HDL(scn));

	if (hif_can_suspend_link(GET_HIF_OPAQUE_HDL(scn)))
		qdf_atomic_set(&scn->link_suspended, 0);

	return 0;
}

#ifdef FEATURE_RUNTIME_PM
/**
 * __hif_runtime_pm_set_state(): utility function
 * @state: state to set
 *
 * indexes into the runtime pm state and sets it.
 */
static void __hif_runtime_pm_set_state(struct hif_softc *scn,
				enum hif_pm_runtime_state state)
{
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(scn);

	if (!sc) {
		HIF_ERROR("%s: HIF_CTX not initialized",
		       __func__);
		return;
	}

	qdf_atomic_set(&sc->pm_state, state);
}

/**
 * hif_runtime_pm_set_state_on():  adjust runtime pm state
 *
 * Notify hif that a the runtime pm state should be on
 */
static void hif_runtime_pm_set_state_on(struct hif_softc *scn)
{
	__hif_runtime_pm_set_state(scn, HIF_PM_RUNTIME_STATE_ON);
}

/**
 * hif_runtime_pm_set_state_resuming(): adjust runtime pm state
 *
 * Notify hif that a runtime pm resuming has started
 */
static void hif_runtime_pm_set_state_resuming(struct hif_softc *scn)
{
	__hif_runtime_pm_set_state(scn, HIF_PM_RUNTIME_STATE_RESUMING);
}

/**
 * hif_runtime_pm_set_state_suspending(): adjust runtime pm state
 *
 * Notify hif that a runtime pm suspend has started
 */
static void hif_runtime_pm_set_state_suspending(struct hif_softc *scn)
{
	__hif_runtime_pm_set_state(scn, HIF_PM_RUNTIME_STATE_SUSPENDING);
}

/**
 * hif_runtime_pm_set_state_suspended():  adjust runtime pm state
 *
 * Notify hif that a runtime suspend attempt has been completed successfully
 */
static void hif_runtime_pm_set_state_suspended(struct hif_softc *scn)
{
	__hif_runtime_pm_set_state(scn, HIF_PM_RUNTIME_STATE_SUSPENDED);
}

/**
 * hif_log_runtime_suspend_success() - log a successful runtime suspend
 */
static void hif_log_runtime_suspend_success(struct hif_softc *hif_ctx)
{
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(hif_ctx);

	if (!sc)
		return;

	sc->pm_stats.suspended++;
	sc->pm_stats.suspend_jiffies = jiffies;
}

/**
 * hif_log_runtime_suspend_failure() - log a failed runtime suspend
 *
 * log a failed runtime suspend
 * mark last busy to prevent immediate runtime suspend
 */
static void hif_log_runtime_suspend_failure(void *hif_ctx)
{
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(hif_ctx);

	if (!sc)
		return;

	sc->pm_stats.suspend_err++;
}

/**
 * hif_log_runtime_resume_success() - log a successful runtime resume
 *
 * log a successful runtime resume
 * mark last busy to prevent immediate runtime suspend
 */
static void hif_log_runtime_resume_success(void *hif_ctx)
{
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(hif_ctx);

	if (!sc)
		return;

	sc->pm_stats.resumed++;
}

/**
 * hif_process_runtime_suspend_failure() - bookkeeping of suspend failure
 *
 * Record the failure.
 * mark last busy to delay a retry.
 * adjust the runtime_pm state.
 */
void hif_process_runtime_suspend_failure(struct hif_opaque_softc *hif_ctx)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	hif_log_runtime_suspend_failure(hif_ctx);
	hif_pm_runtime_mark_last_busy(hif_ctx);
	hif_runtime_pm_set_state_on(scn);
}

/**
 * hif_pre_runtime_suspend() - bookkeeping before beginning runtime suspend
 *
 * Makes sure that the pci link will be taken down by the suspend opperation.
 * If the hif layer is configured to leave the bus on, runtime suspend will
 * not save any power.
 *
 * Set the runtime suspend state to in progress.
 *
 * return -EINVAL if the bus won't go down.  otherwise return 0
 */
int hif_pre_runtime_suspend(struct hif_opaque_softc *hif_ctx)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	if (!hif_can_suspend_link(hif_ctx)) {
		HIF_ERROR("Runtime PM not supported for link up suspend");
		return -EINVAL;
	}

	hif_runtime_pm_set_state_suspending(scn);
	return 0;
}

/**
 * hif_process_runtime_suspend_success() - bookkeeping of suspend success
 *
 * Record the success.
 * adjust the runtime_pm state
 */
void hif_process_runtime_suspend_success(struct hif_opaque_softc *hif_ctx)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	hif_runtime_pm_set_state_suspended(scn);
	hif_log_runtime_suspend_success(scn);
}

/**
 * hif_pre_runtime_resume() - bookkeeping before beginning runtime resume
 *
 * update the runtime pm state.
 */
void hif_pre_runtime_resume(struct hif_opaque_softc *hif_ctx)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	hif_pm_runtime_set_monitor_wake_intr(hif_ctx, 0);
	hif_runtime_pm_set_state_resuming(scn);
}

/**
 * hif_process_runtime_resume_success() - bookkeeping after a runtime resume
 *
 * record the success.
 * adjust the runtime_pm state
 */
void hif_process_runtime_resume_success(struct hif_opaque_softc *hif_ctx)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	hif_log_runtime_resume_success(hif_ctx);
	hif_pm_runtime_mark_last_busy(hif_ctx);
	hif_runtime_pm_set_state_on(scn);
}

/**
 * hif_runtime_suspend() - do the bus suspend part of a runtime suspend
 *
 * Return: 0 for success and non-zero error code for failure
 */
int hif_runtime_suspend(struct hif_opaque_softc *hif_ctx)
{
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(hif_ctx);
	int errno;

	errno = hif_bus_suspend(hif_ctx);
	if (errno) {
		HIF_ERROR("%s: failed bus suspend: %d", __func__, errno);
		return errno;
	}

	hif_pm_runtime_set_monitor_wake_intr(hif_ctx, 1);

	errno = hif_bus_suspend_noirq(hif_ctx);
	if (errno) {
		HIF_ERROR("%s: failed bus suspend noirq: %d", __func__, errno);
		hif_pm_runtime_set_monitor_wake_intr(hif_ctx, 0);
		goto bus_resume;
	}

	qdf_atomic_set(&sc->pm_dp_rx_busy, 0);

	return 0;

bus_resume:
	QDF_BUG(!hif_bus_resume(hif_ctx));

	return errno;
}

/**
 * hif_fastpath_resume() - resume fastpath for runtimepm
 *
 * ensure that the fastpath write index register is up to date
 * since runtime pm may cause ce_send_fast to skip the register
 * write.
 *
 * fastpath only applicable to legacy copy engine
 */
void hif_fastpath_resume(struct hif_opaque_softc *hif_ctx)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);
	struct CE_state *ce_state;

	if (!scn)
		return;

	if (scn->fastpath_mode_on) {
		if (Q_TARGET_ACCESS_BEGIN(scn) < 0)
			return;

		ce_state = scn->ce_id_to_state[CE_HTT_H2T_MSG];
		qdf_spin_lock_bh(&ce_state->ce_index_lock);

		/*war_ce_src_ring_write_idx_set */
		CE_SRC_RING_WRITE_IDX_SET(scn, ce_state->ctrl_addr,
				ce_state->src_ring->write_index);
		qdf_spin_unlock_bh(&ce_state->ce_index_lock);
		Q_TARGET_ACCESS_END(scn);
	}
}

/**
 * hif_runtime_resume() - do the bus resume part of a runtime resume
 *
 *  Return: 0 for success and non-zero error code for failure
 */
int hif_runtime_resume(struct hif_opaque_softc *hif_ctx)
{
	int errno;

	QDF_BUG(!hif_bus_resume_noirq(hif_ctx));
	errno = hif_bus_resume(hif_ctx);
	if (errno)
		HIF_ERROR("%s: failed runtime resume: %d", __func__, errno);

	return errno;
}
#endif /* #ifdef FEATURE_RUNTIME_PM */

#if CONFIG_PCIE_64BIT_MSI
static void hif_free_msi_ctx(struct hif_softc *scn)
{
	struct hif_pci_softc *sc = scn->hif_sc;
	struct hif_msi_info *info = &sc->msi_info;
	struct device *dev = scn->qdf_dev->dev;

	OS_FREE_CONSISTENT(dev, 4, info->magic, info->magic_dma,
			   OS_GET_DMA_MEM_CONTEXT(scn, dmacontext));
	info->magic = NULL;
	info->magic_dma = 0;
}
#else
static void hif_free_msi_ctx(struct hif_softc *scn)
{
}
#endif

void hif_pci_disable_isr(struct hif_softc *scn)
{
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(scn);

	hif_exec_kill(&scn->osc);
	hif_nointrs(scn);
	hif_free_msi_ctx(scn);
	/* Cancel the pending tasklet */
	ce_tasklet_kill(scn);
	tasklet_kill(&sc->intr_tq);
	qdf_atomic_set(&scn->active_tasklet_cnt, 0);
	qdf_atomic_set(&scn->active_grp_tasklet_cnt, 0);
}

/* Function to reset SoC */
void hif_pci_reset_soc(struct hif_softc *hif_sc)
{
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(hif_sc);
	struct hif_opaque_softc *ol_sc = GET_HIF_OPAQUE_HDL(hif_sc);
	struct hif_target_info *tgt_info = hif_get_target_info_handle(ol_sc);

#if defined(CPU_WARM_RESET_WAR)
	/* Currently CPU warm reset sequence is tested only for AR9888_REV2
	 * Need to enable for AR9888_REV1 once CPU warm reset sequence is
	 * verified for AR9888_REV1
	 */
	if (tgt_info->target_version == AR9888_REV2_VERSION)
		hif_pci_device_warm_reset(sc);
	else
		hif_pci_device_reset(sc);
#else
	hif_pci_device_reset(sc);
#endif
}

#ifdef CONFIG_PCI_MSM
static inline void hif_msm_pcie_debug_info(struct hif_pci_softc *sc)
{
	msm_pcie_debug_info(sc->pdev, 13, 1, 0, 0, 0);
	msm_pcie_debug_info(sc->pdev, 13, 2, 0, 0, 0);
}
#else
static inline void hif_msm_pcie_debug_info(struct hif_pci_softc *sc) {};
#endif

/**
 * hif_log_soc_wakeup_timeout() - API to log PCIe and SOC Info
 * @sc: HIF PCIe Context
 *
 * API to log PCIe Config space and SOC info when SOC wakeup timeout happens
 *
 * Return: Failure to caller
 */
static int hif_log_soc_wakeup_timeout(struct hif_pci_softc *sc)
{
	uint16_t val = 0;
	uint32_t bar = 0;
	struct hif_opaque_softc *hif_hdl = GET_HIF_OPAQUE_HDL(sc);
	struct hif_softc *scn = HIF_GET_SOFTC(sc);
	struct HIF_CE_state *hif_state = HIF_GET_CE_STATE(sc);
	struct hif_config_info *cfg = hif_get_ini_handle(hif_hdl);
	struct hif_driver_state_callbacks *cbk = hif_get_callbacks_handle(scn);
	A_target_id_t pci_addr = scn->mem;

	HIF_ERROR("%s: keep_awake_count = %d",
			__func__, hif_state->keep_awake_count);

	pfrm_read_config_word(sc->pdev, PCI_VENDOR_ID, &val);

	HIF_ERROR("%s: PCI Vendor ID = 0x%04x", __func__, val);

	pfrm_read_config_word(sc->pdev, PCI_DEVICE_ID, &val);

	HIF_ERROR("%s: PCI Device ID = 0x%04x", __func__, val);

	pfrm_read_config_word(sc->pdev, PCI_COMMAND, &val);

	HIF_ERROR("%s: PCI Command = 0x%04x", __func__, val);

	pfrm_read_config_word(sc->pdev, PCI_STATUS, &val);

	HIF_ERROR("%s: PCI Status = 0x%04x", __func__, val);

	pfrm_read_config_dword(sc->pdev, PCI_BASE_ADDRESS_0, &bar);

	HIF_ERROR("%s: PCI BAR 0 = 0x%08x", __func__, bar);

	HIF_ERROR("%s: SOC_WAKE_ADDR 0%08x", __func__,
			hif_read32_mb(scn, pci_addr + PCIE_LOCAL_BASE_ADDRESS +
						PCIE_SOC_WAKE_ADDRESS));

	HIF_ERROR("%s: RTC_STATE_ADDR 0x%08x", __func__,
			hif_read32_mb(scn, pci_addr + PCIE_LOCAL_BASE_ADDRESS +
							RTC_STATE_ADDRESS));

	HIF_ERROR("%s:error, wakeup target", __func__);
	hif_msm_pcie_debug_info(sc);

	if (!cfg->enable_self_recovery)
		QDF_BUG(0);

	scn->recovery = true;

	if (cbk->set_recovery_in_progress)
		cbk->set_recovery_in_progress(cbk->context, true);

	pld_is_pci_link_down(sc->dev);
	return -EACCES;
}

/*
 * For now, we use simple on-demand sleep/wake.
 * Some possible improvements:
 *  -Use the Host-destined A_INUM_PCIE_AWAKE interrupt rather than spin/delay
 *   (or perhaps spin/delay for a short while, then convert to sleep/interrupt)
 *   Careful, though, these functions may be used by
 *  interrupt handlers ("atomic")
 *  -Don't use host_reg_table for this code; instead use values directly
 *  -Use a separate timer to track activity and allow Target to sleep only
 *   if it hasn't done anything for a while; may even want to delay some
 *   processing for a short while in order to "batch" (e.g.) transmit
 *   requests with completion processing into "windows of up time".  Costs
 *   some performance, but improves power utilization.
 *  -On some platforms, it might be possible to eliminate explicit
 *   sleep/wakeup. Instead, take a chance that each access works OK. If not,
 *   recover from the failure by forcing the Target awake.
 *  -Change keep_awake_count to an atomic_t in order to avoid spin lock
 *   overhead in some cases. Perhaps this makes more sense when
 *   CONFIG_ATH_PCIE_ACCESS_LIKELY is used and less sense when LIKELY is
 *   disabled.
 *  -It is possible to compile this code out and simply force the Target
 *   to remain awake.  That would yield optimal performance at the cost of
 *   increased power. See CONFIG_ATH_PCIE_MAX_PERF.
 *
 * Note: parameter wait_for_it has meaning only when waking (when sleep_ok==0).
 */
/**
 * hif_target_sleep_state_adjust() - on-demand sleep/wake
 * @scn: hif_softc pointer.
 * @sleep_ok: bool
 * @wait_for_it: bool
 *
 * Output the pipe error counts of each pipe to log file
 *
 * Return: int
 */
int hif_pci_target_sleep_state_adjust(struct hif_softc *scn,
			      bool sleep_ok, bool wait_for_it)
{
	struct HIF_CE_state *hif_state = HIF_GET_CE_STATE(scn);
	A_target_id_t pci_addr = scn->mem;
	static int max_delay;
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(scn);
	static int debug;
	if (scn->recovery)
		return -EACCES;

	if (qdf_atomic_read(&scn->link_suspended)) {
		HIF_ERROR("%s:invalid access, PCIe link is down", __func__);
		debug = true;
		QDF_ASSERT(0);
		return -EACCES;
	}

	if (debug) {
		wait_for_it = true;
		HIF_ERROR("%s: doing debug for invalid access, PCIe link is suspended",
				__func__);
		QDF_ASSERT(0);
	}

	if (sleep_ok) {
		qdf_spin_lock_irqsave(&hif_state->keep_awake_lock);
		hif_state->keep_awake_count--;
		if (hif_state->keep_awake_count == 0) {
			/* Allow sleep */
			hif_state->verified_awake = false;
			hif_state->sleep_ticks = qdf_system_ticks();
		}
		if (hif_state->fake_sleep == false) {
			/* Set the Fake Sleep */
			hif_state->fake_sleep = true;

			/* Start the Sleep Timer */
			qdf_timer_stop(&hif_state->sleep_timer);
			qdf_timer_start(&hif_state->sleep_timer,
				HIF_SLEEP_INACTIVITY_TIMER_PERIOD_MS);
		}
		qdf_spin_unlock_irqrestore(&hif_state->keep_awake_lock);
	} else {
		qdf_spin_lock_irqsave(&hif_state->keep_awake_lock);

		if (hif_state->fake_sleep) {
			hif_state->verified_awake = true;
		} else {
			if (hif_state->keep_awake_count == 0) {
				/* Force AWAKE */
				hif_write32_mb(sc, pci_addr +
					      PCIE_LOCAL_BASE_ADDRESS +
					      PCIE_SOC_WAKE_ADDRESS,
					      PCIE_SOC_WAKE_V_MASK);
			}
		}
		hif_state->keep_awake_count++;
		qdf_spin_unlock_irqrestore(&hif_state->keep_awake_lock);

		if (wait_for_it && !hif_state->verified_awake) {
#define PCIE_SLEEP_ADJUST_TIMEOUT 8000  /* 8Ms */
			int tot_delay = 0;
			int curr_delay = 5;

			for (;; ) {
				if (hif_targ_is_awake(scn, pci_addr)) {
					hif_state->verified_awake = true;
					break;
				}
				if (!hif_pci_targ_is_present(scn, pci_addr))
					break;
				if (tot_delay > PCIE_SLEEP_ADJUST_TIMEOUT)
					return hif_log_soc_wakeup_timeout(sc);

				OS_DELAY(curr_delay);
				tot_delay += curr_delay;

				if (curr_delay < 50)
					curr_delay += 5;
			}

			/*
			 * NB: If Target has to come out of Deep Sleep,
			 * this may take a few Msecs. Typically, though
			 * this delay should be <30us.
			 */
			if (tot_delay > max_delay)
				max_delay = tot_delay;
		}
	}

	if (debug && hif_state->verified_awake) {
		debug = 0;
		HIF_ERROR("%s: INTR_ENABLE_REG = 0x%08x, INTR_CAUSE_REG = 0x%08x, CPU_INTR_REG = 0x%08x, INTR_CLR_REG = 0x%08x, CE_INTERRUPT_SUMMARY_REG = 0x%08x",
			__func__,
			hif_read32_mb(sc, sc->mem + SOC_CORE_BASE_ADDRESS +
				PCIE_INTR_ENABLE_ADDRESS),
			hif_read32_mb(sc, sc->mem + SOC_CORE_BASE_ADDRESS +
				PCIE_INTR_CAUSE_ADDRESS),
			hif_read32_mb(sc, sc->mem + SOC_CORE_BASE_ADDRESS +
				CPU_INTR_ADDRESS),
			hif_read32_mb(sc, sc->mem + SOC_CORE_BASE_ADDRESS +
				PCIE_INTR_CLR_ADDRESS),
			hif_read32_mb(sc, sc->mem + CE_WRAPPER_BASE_ADDRESS +
				CE_WRAPPER_INTERRUPT_SUMMARY_ADDRESS));
	}

	return 0;
}

#ifdef CONFIG_ATH_PCIE_ACCESS_DEBUG
uint32_t hif_target_read_checked(struct hif_softc *scn, uint32_t offset)
{
	uint32_t value;
	void *addr;

	addr = scn->mem + offset;
	value = hif_read32_mb(scn, addr);

	{
		unsigned long irq_flags;
		int idx = pcie_access_log_seqnum % PCIE_ACCESS_LOG_NUM;

		spin_lock_irqsave(&pcie_access_log_lock, irq_flags);
		pcie_access_log[idx].seqnum = pcie_access_log_seqnum;
		pcie_access_log[idx].is_write = false;
		pcie_access_log[idx].addr = addr;
		pcie_access_log[idx].value = value;
		pcie_access_log_seqnum++;
		spin_unlock_irqrestore(&pcie_access_log_lock, irq_flags);
	}

	return value;
}

void
hif_target_write_checked(struct hif_softc *scn, uint32_t offset, uint32_t value)
{
	void *addr;

	addr = scn->mem + (offset);
	hif_write32_mb(scn, addr, value);

	{
		unsigned long irq_flags;
		int idx = pcie_access_log_seqnum % PCIE_ACCESS_LOG_NUM;

		spin_lock_irqsave(&pcie_access_log_lock, irq_flags);
		pcie_access_log[idx].seqnum = pcie_access_log_seqnum;
		pcie_access_log[idx].is_write = true;
		pcie_access_log[idx].addr = addr;
		pcie_access_log[idx].value = value;
		pcie_access_log_seqnum++;
		spin_unlock_irqrestore(&pcie_access_log_lock, irq_flags);
	}
}

/**
 * hif_target_dump_access_log() - dump access log
 *
 * dump access log
 *
 * Return: n/a
 */
void hif_target_dump_access_log(void)
{
	int idx, len, start_idx, cur_idx;
	unsigned long irq_flags;

	spin_lock_irqsave(&pcie_access_log_lock, irq_flags);
	if (pcie_access_log_seqnum > PCIE_ACCESS_LOG_NUM) {
		len = PCIE_ACCESS_LOG_NUM;
		start_idx = pcie_access_log_seqnum % PCIE_ACCESS_LOG_NUM;
	} else {
		len = pcie_access_log_seqnum;
		start_idx = 0;
	}

	for (idx = 0; idx < len; idx++) {
		cur_idx = (start_idx + idx) % PCIE_ACCESS_LOG_NUM;
		HIF_ERROR("%s: idx:%d sn:%u wr:%d addr:%pK val:%u.",
		       __func__, idx,
		       pcie_access_log[cur_idx].seqnum,
		       pcie_access_log[cur_idx].is_write,
		       pcie_access_log[cur_idx].addr,
		       pcie_access_log[cur_idx].value);
	}

	pcie_access_log_seqnum = 0;
	spin_unlock_irqrestore(&pcie_access_log_lock, irq_flags);
}
#endif

#ifndef HIF_AHB
int hif_ahb_configure_legacy_irq(struct hif_pci_softc *sc)
{
	QDF_BUG(0);
	return -EINVAL;
}

int hif_ahb_configure_irq(struct hif_pci_softc *sc)
{
	QDF_BUG(0);
	return -EINVAL;
}
#endif

static irqreturn_t hif_ce_interrupt_handler(int irq, void *context)
{
	struct ce_tasklet_entry *tasklet_entry = context;
	return ce_dispatch_interrupt(tasklet_entry->ce_id, tasklet_entry);
}
extern const char *ce_name[];

static int hif_ce_msi_map_ce_to_irq(struct hif_softc *scn, int ce_id)
{
	struct hif_pci_softc *pci_scn = HIF_GET_PCI_SOFTC(scn);

	return pci_scn->ce_msi_irq_num[ce_id];
}

/* hif_srng_msi_irq_disable() - disable the irq for msi
 * @hif_sc: hif context
 * @ce_id: which ce to disable copy complete interrupts for
 *
 * since MSI interrupts are not level based, the system can function
 * without disabling these interrupts.  Interrupt mitigation can be
 * added here for better system performance.
 */
static void hif_ce_srng_msi_irq_disable(struct hif_softc *hif_sc, int ce_id)
{
	pfrm_disable_irq_nosync(hif_sc->qdf_dev->dev,
				hif_ce_msi_map_ce_to_irq(hif_sc, ce_id));
}

static void hif_ce_srng_msi_irq_enable(struct hif_softc *hif_sc, int ce_id)
{
	if (__hif_check_link_status(hif_sc))
		return;

	pfrm_enable_irq(hif_sc->qdf_dev->dev,
			hif_ce_msi_map_ce_to_irq(hif_sc, ce_id));
}

static void hif_ce_legacy_msi_irq_disable(struct hif_softc *hif_sc, int ce_id)
{
	disable_irq_nosync(hif_ce_msi_map_ce_to_irq(hif_sc, ce_id));
}

static void hif_ce_legacy_msi_irq_enable(struct hif_softc *hif_sc, int ce_id)
{
	enable_irq(hif_ce_msi_map_ce_to_irq(hif_sc, ce_id));
}

static int hif_ce_msi_configure_irq(struct hif_softc *scn)
{
	int ret;
	int ce_id, irq;
	uint32_t msi_data_start;
	uint32_t msi_data_count;
	uint32_t msi_irq_start;
	struct HIF_CE_state *ce_sc = HIF_GET_CE_STATE(scn);
	struct hif_pci_softc *pci_sc = HIF_GET_PCI_SOFTC(scn);
	struct CE_attr *host_ce_conf = ce_sc->host_ce_config;

	if (!scn->disable_wake_irq) {
		/* do wake irq assignment */
		ret = pld_get_user_msi_assignment(scn->qdf_dev->dev, "WAKE",
						  &msi_data_count,
						  &msi_data_start,
						  &msi_irq_start);
		if (ret)
			return ret;

		scn->wake_irq = pld_get_msi_irq(scn->qdf_dev->dev,
						msi_irq_start);

		ret = pfrm_request_irq(scn->qdf_dev->dev, scn->wake_irq,
				       hif_wake_interrupt_handler,
				       IRQF_NO_SUSPEND, "wlan_wake_irq", scn);

		if (ret)
			return ret;
	}

	/* do ce irq assignments */
	ret = pld_get_user_msi_assignment(scn->qdf_dev->dev, "CE",
					    &msi_data_count, &msi_data_start,
					    &msi_irq_start);
	if (ret)
		goto free_wake_irq;

	if (ce_srng_based(scn)) {
		scn->bus_ops.hif_irq_disable = &hif_ce_srng_msi_irq_disable;
		scn->bus_ops.hif_irq_enable = &hif_ce_srng_msi_irq_enable;
	} else {
		scn->bus_ops.hif_irq_disable = &hif_ce_legacy_msi_irq_disable;
		scn->bus_ops.hif_irq_enable = &hif_ce_legacy_msi_irq_enable;
	}

	scn->bus_ops.hif_map_ce_to_irq = &hif_ce_msi_map_ce_to_irq;

	/* needs to match the ce_id -> irq data mapping
	 * used in the srng parameter configuration
	 */
	for (ce_id = 0; ce_id < scn->ce_count; ce_id++) {
		unsigned int msi_data = (ce_id % msi_data_count) +
			msi_irq_start;
		if (host_ce_conf[ce_id].flags & CE_ATTR_DISABLE_INTR)
			continue;
		irq = pld_get_msi_irq(scn->qdf_dev->dev, msi_data);
		HIF_DBG("%s: (ce_id %d, msi_data %d, irq %d tasklet %pK)",
			 __func__, ce_id, msi_data, irq,
			 &ce_sc->tasklets[ce_id]);

		/* implies the ce is also initialized */
		if (!ce_sc->tasklets[ce_id].inited)
			continue;

		pci_sc->ce_msi_irq_num[ce_id] = irq;
		ret = pfrm_request_irq(scn->qdf_dev->dev,
				       irq, hif_ce_interrupt_handler,
				       IRQF_SHARED,
				       ce_name[ce_id],
				       &ce_sc->tasklets[ce_id]);
		if (ret)
			goto free_irq;
	}

	return ret;

free_irq:
	/* the request_irq for the last ce_id failed so skip it. */
	while (ce_id > 0 && ce_id < scn->ce_count) {
		unsigned int msi_data;

		ce_id--;
		msi_data = (ce_id % msi_data_count) + msi_irq_start;
		irq = pld_get_msi_irq(scn->qdf_dev->dev, msi_data);
		pfrm_free_irq(scn->qdf_dev->dev,
			      irq, &ce_sc->tasklets[ce_id]);
	}

free_wake_irq:
	if (!scn->disable_wake_irq) {
		pfrm_free_irq(scn->qdf_dev->dev,
			      scn->wake_irq, scn->qdf_dev->dev);
		scn->wake_irq = 0;
	}

	return ret;
}

static void hif_exec_grp_irq_disable(struct hif_exec_context *hif_ext_group)
{
	int i;
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ext_group->hif);

	for (i = 0; i < hif_ext_group->numirq; i++)
		pfrm_disable_irq_nosync(scn->qdf_dev->dev,
					hif_ext_group->os_irq[i]);
}

static void hif_exec_grp_irq_enable(struct hif_exec_context *hif_ext_group)
{
	int i;
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ext_group->hif);

	for (i = 0; i < hif_ext_group->numirq; i++)
		pfrm_enable_irq(scn->qdf_dev->dev, hif_ext_group->os_irq[i]);
}

/**
 * hif_pci_get_irq_name() - get irqname
 * This function gives irqnumber to irqname
 * mapping.
 *
 * @irq_no: irq number
 *
 * Return: irq name
 */
const char *hif_pci_get_irq_name(int irq_no)
{
	return "pci-dummy";
}

#ifdef HIF_CPU_PERF_AFFINE_MASK
/**
 * hif_pci_irq_set_affinity_hint() - API to set IRQ affinity
 * @hif_ext_group: hif_ext_group to extract the irq info
 *
 * This function will set the WLAN DP IRQ affinity to the gold
 * cores only for defconfig builds
 *
 * @hif_ext_group: hif_ext_group to extract the irq info
 *
 * Return: none
 */
void hif_pci_irq_set_affinity_hint(
	struct hif_exec_context *hif_ext_group)
{
	int i, ret;
	unsigned int cpus;
	bool mask_set = false;

	for (i = 0; i < hif_ext_group->numirq; i++)
		qdf_cpumask_clear(&hif_ext_group->new_cpu_mask[i]);

	for (i = 0; i < hif_ext_group->numirq; i++) {
		qdf_for_each_online_cpu(cpus) {
			if (qdf_topology_physical_package_id(cpus) ==
				CPU_CLUSTER_TYPE_PERF) {
				qdf_cpumask_set_cpu(cpus,
						    &hif_ext_group->
						    new_cpu_mask[i]);
				mask_set = true;
			}
		}
	}
	for (i = 0; i < hif_ext_group->numirq; i++) {
		if (mask_set) {
			qdf_dev_modify_irq_status(hif_ext_group->os_irq[i],
						  IRQ_NO_BALANCING, 0);
			ret = qdf_dev_set_irq_affinity(hif_ext_group->os_irq[i],
						       (struct qdf_cpu_mask *)
						       &hif_ext_group->
						       new_cpu_mask[i]);
			qdf_dev_modify_irq_status(hif_ext_group->os_irq[i],
						  0, IRQ_NO_BALANCING);
			if (ret)
				qdf_err("Set affinity %*pbl fails for IRQ %d ",
					qdf_cpumask_pr_args(&hif_ext_group->
							    new_cpu_mask[i]),
					hif_ext_group->os_irq[i]);
			else
				qdf_debug("Set affinity %*pbl for IRQ: %d",
					  qdf_cpumask_pr_args(&hif_ext_group->
							      new_cpu_mask[i]),
					  hif_ext_group->os_irq[i]);
		} else {
			qdf_err("Offline CPU: Set affinity fails for IRQ: %d",
				hif_ext_group->os_irq[i]);
		}
	}
}

void hif_pci_ce_irq_set_affinity_hint(
	struct hif_softc *scn)
{
	int ret;
	unsigned int cpus;
	struct HIF_CE_state *ce_sc = HIF_GET_CE_STATE(scn);
	struct hif_pci_softc *pci_sc = HIF_GET_PCI_SOFTC(scn);
	struct CE_attr *host_ce_conf;
	int ce_id;
	qdf_cpu_mask ce_cpu_mask;

	host_ce_conf = ce_sc->host_ce_config;
	qdf_cpumask_clear(&ce_cpu_mask);

	qdf_for_each_online_cpu(cpus) {
		if (qdf_topology_physical_package_id(cpus) ==
			CPU_CLUSTER_TYPE_PERF) {
			qdf_cpumask_set_cpu(cpus,
					    &ce_cpu_mask);
		} else {
			hif_err_rl("Unable to set cpu mask for offline CPU %d"
				   , cpus);
		}
	}
	if (qdf_cpumask_empty(&ce_cpu_mask)) {
		hif_err_rl("Empty cpu_mask, unable to set CE IRQ affinity");
		return;
	}
	for (ce_id = 0; ce_id < scn->ce_count; ce_id++) {
		if (host_ce_conf[ce_id].flags & CE_ATTR_DISABLE_INTR)
			continue;
		qdf_cpumask_clear(&pci_sc->ce_irq_cpu_mask[ce_id]);
		qdf_cpumask_copy(&pci_sc->ce_irq_cpu_mask[ce_id],
				 &ce_cpu_mask);
		qdf_dev_modify_irq_status(pci_sc->ce_msi_irq_num[ce_id],
					  IRQ_NO_BALANCING, 0);
		ret =
		qdf_dev_set_irq_affinity(pci_sc->ce_msi_irq_num[ce_id],
					 (struct qdf_cpu_mask *)
					 &pci_sc->ce_irq_cpu_mask[ce_id]);
		qdf_dev_modify_irq_status(pci_sc->ce_msi_irq_num[ce_id],
					  0, IRQ_NO_BALANCING);
		if (ret)
			hif_err_rl("Set affinity %*pbl fails for CE IRQ %d",
				   qdf_cpumask_pr_args(
					&pci_sc->ce_irq_cpu_mask[ce_id]),
				   pci_sc->ce_msi_irq_num[ce_id]);
		else
			hif_debug_rl("Set affinity %*pbl for CE IRQ: %d",
				     qdf_cpumask_pr_args(
					&pci_sc->ce_irq_cpu_mask[ce_id]),
				     pci_sc->ce_msi_irq_num[ce_id]);
	}
}
#endif /* #ifdef HIF_CPU_PERF_AFFINE_MASK */

void hif_pci_config_irq_affinity(struct hif_softc *scn)
{
	int i;
	struct HIF_CE_state *hif_state = HIF_GET_CE_STATE(scn);
	struct hif_exec_context *hif_ext_group;

	hif_core_ctl_set_boost(true);
	/* Set IRQ affinity for WLAN DP interrupts*/
	for (i = 0; i < hif_state->hif_num_extgroup; i++) {
		hif_ext_group = hif_state->hif_ext_group[i];
		hif_pci_irq_set_affinity_hint(hif_ext_group);
	}
	/* Set IRQ affinity for CE interrupts*/
	hif_pci_ce_irq_set_affinity_hint(scn);
}

int hif_pci_configure_grp_irq(struct hif_softc *scn,
			      struct hif_exec_context *hif_ext_group)
{
	int ret = 0;
	int irq = 0;
	int j;

	hif_ext_group->irq_enable = &hif_exec_grp_irq_enable;
	hif_ext_group->irq_disable = &hif_exec_grp_irq_disable;
	hif_ext_group->irq_name = &hif_pci_get_irq_name;
	hif_ext_group->work_complete = &hif_dummy_grp_done;

	for (j = 0; j < hif_ext_group->numirq; j++) {
		irq = hif_ext_group->irq[j];

		hif_debug("request_irq = %d for grp %d",
			  irq, hif_ext_group->grp_id);
		ret = pfrm_request_irq(
				scn->qdf_dev->dev, irq,
				hif_ext_group_interrupt_handler,
				IRQF_SHARED | IRQF_NO_SUSPEND,
				"wlan_EXT_GRP",
				hif_ext_group);
		if (ret) {
			HIF_ERROR("%s: request_irq failed ret = %d",
				  __func__, ret);
			return -EFAULT;
		}
		hif_ext_group->os_irq[j] = irq;
	}
	hif_ext_group->irq_requested = true;
	return 0;
}

/**
 * hif_configure_irq() - configure interrupt
 *
 * This function configures interrupt(s)
 *
 * @sc: PCIe control struct
 * @hif_hdl: struct HIF_CE_state
 *
 * Return: 0 - for success
 */
int hif_configure_irq(struct hif_softc *scn)
{
	int ret = 0;
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(scn);

	HIF_TRACE("%s: E", __func__);

	if (hif_is_polled_mode_enabled(GET_HIF_OPAQUE_HDL(scn))) {
		scn->request_irq_done = false;
		return 0;
	}

	hif_init_reschedule_tasklet_work(sc);

	ret = hif_ce_msi_configure_irq(scn);
	if (ret == 0) {
		goto end;
	}

	switch (scn->target_info.target_type) {
	case TARGET_TYPE_IPQ4019:
		ret = hif_ahb_configure_legacy_irq(sc);
		break;
	case TARGET_TYPE_QCA8074:
	case TARGET_TYPE_QCA8074V2:
	case TARGET_TYPE_QCA6018:
		ret = hif_ahb_configure_irq(sc);
		break;
	default:
		ret = hif_pci_configure_legacy_irq(sc);
		break;
	}
	if (ret < 0) {
		HIF_ERROR("%s: hif_pci_configure_legacy_irq error = %d",
			__func__, ret);
		return ret;
	}
end:
	scn->request_irq_done = true;
	return 0;
}

/**
 * hif_trigger_timer_irq() : Triggers interrupt on LF_Timer 0
 * @scn: hif control structure
 *
 * Sets IRQ bit in LF Timer Status Address to awake peregrine/swift
 * stuck at a polling loop in pcie_address_config in FW
 *
 * Return: none
 */
static void hif_trigger_timer_irq(struct hif_softc *scn)
{
	int tmp;
	/* Trigger IRQ on Peregrine/Swift by setting
	 * IRQ Bit of LF_TIMER 0
	 */
	tmp = hif_read32_mb(scn, scn->mem + (RTC_SOC_BASE_ADDRESS +
						SOC_LF_TIMER_STATUS0_ADDRESS));
	/* Set Raw IRQ Bit */
	tmp |= 1;
	/* SOC_LF_TIMER_STATUS0 */
	hif_write32_mb(scn, scn->mem + (RTC_SOC_BASE_ADDRESS +
		       SOC_LF_TIMER_STATUS0_ADDRESS), tmp);
}

/**
 * hif_target_sync() : ensure the target is ready
 * @scn: hif control structure
 *
 * Informs fw that we plan to use legacy interupts so that
 * it can begin booting. Ensures that the fw finishes booting
 * before continuing. Should be called before trying to write
 * to the targets other registers for the first time.
 *
 * Return: none
 */
static void hif_target_sync(struct hif_softc *scn)
{
	hif_write32_mb(scn, scn->mem + (SOC_CORE_BASE_ADDRESS |
			    PCIE_INTR_ENABLE_ADDRESS),
			    PCIE_INTR_FIRMWARE_MASK | PCIE_INTR_CE_MASK_ALL);
	/* read to flush pcie write */
	(void)hif_read32_mb(scn, scn->mem + (SOC_CORE_BASE_ADDRESS |
			PCIE_INTR_ENABLE_ADDRESS));

	hif_write32_mb(scn, scn->mem + PCIE_LOCAL_BASE_ADDRESS +
			PCIE_SOC_WAKE_ADDRESS,
			PCIE_SOC_WAKE_V_MASK);
	while (!hif_targ_is_awake(scn, scn->mem))
		;

	if (HAS_FW_INDICATOR) {
		int wait_limit = 500;
		int fw_ind = 0;
		int retry_count = 0;
		uint32_t target_type = scn->target_info.target_type;
fw_retry:
		HIF_TRACE("%s: Loop checking FW signal", __func__);
		while (1) {
			fw_ind = hif_read32_mb(scn, scn->mem +
					FW_INDICATOR_ADDRESS);
			if (fw_ind & FW_IND_INITIALIZED)
				break;
			if (wait_limit-- < 0)
				break;
			hif_write32_mb(scn, scn->mem + (SOC_CORE_BASE_ADDRESS |
			    PCIE_INTR_ENABLE_ADDRESS),
			    PCIE_INTR_FIRMWARE_MASK | PCIE_INTR_CE_MASK_ALL);
			    /* read to flush pcie write */
			(void)hif_read32_mb(scn, scn->mem +
			    (SOC_CORE_BASE_ADDRESS | PCIE_INTR_ENABLE_ADDRESS));

			qdf_mdelay(10);
		}
		if (wait_limit < 0) {
			if (target_type == TARGET_TYPE_AR9888 &&
			    retry_count++ < 2) {
				hif_trigger_timer_irq(scn);
				wait_limit = 500;
				goto fw_retry;
			}
			HIF_TRACE("%s: FW signal timed out",
					__func__);
			qdf_assert_always(0);
		} else {
			HIF_TRACE("%s: Got FW signal, retries = %x",
					__func__, 500-wait_limit);
		}
	}
	hif_write32_mb(scn, scn->mem + PCIE_LOCAL_BASE_ADDRESS +
			PCIE_SOC_WAKE_ADDRESS, PCIE_SOC_WAKE_RESET);
}

static void hif_pci_get_soc_info_pld(struct hif_pci_softc *sc,
				     struct device *dev)
{
	struct pld_soc_info info;
	struct hif_softc *scn = HIF_GET_SOFTC(sc);

	pld_get_soc_info(dev, &info);
	sc->mem = info.v_addr;
	sc->ce_sc.ol_sc.mem    = info.v_addr;
	sc->ce_sc.ol_sc.mem_pa = info.p_addr;
	scn->target_info.target_version = info.soc_id;
	scn->target_info.target_revision = 0;
}

static void hif_pci_get_soc_info_nopld(struct hif_pci_softc *sc,
				       struct device *dev)
{}

static bool hif_is_pld_based_target(struct hif_pci_softc *sc,
				    int device_id)
{
	if (!pld_have_platform_driver_support(sc->dev))
		return false;

	switch (device_id) {
	case QCA6290_DEVICE_ID:
	case QCN9000_DEVICE_ID:
	case QCA6290_EMULATION_DEVICE_ID:
	case QCA6390_DEVICE_ID:
	case QCA6490_DEVICE_ID:
	case AR6320_DEVICE_ID:
	case QCN7605_DEVICE_ID:
		return true;
	}
	return false;
}

static void hif_pci_init_deinit_ops_attach(struct hif_pci_softc *sc,
					   int device_id)
{
	if (hif_is_pld_based_target(sc, device_id)) {
		sc->hif_enable_pci = hif_enable_pci_pld;
		sc->hif_pci_deinit = hif_pci_deinit_pld;
		sc->hif_pci_get_soc_info = hif_pci_get_soc_info_pld;
	} else {
		sc->hif_enable_pci = hif_enable_pci_nopld;
		sc->hif_pci_deinit = hif_pci_deinit_nopld;
		sc->hif_pci_get_soc_info = hif_pci_get_soc_info_nopld;
	}
}

#ifdef HIF_REG_WINDOW_SUPPORT
static void hif_pci_init_reg_windowing_support(struct hif_pci_softc *sc,
					       u32 target_type)
{
	switch (target_type) {
	case TARGET_TYPE_QCN7605:
		sc->use_register_windowing = true;
		qdf_spinlock_create(&sc->register_access_lock);
		sc->register_window = 0;
		break;
	default:
		sc->use_register_windowing = false;
	}
}
#else
static void hif_pci_init_reg_windowing_support(struct hif_pci_softc *sc,
					       u32 target_type)
{
	sc->use_register_windowing = false;
}
#endif

/**
 * hif_enable_bus(): enable bus
 *
 * This function enables the bus
 *
 * @ol_sc: soft_sc struct
 * @dev: device pointer
 * @bdev: bus dev pointer
 * bid: bus id pointer
 * type: enum hif_enable_type such as HIF_ENABLE_TYPE_PROBE
 * Return: QDF_STATUS
 */
QDF_STATUS hif_pci_enable_bus(struct hif_softc *ol_sc,
			  struct device *dev, void *bdev,
			  const struct hif_bus_id *bid,
			  enum hif_enable_type type)
{
	int ret = 0;
	uint32_t hif_type;
	uint32_t target_type = TARGET_TYPE_UNKNOWN;
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(ol_sc);
	struct hif_opaque_softc *hif_hdl = GET_HIF_OPAQUE_HDL(ol_sc);
	uint16_t revision_id = 0;
	int probe_again = 0;
	struct pci_dev *pdev = bdev;
	const struct pci_device_id *id = (const struct pci_device_id *)bid;
	struct hif_target_info *tgt_info;

	if (!ol_sc) {
		HIF_ERROR("%s: hif_ctx is NULL", __func__);
		return QDF_STATUS_E_NOMEM;
	}
	/* Following print is used by various tools to identify
	 * WLAN SOC (e.g. crash dump analysis and reporting tool).
	 */
	HIF_TRACE("%s: con_mode = 0x%x, WLAN_SOC_device_id = 0x%x",
		  __func__, hif_get_conparam(ol_sc), id->device);

	sc->pdev = pdev;
	sc->dev = &pdev->dev;
	sc->devid = id->device;
	sc->cacheline_sz = dma_get_cache_alignment();
	tgt_info = hif_get_target_info_handle(hif_hdl);
	hif_pci_init_deinit_ops_attach(sc, id->device);
	sc->hif_pci_get_soc_info(sc, dev);
again:
	ret = sc->hif_enable_pci(sc, pdev, id);
	if (ret < 0) {
		HIF_ERROR("%s: ERROR - hif_enable_pci error = %d",
		       __func__, ret);
		goto err_enable_pci;
	}
	HIF_TRACE("%s: hif_enable_pci done", __func__);

	/* Temporary FIX: disable ASPM on peregrine.
	 * Will be removed after the OTP is programmed
	 */
	hif_disable_power_gating(hif_hdl);

	device_disable_async_suspend(&pdev->dev);
	pfrm_read_config_word(pdev, 0x08, &revision_id);

	ret = hif_get_device_type(id->device, revision_id,
						&hif_type, &target_type);
	if (ret < 0) {
		HIF_ERROR("%s: invalid device id/revision_id", __func__);
		goto err_tgtstate;
	}
	HIF_TRACE("%s: hif_type = 0x%x, target_type = 0x%x",
		  __func__, hif_type, target_type);

	hif_register_tbl_attach(ol_sc, hif_type);
	hif_target_register_tbl_attach(ol_sc, target_type);

	hif_pci_init_reg_windowing_support(sc, target_type);

	tgt_info->target_type = target_type;

	if (ce_srng_based(ol_sc)) {
		HIF_TRACE("%s:Skip tgt_wake up for srng devices\n", __func__);
	} else {
		ret = hif_pci_probe_tgt_wakeup(sc);
		if (ret < 0) {
			HIF_ERROR("%s: ERROR - hif_pci_prob_wakeup error = %d",
					__func__, ret);
			if (ret == -EAGAIN)
				probe_again++;
			goto err_tgtstate;
		}
		HIF_TRACE("%s: hif_pci_probe_tgt_wakeup done", __func__);
	}

	if (!ol_sc->mem_pa) {
		HIF_ERROR("%s: ERROR - BAR0 uninitialized", __func__);
		ret = -EIO;
		goto err_tgtstate;
	}

	if (!ce_srng_based(ol_sc)) {
		hif_target_sync(ol_sc);

		if (ADRASTEA_BU)
			hif_vote_link_up(hif_hdl);
	}

	return 0;

err_tgtstate:
	hif_disable_pci(sc);
	sc->pci_enabled = false;
	HIF_ERROR("%s: error, hif_disable_pci done", __func__);
	return QDF_STATUS_E_ABORTED;

err_enable_pci:
	if (probe_again && (probe_again <= ATH_PCI_PROBE_RETRY_MAX)) {
		int delay_time;

		HIF_INFO("%s: pci reprobe", __func__);
		/* 10, 40, 90, 100, 100, ... */
		delay_time = max(100, 10 * (probe_again * probe_again));
		qdf_mdelay(delay_time);
		goto again;
	}
	return ret;
}

/**
 * hif_pci_irq_enable() - ce_irq_enable
 * @scn: hif_softc
 * @ce_id: ce_id
 *
 * Return: void
 */
void hif_pci_irq_enable(struct hif_softc *scn, int ce_id)
{
	uint32_t tmp = 1 << ce_id;
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(scn);

	qdf_spin_lock_irqsave(&sc->irq_lock);
	scn->ce_irq_summary &= ~tmp;
	if (scn->ce_irq_summary == 0) {
		/* Enable Legacy PCI line interrupts */
		if (LEGACY_INTERRUPTS(sc) &&
			(scn->target_status != TARGET_STATUS_RESET) &&
			(!qdf_atomic_read(&scn->link_suspended))) {

			hif_write32_mb(scn, scn->mem +
				(SOC_CORE_BASE_ADDRESS |
				PCIE_INTR_ENABLE_ADDRESS),
				HOST_GROUP0_MASK);

			hif_read32_mb(scn, scn->mem +
					(SOC_CORE_BASE_ADDRESS |
					PCIE_INTR_ENABLE_ADDRESS));
		}
	}
	if (scn->hif_init_done == true)
		Q_TARGET_ACCESS_END(scn);
	qdf_spin_unlock_irqrestore(&sc->irq_lock);

	/* check for missed firmware crash */
	hif_fw_interrupt_handler(0, scn);
}

/**
 * hif_pci_irq_disable() - ce_irq_disable
 * @scn: hif_softc
 * @ce_id: ce_id
 *
 * only applicable to legacy copy engine...
 *
 * Return: void
 */
void hif_pci_irq_disable(struct hif_softc *scn, int ce_id)
{
	/* For Rome only need to wake up target */
	/* target access is maintained until interrupts are re-enabled */
	Q_TARGET_ACCESS_BEGIN(scn);
}

#ifdef FEATURE_RUNTIME_PM
/**
 * hif_pm_stats_runtime_get_record() - record runtime get statistics
 * @sc: hif pci context
 * @rtpm_dbgid: debug id to trace who use it
 *
 *
 * Return: void
 */
static void hif_pm_stats_runtime_get_record(struct hif_pci_softc *sc,
					    wlan_rtpm_dbgid rtpm_dbgid)
{
	if (rtpm_dbgid >= RTPM_ID_MAX) {
		QDF_BUG(0);
		return;
	}
	qdf_atomic_inc(&sc->pm_stats.runtime_get);
	qdf_atomic_inc(&sc->pm_stats.runtime_get_dbgid[rtpm_dbgid]);
	sc->pm_stats.runtime_get_timestamp_dbgid[rtpm_dbgid] =
		qdf_get_log_timestamp();
}

/**
 * hif_pm_stats_runtime_put_record() - record runtime put statistics
 * @sc: hif pci context
 * @rtpm_dbgid: dbg_id to trace who use it
 *
 *
 * Return: void
 */
static void hif_pm_stats_runtime_put_record(struct hif_pci_softc *sc,
					    wlan_rtpm_dbgid rtpm_dbgid)
{
	if (rtpm_dbgid >= RTPM_ID_MAX) {
		QDF_BUG(0);
		return;
	}

	if (atomic_read(&sc->dev->power.usage_count) <= 0) {
		QDF_BUG(0);
		return;
	}

	qdf_atomic_inc(&sc->pm_stats.runtime_put);
	qdf_atomic_inc(&sc->pm_stats.runtime_put_dbgid[rtpm_dbgid]);
	sc->pm_stats.runtime_put_timestamp_dbgid[rtpm_dbgid] =
		qdf_get_log_timestamp();
}

/**
 * hif_pm_runtime_get_sync() - do a get operation with sync resume
 * @hif_ctx: pointer of HIF context
 * @rtpm_dbgid: dbgid to trace who use it
 *
 * A get operation will prevent a runtime suspend until a corresponding
 * put is done. Unlike hif_pm_runtime_get(), this API will do a sync
 * resume instead of requesting a resume if it is runtime PM suspended
 * so it can only be called in non-atomic context.
 *
 * Return: 0 if it is runtime PM resumed otherwise an error code.
 */
int hif_pm_runtime_get_sync(struct hif_opaque_softc *hif_ctx,
			    wlan_rtpm_dbgid rtpm_dbgid)
{
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(hif_ctx);
	int pm_state;
	int ret;

	if (!sc)
		return -EINVAL;

	if (!hif_pci_pm_runtime_enabled(sc))
		return 0;

	pm_state = qdf_atomic_read(&sc->pm_state);
	if (pm_state == HIF_PM_RUNTIME_STATE_SUSPENDED ||
	    pm_state == HIF_PM_RUNTIME_STATE_SUSPENDING)
		hif_info_high("Runtime PM resume is requested by %ps",
			      (void *)_RET_IP_);

	hif_pm_stats_runtime_get_record(sc, rtpm_dbgid);
	ret = pm_runtime_get_sync(sc->dev);

	/* Get can return 1 if the device is already active, just return
	 * success in that case.
	 */
	if (ret > 0)
		ret = 0;

	if (ret) {
		sc->pm_stats.runtime_get_err++;
		hif_err("Runtime PM Get Sync error in pm_state: %d, ret: %d",
			qdf_atomic_read(&sc->pm_state), ret);
		hif_pm_runtime_put(hif_ctx, rtpm_dbgid);
	}

	return ret;
}

/**
 * hif_pm_runtime_put_sync_suspend() - do a put operation with sync suspend
 * @hif_ctx: pointer of HIF context
 * @rtpm_dbgid: dbgid to trace who use it
 *
 * This API will do a runtime put operation followed by a sync suspend if usage
 * count is 0 so it can only be called in non-atomic context.
 *
 * Return: 0 for success otherwise an error code
 */
int hif_pm_runtime_put_sync_suspend(struct hif_opaque_softc *hif_ctx,
				    wlan_rtpm_dbgid rtpm_dbgid)
{
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(hif_ctx);
	int usage_count, pm_state;
	char *err = NULL;

	if (!sc)
		return -EINVAL;

	if (!hif_pci_pm_runtime_enabled(sc))
		return 0;

	usage_count = atomic_read(&sc->dev->power.usage_count);
	if (usage_count == 1) {
		pm_state = qdf_atomic_read(&sc->pm_state);
		if (pm_state == HIF_PM_RUNTIME_STATE_NONE)
			err = "Ignore unexpected Put as runtime PM is disabled";
	} else if (usage_count == 0) {
		err = "Put without a Get Operation";
	}

	if (err) {
		hif_pci_runtime_pm_warn(sc, err);
		return -EINVAL;
	}

	hif_pm_stats_runtime_put_record(sc, rtpm_dbgid);
	return pm_runtime_put_sync_suspend(sc->dev);
}

int hif_pm_runtime_request_resume(struct hif_opaque_softc *hif_ctx)
{
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(hif_ctx);
	int pm_state;

	if (!sc)
		return -EINVAL;

	if (!hif_pci_pm_runtime_enabled(sc))
		return 0;

	pm_state = qdf_atomic_read(&sc->pm_state);
	if (pm_state == HIF_PM_RUNTIME_STATE_SUSPENDED ||
	    pm_state == HIF_PM_RUNTIME_STATE_SUSPENDING)
		HIF_INFO("Runtime PM resume is requested by %ps",
			 (void *)_RET_IP_);

	sc->pm_stats.request_resume++;
	sc->pm_stats.last_resume_caller = (void *)_RET_IP_;

	return hif_pm_request_resume(sc->dev);
}

void hif_pm_runtime_mark_last_busy(struct hif_opaque_softc *hif_ctx)
{
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(hif_ctx);

	if (!sc)
		return;

	sc->pm_stats.last_busy_marker = (void *)_RET_IP_;
	sc->pm_stats.last_busy_timestamp = qdf_get_log_timestamp_usecs();

	return pm_runtime_mark_last_busy(sc->dev);
}

void hif_pm_runtime_get_noresume(struct hif_opaque_softc *hif_ctx,
				 wlan_rtpm_dbgid rtpm_dbgid)
{
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(hif_ctx);

	if (!sc)
		return;

	if (!hif_pci_pm_runtime_enabled(sc))
		return;

	hif_pm_stats_runtime_get_record(sc, rtpm_dbgid);
	pm_runtime_get_noresume(sc->dev);
}

/**
 * hif_pm_runtime_get() - do a get opperation on the device
 * @hif_ctx: pointer of HIF context
 * @rtpm_dbgid: dbgid to trace who use it
 *
 * A get opperation will prevent a runtime suspend until a
 * corresponding put is done.  This api should be used when sending
 * data.
 *
 * CONTRARY TO THE REGULAR RUNTIME PM, WHEN THE BUS IS SUSPENDED,
 * THIS API WILL ONLY REQUEST THE RESUME AND NOT TO A GET!!!
 *
 * return: success if the bus is up and a get has been issued
 *   otherwise an error code.
 */
int hif_pm_runtime_get(struct hif_opaque_softc *hif_ctx,
		       wlan_rtpm_dbgid rtpm_dbgid)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(hif_ctx);
	int ret;
	int pm_state;

	if (!scn) {
		hif_err("Could not do runtime get, scn is null");
		return -EFAULT;
	}

	if (!hif_pci_pm_runtime_enabled(sc))
		return 0;

	pm_state = qdf_atomic_read(&sc->pm_state);

	if (pm_state  == HIF_PM_RUNTIME_STATE_ON ||
			pm_state == HIF_PM_RUNTIME_STATE_NONE) {
		hif_pm_stats_runtime_get_record(sc, rtpm_dbgid);
		ret = __hif_pm_runtime_get(sc->dev);

		/* Get can return 1 if the device is already active, just return
		 * success in that case
		 */
		if (ret > 0)
			ret = 0;

		if (ret)
			hif_pm_runtime_put(hif_ctx, rtpm_dbgid);

		if (ret && ret != -EINPROGRESS) {
			sc->pm_stats.runtime_get_err++;
			hif_err("Runtime Get PM Error in pm_state:%d ret: %d",
				qdf_atomic_read(&sc->pm_state), ret);
		}

		return ret;
	}

	if (pm_state == HIF_PM_RUNTIME_STATE_SUSPENDED ||
	    pm_state == HIF_PM_RUNTIME_STATE_SUSPENDING) {
		hif_info_high("Runtime PM resume is requested by %ps",
			      (void *)_RET_IP_);
		ret = -EAGAIN;
	} else {
		ret = -EBUSY;
	}

	sc->pm_stats.request_resume++;
	sc->pm_stats.last_resume_caller = (void *)_RET_IP_;
	hif_pm_request_resume(sc->dev);

	return ret;
}

/**
 * hif_pm_runtime_put() - do a put operation on the device
 * @hif_ctx: pointer of HIF context
 * @rtpm_dbgid: dbgid to trace who use it
 *
 * A put operation will allow a runtime suspend after a corresponding
 * get was done.  This api should be used when sending data.
 *
 * This api will return a failure if runtime pm is stopped
 * This api will return failure if it would decrement the usage count below 0.
 *
 * return: QDF_STATUS_SUCCESS if the put is performed
 */
int hif_pm_runtime_put(struct hif_opaque_softc *hif_ctx,
		       wlan_rtpm_dbgid rtpm_dbgid)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(hif_ctx);
	int pm_state, usage_count;
	char *error = NULL;

	if (!scn) {
		HIF_ERROR("%s: Could not do runtime put, scn is null",
				__func__);
		return -EFAULT;
	}

	if (!hif_pci_pm_runtime_enabled(sc))
		return 0;

	usage_count = atomic_read(&sc->dev->power.usage_count);

	if (usage_count == 1) {
		pm_state = qdf_atomic_read(&sc->pm_state);

		if (pm_state == HIF_PM_RUNTIME_STATE_NONE)
			error = "Ignoring unexpected put when runtime pm is disabled";

	} else if (usage_count == 0) {
		error = "PUT Without a Get Operation";
	}

	if (error) {
		hif_pci_runtime_pm_warn(sc, error);
		return -EINVAL;
	}

	hif_pm_stats_runtime_put_record(sc, rtpm_dbgid);

	hif_pm_runtime_mark_last_busy(hif_ctx);
	hif_pm_runtime_put_auto(sc->dev);

	return 0;
}

/**
 * hif_pm_runtime_put_noidle() - do a put operation with no idle
 * @hif_ctx: pointer of HIF context
 * @rtpm_dbgid: dbgid to trace who use it
 *
 * This API will do a runtime put no idle operation
 *
 * Return: 0 for success otherwise an error code
 */
int hif_pm_runtime_put_noidle(struct hif_opaque_softc *hif_ctx,
			      wlan_rtpm_dbgid rtpm_dbgid)
{
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(hif_ctx);
	int usage_count, pm_state;
	char *err = NULL;

	if (!sc)
		return -EINVAL;

	if (!hif_pci_pm_runtime_enabled(sc))
		return 0;

	usage_count = atomic_read(&sc->dev->power.usage_count);
	if (usage_count == 1) {
		pm_state = qdf_atomic_read(&sc->pm_state);
		if (pm_state == HIF_PM_RUNTIME_STATE_NONE)
			err = "Ignore unexpected Put as runtime PM is disabled";
	} else if (usage_count == 0) {
		err = "Put without a Get Operation";
	}

	if (err) {
		hif_pci_runtime_pm_warn(sc, err);
		return -EINVAL;
	}

	hif_pm_stats_runtime_put_record(sc, rtpm_dbgid);
	pm_runtime_put_noidle(sc->dev);

	return 0;
}

/**
 * __hif_pm_runtime_prevent_suspend() - prevent runtime suspend for a protocol
 *                                      reason
 * @hif_sc: pci context
 * @lock: runtime_pm lock being acquired
 *
 * Return 0 if successful.
 */
static int __hif_pm_runtime_prevent_suspend(struct hif_pci_softc
		*hif_sc, struct hif_pm_runtime_lock *lock)
{
	int ret = 0;

	/*
	 * We shouldn't be setting context->timeout to zero here when
	 * context is active as we will have a case where Timeout API's
	 * for the same context called back to back.
	 * eg: echo "1=T:10:T:20" > /d/cnss_runtime_pm
	 * Set context->timeout to zero in hif_pm_runtime_prevent_suspend
	 * API to ensure the timeout version is no more active and
	 * list entry of this context will be deleted during allow suspend.
	 */
	if (lock->active)
		return 0;

	ret = __hif_pm_runtime_get(hif_sc->dev);

	/**
	 * The ret can be -EINPROGRESS, if Runtime status is RPM_RESUMING or
	 * RPM_SUSPENDING. Any other negative value is an error.
	 * We shouldn't be do runtime_put here as in later point allow
	 * suspend gets called with the the context and there the usage count
	 * is decremented, so suspend will be prevented.
	 */

	if (ret < 0 && ret != -EINPROGRESS) {
		hif_sc->pm_stats.runtime_get_err++;
		hif_pci_runtime_pm_warn(hif_sc,
				"Prevent Suspend Runtime PM Error");
	}

	hif_sc->prevent_suspend_cnt++;

	lock->active = true;

	list_add_tail(&lock->list, &hif_sc->prevent_suspend_list);

	qdf_atomic_inc(&hif_sc->pm_stats.prevent_suspend);

	hif_debug("%s: in pm_state:%s ret: %d", __func__,
		  hif_pm_runtime_state_to_string(
			  qdf_atomic_read(&hif_sc->pm_state)),
		  ret);

	return ret;
}

static int __hif_pm_runtime_allow_suspend(struct hif_pci_softc *hif_sc,
		struct hif_pm_runtime_lock *lock)
{
	struct hif_opaque_softc *hif_ctx = GET_HIF_OPAQUE_HDL(hif_sc);
	int ret = 0;
	int usage_count;

	if (hif_sc->prevent_suspend_cnt == 0)
		return ret;

	if (!lock->active)
		return ret;

	usage_count = atomic_read(&hif_sc->dev->power.usage_count);

	/*
	 * During Driver unload, platform driver increments the usage
	 * count to prevent any runtime suspend getting called.
	 * So during driver load in HIF_PM_RUNTIME_STATE_NONE state the
	 * usage_count should be one. Ideally this shouldn't happen as
	 * context->active should be active for allow suspend to happen
	 * Handling this case here to prevent any failures.
	 */
	if ((qdf_atomic_read(&hif_sc->pm_state) == HIF_PM_RUNTIME_STATE_NONE
				&& usage_count == 1) || usage_count == 0) {
		hif_pci_runtime_pm_warn(hif_sc,
				"Allow without a prevent suspend");
		return -EINVAL;
	}

	list_del(&lock->list);

	hif_sc->prevent_suspend_cnt--;

	lock->active = false;
	lock->timeout = 0;

	hif_pm_runtime_mark_last_busy(hif_ctx);
	ret = hif_pm_runtime_put_auto(hif_sc->dev);

	hif_debug("%s: in pm_state:%s ret: %d", __func__,
		  hif_pm_runtime_state_to_string(
			  qdf_atomic_read(&hif_sc->pm_state)),
		  ret);

	qdf_atomic_inc(&hif_sc->pm_stats.allow_suspend);
	return ret;
}

/**
 * hif_pm_runtime_lock_timeout_fn() - callback the runtime lock timeout
 * @data: calback data that is the pci context
 *
 * if runtime locks are acquired with a timeout, this function releases
 * the locks when the last runtime lock expires.
 *
 * dummy implementation until lock acquisition is implemented.
 */
static void hif_pm_runtime_lock_timeout_fn(void *data)
{
	struct hif_pci_softc *hif_sc = data;
	unsigned long timer_expires;
	struct hif_pm_runtime_lock *context, *temp;

	spin_lock_bh(&hif_sc->runtime_lock);

	timer_expires = hif_sc->runtime_timer_expires;

	/* Make sure we are not called too early, this should take care of
	 * following case
	 *
	 * CPU0                         CPU1 (timeout function)
	 * ----                         ----------------------
	 * spin_lock_irq
	 *                              timeout function called
	 *
	 * mod_timer()
	 *
	 * spin_unlock_irq
	 *                              spin_lock_irq
	 */
	if (timer_expires > 0 && !time_after(timer_expires, jiffies)) {
		hif_sc->runtime_timer_expires = 0;
		list_for_each_entry_safe(context, temp,
				&hif_sc->prevent_suspend_list, list) {
			if (context->timeout) {
				__hif_pm_runtime_allow_suspend(hif_sc, context);
				hif_sc->pm_stats.allow_suspend_timeout++;
			}
		}
	}

	spin_unlock_bh(&hif_sc->runtime_lock);
}

int hif_pm_runtime_prevent_suspend(struct hif_opaque_softc *ol_sc,
		struct hif_pm_runtime_lock *data)
{
	struct hif_softc *sc = HIF_GET_SOFTC(ol_sc);
	struct hif_pci_softc *hif_sc = HIF_GET_PCI_SOFTC(ol_sc);
	struct hif_pm_runtime_lock *context = data;

	if (!sc->hif_config.enable_runtime_pm)
		return 0;

	if (!context)
		return -EINVAL;

	if (in_irq())
		WARN_ON(1);

	spin_lock_bh(&hif_sc->runtime_lock);
	context->timeout = 0;
	__hif_pm_runtime_prevent_suspend(hif_sc, context);
	spin_unlock_bh(&hif_sc->runtime_lock);

	return 0;
}

int hif_pm_runtime_allow_suspend(struct hif_opaque_softc *ol_sc,
				struct hif_pm_runtime_lock *data)
{
	struct hif_softc *sc = HIF_GET_SOFTC(ol_sc);
	struct hif_pci_softc *hif_sc = HIF_GET_PCI_SOFTC(ol_sc);
	struct hif_pm_runtime_lock *context = data;

	if (!sc->hif_config.enable_runtime_pm)
		return 0;

	if (!context)
		return -EINVAL;

	if (in_irq())
		WARN_ON(1);

	spin_lock_bh(&hif_sc->runtime_lock);

	__hif_pm_runtime_allow_suspend(hif_sc, context);

	/* The list can be empty as well in cases where
	 * we have one context in the list and the allow
	 * suspend came before the timer expires and we delete
	 * context above from the list.
	 * When list is empty prevent_suspend count will be zero.
	 */
	if (hif_sc->prevent_suspend_cnt == 0 &&
			hif_sc->runtime_timer_expires > 0) {
		qdf_timer_free(&hif_sc->runtime_timer);
		hif_sc->runtime_timer_expires = 0;
	}

	spin_unlock_bh(&hif_sc->runtime_lock);

	return 0;
}

/**
 * hif_pm_runtime_prevent_suspend_timeout() - Prevent runtime suspend timeout
 * @ol_sc: HIF context
 * @lock: which lock is being acquired
 * @delay: Timeout in milliseconds
 *
 * Prevent runtime suspend with a timeout after which runtime suspend would be
 * allowed. This API uses a single timer to allow the suspend and timer is
 * modified if the timeout is changed before timer fires.
 * If the timeout is less than autosuspend_delay then use mark_last_busy instead
 * of starting the timer.
 *
 * It is wise to try not to use this API and correct the design if possible.
 *
 * Return: 0 on success and negative error code on failure
 */
int hif_pm_runtime_prevent_suspend_timeout(struct hif_opaque_softc *ol_sc,
		struct hif_pm_runtime_lock *lock, unsigned int delay)
{
	struct hif_softc *sc = HIF_GET_SOFTC(ol_sc);
	struct hif_pci_softc *hif_sc = HIF_GET_PCI_SOFTC(sc);

	int ret = 0;
	unsigned long expires;
	struct hif_pm_runtime_lock *context = lock;

	if (hif_is_load_or_unload_in_progress(sc)) {
		HIF_ERROR("%s: Load/unload in progress, ignore!",
				__func__);
		return -EINVAL;
	}

	if (hif_is_recovery_in_progress(sc)) {
		HIF_ERROR("%s: LOGP in progress, ignore!", __func__);
		return -EINVAL;
	}

	if (!sc->hif_config.enable_runtime_pm)
		return 0;

	if (!context)
		return -EINVAL;

	if (in_irq())
		WARN_ON(1);

	/*
	 * Don't use internal timer if the timeout is less than auto suspend
	 * delay.
	 */
	if (delay <= hif_sc->dev->power.autosuspend_delay) {
		hif_pm_request_resume(hif_sc->dev);
		hif_pm_runtime_mark_last_busy(ol_sc);
		return ret;
	}

	expires = jiffies + msecs_to_jiffies(delay);
	expires += !expires;

	spin_lock_bh(&hif_sc->runtime_lock);

	context->timeout = delay;
	ret = __hif_pm_runtime_prevent_suspend(hif_sc, context);
	hif_sc->pm_stats.prevent_suspend_timeout++;

	/* Modify the timer only if new timeout is after already configured
	 * timeout
	 */
	if (time_after(expires, hif_sc->runtime_timer_expires)) {
		qdf_timer_mod(&hif_sc->runtime_timer, delay);
		hif_sc->runtime_timer_expires = expires;
	}

	spin_unlock_bh(&hif_sc->runtime_lock);

	HIF_ERROR("%s: pm_state: %s delay: %dms ret: %d\n", __func__,
		hif_pm_runtime_state_to_string(
			qdf_atomic_read(&hif_sc->pm_state)),
					delay, ret);

	return ret;
}

/**
 * hif_runtime_lock_init() - API to initialize Runtime PM context
 * @name: Context name
 *
 * This API initializes the Runtime PM context of the caller and
 * return the pointer.
 *
 * Return: None
 */
int hif_runtime_lock_init(qdf_runtime_lock_t *lock, const char *name)
{
	struct hif_pm_runtime_lock *context;

	HIF_INFO("Initializing Runtime PM wakelock %s", name);

	context = qdf_mem_malloc(sizeof(*context));
	if (!context)
		return -ENOMEM;

	context->name = name ? name : "Default";
	lock->lock = context;

	return 0;
}

/**
 * hif_runtime_lock_deinit() - This API frees the runtime pm ctx
 * @data: Runtime PM context
 *
 * Return: void
 */
void hif_runtime_lock_deinit(struct hif_opaque_softc *hif_ctx,
			     struct hif_pm_runtime_lock *data)
{
	struct hif_pm_runtime_lock *context = data;
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(hif_ctx);

	if (!context) {
		HIF_ERROR("Runtime PM wakelock context is NULL");
		return;
	}

	HIF_INFO("Deinitializing Runtime PM wakelock %s", context->name);

	/*
	 * Ensure to delete the context list entry and reduce the usage count
	 * before freeing the context if context is active.
	 */
	if (sc) {
		spin_lock_bh(&sc->runtime_lock);
		__hif_pm_runtime_allow_suspend(sc, context);
		spin_unlock_bh(&sc->runtime_lock);
	}

	qdf_mem_free(context);
}

/**
 * hif_pm_runtime_is_suspended() - API to check if driver has runtime suspended
 * @hif_ctx: HIF context
 *
 * Return: true for runtime suspended, otherwise false
 */
bool hif_pm_runtime_is_suspended(struct hif_opaque_softc *hif_ctx)
{
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(hif_ctx);

	return qdf_atomic_read(&sc->pm_state) ==
		HIF_PM_RUNTIME_STATE_SUSPENDED;
}

/**
 * hif_pm_runtime_get_monitor_wake_intr() - API to get monitor_wake_intr
 * @hif_ctx: HIF context
 *
 * monitor_wake_intr variable can be used to indicate if driver expects wake
 * MSI for runtime PM
 *
 * Return: monitor_wake_intr variable
 */
int hif_pm_runtime_get_monitor_wake_intr(struct hif_opaque_softc *hif_ctx)
{
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(hif_ctx);

	return qdf_atomic_read(&sc->monitor_wake_intr);
}

/**
 * hif_pm_runtime_set_monitor_wake_intr() - API to set monitor_wake_intr
 * @hif_ctx: HIF context
 * @val: value to set
 *
 * monitor_wake_intr variable can be used to indicate if driver expects wake
 * MSI for runtime PM
 *
 * Return: void
 */
void hif_pm_runtime_set_monitor_wake_intr(struct hif_opaque_softc *hif_ctx,
					  int val)
{
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(hif_ctx);

	qdf_atomic_set(&sc->monitor_wake_intr, val);
}

/**
 * hif_pm_runtime_mark_dp_rx_busy() - Set last busy mark my data path
 * @hif_ctx: HIF context
 *
 * Return: void
 */
void hif_pm_runtime_mark_dp_rx_busy(struct hif_opaque_softc *hif_ctx)
{
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(hif_ctx);

	if (!sc)
		return;

	qdf_atomic_set(&sc->pm_dp_rx_busy, 1);
	sc->dp_last_busy_timestamp = qdf_get_log_timestamp_usecs();

	hif_pm_runtime_mark_last_busy(hif_ctx);
}

/**
 * hif_pm_runtime_is_dp_rx_busy() - Check if last mark busy by dp rx
 * @hif_ctx: HIF context
 *
 * Return: dp rx busy set value
 */
int hif_pm_runtime_is_dp_rx_busy(struct hif_opaque_softc *hif_ctx)
{
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(hif_ctx);

	if (!sc)
		return 0;

	return qdf_atomic_read(&sc->pm_dp_rx_busy);
}

/**
 * hif_pm_runtime_get_dp_rx_busy_mark() - Get last busy by dp rx timestamp
 * @hif_ctx: HIF context
 *
 * Return: timestamp of last mark busy by dp rx
 */
qdf_time_t hif_pm_runtime_get_dp_rx_busy_mark(struct hif_opaque_softc *hif_ctx)
{
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(hif_ctx);

	if (!sc)
		return 0;

	return sc->dp_last_busy_timestamp;
}

void hif_pm_set_link_state(struct hif_opaque_softc *hif_handle, uint8_t val)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_handle);

	qdf_atomic_set(&scn->pm_link_state, val);
}

uint8_t hif_pm_get_link_state(struct hif_opaque_softc *hif_handle)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_handle);

	return qdf_atomic_read(&scn->pm_link_state);
}
#endif /* FEATURE_RUNTIME_PM */

int hif_pci_legacy_map_ce_to_irq(struct hif_softc *scn, int ce_id)
{
	struct hif_pci_softc *pci_scn = HIF_GET_PCI_SOFTC(scn);

	/* legacy case only has one irq */
	return pci_scn->irq;
}

int hif_pci_addr_in_boundary(struct hif_softc *scn, uint32_t offset)
{
	struct hif_pci_softc *sc = HIF_GET_PCI_SOFTC(scn);
	struct hif_target_info *tgt_info;

	tgt_info = hif_get_target_info_handle(GET_HIF_OPAQUE_HDL(scn));

	if (tgt_info->target_type == TARGET_TYPE_QCA6290 ||
	    tgt_info->target_type == TARGET_TYPE_QCA6390 ||
	    tgt_info->target_type == TARGET_TYPE_QCA6490 ||
	    tgt_info->target_type == TARGET_TYPE_QCA8074) {
		/*
		 * Need to consider offset's memtype for QCA6290/QCA8074,
		 * also mem_len and DRAM_BASE_ADDRESS/DRAM_SIZE need to be
		 * well initialized/defined.
		 */
		return 0;
	}

	if ((offset >= DRAM_BASE_ADDRESS && offset <= DRAM_BASE_ADDRESS + DRAM_SIZE)
		 || (offset + sizeof(unsigned int) <= sc->mem_len)) {
		return 0;
	}

	HIF_TRACE("Refusing to read memory at 0x%x - 0x%x (max 0x%zx)\n",
		  offset, (uint32_t)(offset + sizeof(unsigned int)),
		  sc->mem_len);

	return -EINVAL;
}

/**
 * hif_pci_needs_bmi() - return true if the soc needs bmi through the driver
 * @scn: hif context
 *
 * Return: true if soc needs driver bmi otherwise false
 */
bool hif_pci_needs_bmi(struct hif_softc *scn)
{
	return !ce_srng_based(scn);
}

#ifdef FORCE_WAKE
#ifdef DEVICE_FORCE_WAKE_ENABLE
int hif_force_wake_request(struct hif_opaque_softc *hif_handle)
{
	uint32_t timeout, value;
	struct hif_softc *scn = (struct hif_softc *)hif_handle;
	struct hif_pci_softc *pci_scn = HIF_GET_PCI_SOFTC(scn);

	HIF_STATS_INC(pci_scn, mhi_force_wake_request_vote, 1);

	if (qdf_in_interrupt())
		timeout = FORCE_WAKE_DELAY_TIMEOUT_MS * 1000;
	else
		timeout = 0;

	if (pld_force_wake_request_sync(scn->qdf_dev->dev, timeout)) {
		hif_err("force wake request send failed");
		HIF_STATS_INC(pci_scn, mhi_force_wake_failure, 1);
		return -EINVAL;
	}

	/* If device's M1 state-change event races here, it can be ignored,
	 * as the device is expected to immediately move from M2 to M0
	 * without entering low power state.
	 */
	if (!pld_is_device_awake(scn->qdf_dev->dev))
		HIF_INFO("%s: state-change event races, ignore", __func__);

	HIF_STATS_INC(pci_scn, mhi_force_wake_success, 1);
	hif_write32_mb(scn,
		       scn->mem +
		       PCIE_SOC_PCIE_REG_PCIE_SCRATCH_0_SOC_PCIE_REG,
		       0);
	hif_write32_mb(scn,
		       scn->mem +
		       PCIE_PCIE_LOCAL_REG_PCIE_SOC_WAKE_PCIE_LOCAL_REG,
		       1);

	HIF_STATS_INC(pci_scn, soc_force_wake_register_write_success, 1);
	/*
	 * do not reset the timeout
	 * total_wake_time = MHI_WAKE_TIME + PCI_WAKE_TIME < 50 ms
	 */
	timeout = 0;
	do {
		value =
		hif_read32_mb(scn,
			      scn->mem +
			      PCIE_SOC_PCIE_REG_PCIE_SCRATCH_0_SOC_PCIE_REG);
		if (value)
			break;
		qdf_mdelay(FORCE_WAKE_DELAY_MS);
		timeout += FORCE_WAKE_DELAY_MS;
	} while (timeout <= FORCE_WAKE_DELAY_TIMEOUT_MS);

	if (!value) {
		hif_err("failed handshake mechanism");
		HIF_STATS_INC(pci_scn, soc_force_wake_failure, 1);
		return -ETIMEDOUT;
	}

	HIF_STATS_INC(pci_scn, soc_force_wake_success, 1);
	return 0;
}

int hif_force_wake_release(struct hif_opaque_softc *hif_handle)
{
	int ret;
	struct hif_softc *scn = (struct hif_softc *)hif_handle;
	struct hif_pci_softc *pci_scn = HIF_GET_PCI_SOFTC(scn);

	ret = pld_force_wake_release(scn->qdf_dev->dev);
	if (ret) {
		hif_err("force wake release failure");
		HIF_STATS_INC(pci_scn, mhi_force_wake_release_failure, 1);
		return ret;
	}

	HIF_STATS_INC(pci_scn, mhi_force_wake_release_success, 1);
	hif_write32_mb(scn,
		       scn->mem +
		       PCIE_PCIE_LOCAL_REG_PCIE_SOC_WAKE_PCIE_LOCAL_REG,
		       0);
	HIF_STATS_INC(pci_scn, soc_force_wake_release_success, 1);
	return 0;
}

#else /* DEVICE_FORCE_WAKE_ENABLE */
/** hif_force_wake_request() - Disable the PCIE scratch register
 * write/read
 *
 * Return: 0
 */
int hif_force_wake_request(struct hif_opaque_softc *hif_handle)
{
	struct hif_softc *scn = (struct hif_softc *)hif_handle;
	struct hif_pci_softc *pci_scn = HIF_GET_PCI_SOFTC(scn);
	uint32_t timeout;

	HIF_STATS_INC(pci_scn, mhi_force_wake_request_vote, 1);

	if (qdf_in_interrupt())
		timeout = FORCE_WAKE_DELAY_TIMEOUT_MS * 1000;
	else
		timeout = 0;

	if (pld_force_wake_request_sync(scn->qdf_dev->dev, timeout)) {
		hif_err("force wake request send failed");
		HIF_STATS_INC(pci_scn, mhi_force_wake_failure, 1);
		return -EINVAL;
	}

	/* If device's M1 state-change event races here, it can be ignored,
	 * as the device is expected to immediately move from M2 to M0
	 * without entering low power state.
	 */
	if (!pld_is_device_awake(scn->qdf_dev->dev))
		HIF_INFO("%s: state-change event races, ignore", __func__);

	HIF_STATS_INC(pci_scn, mhi_force_wake_success, 1);

	return 0;
}

int hif_force_wake_release(struct hif_opaque_softc *hif_handle)
{
	int ret;
	struct hif_softc *scn = (struct hif_softc *)hif_handle;
	struct hif_pci_softc *pci_scn = HIF_GET_PCI_SOFTC(scn);

	ret = pld_force_wake_release(scn->qdf_dev->dev);
	if (ret) {
		hif_err("force wake release failure");
		HIF_STATS_INC(pci_scn, mhi_force_wake_release_failure, 1);
		return ret;
	}

	HIF_STATS_INC(pci_scn, mhi_force_wake_release_success, 1);
	return 0;
}
#endif /* DEVICE_FORCE_WAKE_ENABLE */

void hif_print_pci_stats(struct hif_pci_softc *pci_handle)
{
	hif_debug("mhi_force_wake_request_vote: %d",
		  pci_handle->stats.mhi_force_wake_request_vote);
	hif_debug("mhi_force_wake_failure: %d",
		  pci_handle->stats.mhi_force_wake_failure);
	hif_debug("mhi_force_wake_success: %d",
		  pci_handle->stats.mhi_force_wake_success);
	hif_debug("soc_force_wake_register_write_success: %d",
		  pci_handle->stats.soc_force_wake_register_write_success);
	hif_debug("soc_force_wake_failure: %d",
		  pci_handle->stats.soc_force_wake_failure);
	hif_debug("soc_force_wake_success: %d",
		  pci_handle->stats.soc_force_wake_success);
	hif_debug("mhi_force_wake_release_failure: %d",
		  pci_handle->stats.mhi_force_wake_release_failure);
	hif_debug("mhi_force_wake_release_success: %d",
		  pci_handle->stats.mhi_force_wake_release_success);
	hif_debug("oc_force_wake_release_success: %d",
		  pci_handle->stats.soc_force_wake_release_success);
}
#endif /* FORCE_WAKE */

#ifdef FEATURE_HAL_DELAYED_REG_WRITE
int hif_prevent_link_low_power_states(struct hif_opaque_softc *hif)
{
	return pld_prevent_l1(HIF_GET_SOFTC(hif)->qdf_dev->dev);
}

void hif_allow_link_low_power_states(struct hif_opaque_softc *hif)
{
	pld_allow_l1(HIF_GET_SOFTC(hif)->qdf_dev->dev);
}
#endif
