/* Copyright (c) 2002,2007-2012, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/ioctl.h>
#include <linux/sched.h>

#include <mach/socinfo.h>

#include "kgsl.h"
#include "kgsl_pwrscale.h"
#include "kgsl_cffdump.h"
#include "kgsl_sharedmem.h"

#include "adreno.h"
#include "adreno_pm4types.h"
#include "adreno_debugfs.h"
#include "adreno_postmortem.h"

#include "a2xx_reg.h"
#include "kgsl_mmu.h"

#define DRIVER_VERSION_MAJOR   3
#define DRIVER_VERSION_MINOR   1

/* Adreno MH arbiter config*/
#define ADRENO_CFG_MHARB \
	(0x10 \
		| (0 << MH_ARBITER_CONFIG__SAME_PAGE_GRANULARITY__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__L1_ARB_ENABLE__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__L1_ARB_HOLD_ENABLE__SHIFT) \
		| (0 << MH_ARBITER_CONFIG__L2_ARB_CONTROL__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__PAGE_SIZE__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__TC_REORDER_ENABLE__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__TC_ARB_HOLD_ENABLE__SHIFT) \
		| (0 << MH_ARBITER_CONFIG__IN_FLIGHT_LIMIT_ENABLE__SHIFT) \
		| (0x8 << MH_ARBITER_CONFIG__IN_FLIGHT_LIMIT__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__CP_CLNT_ENABLE__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__VGT_CLNT_ENABLE__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__TC_CLNT_ENABLE__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__RB_CLNT_ENABLE__SHIFT) \
		| (1 << MH_ARBITER_CONFIG__PA_CLNT_ENABLE__SHIFT))

#define ADRENO_MMU_CONFIG						\
	(0x01								\
	 | (MMU_CONFIG << MH_MMU_CONFIG__RB_W_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__CP_W_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__CP_R0_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__CP_R1_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__CP_R2_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__CP_R3_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__CP_R4_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__VGT_R0_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__VGT_R1_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__TC_R_CLNT_BEHAVIOR__SHIFT)	\
	 | (MMU_CONFIG << MH_MMU_CONFIG__PA_W_CLNT_BEHAVIOR__SHIFT))

static const struct kgsl_functable adreno_functable;

static struct adreno_device device_3d0 = {
	.dev = {
		.name = DEVICE_3D0_NAME,
		.id = KGSL_DEVICE_3D0,
		.ver_major = DRIVER_VERSION_MAJOR,
		.ver_minor = DRIVER_VERSION_MINOR,
		.mh = {
			.mharb  = ADRENO_CFG_MHARB,
			/* Remove 1k boundary check in z470 to avoid a GPU
			 * hang.  Notice that this solution won't work if
			 * both EBI and SMI are used
			 */
			.mh_intf_cfg1 = 0x00032f07,
			/* turn off memory protection unit by setting
			   acceptable physical address range to include
			   all pages. */
			.mpu_base = 0x00000000,
			.mpu_range =  0xFFFFF000,
		},
		.mmu = {
			.config = ADRENO_MMU_CONFIG,
		},
		.pwrctrl = {
			.regulator_name = "fs_gfx3d",
			.irq_name = KGSL_3D0_IRQ,
		},
		.mutex = __MUTEX_INITIALIZER(device_3d0.dev.mutex),
		.state = KGSL_STATE_INIT,
		.active_cnt = 0,
		.iomemname = KGSL_3D0_REG_MEMORY,
		.ftbl = &adreno_functable,
#ifdef CONFIG_HAS_EARLYSUSPEND
		.display_off = {
			.level = EARLY_SUSPEND_LEVEL_STOP_DRAWING,
			.suspend = kgsl_early_suspend_driver,
			.resume = kgsl_late_resume_driver,
		},
#endif
	},
	.gmemspace = {
		.gpu_base = 0,
		.sizebytes = SZ_256K,
	},
	.pfp_fw = NULL,
	.pm4_fw = NULL,
	.wait_timeout = 0, /* in milliseconds, 0 means disabled */
	.ib_check_level = 0,
};

/* This set of registers are used for Hang detection
 * If the values of these registers are same after
 * KGSL_TIMEOUT_PART time, GPU hang is reported in
 * kernel log.
 */
unsigned int hang_detect_regs[] = {
	REG_RBBM_STATUS, /* Set to A2xx register value by default */
	REG_CP_RB_RPTR,
	REG_CP_IB1_BASE,
	REG_CP_IB1_BUFSZ,
	REG_CP_IB2_BASE,
	REG_CP_IB2_BUFSZ,
};

const unsigned int hang_detect_regs_count = ARRAY_SIZE(hang_detect_regs);

/*
 * This is the master list of all GPU cores that are supported by this
 * driver.
 */

#define ANY_ID (~0)

static const struct {
	enum adreno_gpurev gpurev;
	unsigned int core, major, minor, patchid;
	const char *pm4fw;
	const char *pfpfw;
	struct adreno_gpudev *gpudev;
	unsigned int istore_size;
	unsigned int pix_shader_start;
} adreno_gpulist[] = {
	{ ADRENO_REV_A200, 0, 2, ANY_ID, ANY_ID,
		"yamato_pm4.fw", "yamato_pfp.fw", &adreno_a2xx_gpudev,
		512, 384},
	{ ADRENO_REV_A205, 0, 1, 0, ANY_ID,
		"yamato_pm4.fw", "yamato_pfp.fw", &adreno_a2xx_gpudev,
		512, 384},
	{ ADRENO_REV_A220, 2, 1, ANY_ID, ANY_ID,
		"leia_pm4_470.fw", "leia_pfp_470.fw", &adreno_a2xx_gpudev,
		512, 384},
	/*
	 * patchlevel 5 (8960v2) needs special pm4 firmware to work around
	 * a hardware problem.
	 */
	{ ADRENO_REV_A225, 2, 2, 0, 5,
		"a225p5_pm4.fw", "a225_pfp.fw", &adreno_a2xx_gpudev,
		1536, 768 },
	{ ADRENO_REV_A225, 2, 2, 0, 6,
		"a225_pm4.fw", "a225_pfp.fw", &adreno_a2xx_gpudev,
		1536, 768 },
	{ ADRENO_REV_A225, 2, 2, ANY_ID, ANY_ID,
		"a225_pm4.fw", "a225_pfp.fw", &adreno_a2xx_gpudev,
		1536, 768 },
};

static void adreno_gmeminit(struct adreno_device *adreno_dev)
{
	struct kgsl_device *device = &adreno_dev->dev;
	union reg_rb_edram_info rb_edram_info;
	unsigned int gmem_size;
	unsigned int edram_value = 0;

	/* make sure edram range is aligned to size */
	BUG_ON(adreno_dev->gmemspace.gpu_base &
				(adreno_dev->gmemspace.sizebytes - 1));

	/* get edram_size value equivalent */
	gmem_size = (adreno_dev->gmemspace.sizebytes >> 14);
	while (gmem_size >>= 1)
		edram_value++;

	rb_edram_info.val = 0;

	rb_edram_info.f.edram_size = edram_value;
	rb_edram_info.f.edram_mapping_mode = 0; /* EDRAM_MAP_UPPER */

	/* must be aligned to size */
	rb_edram_info.f.edram_range = (adreno_dev->gmemspace.gpu_base >> 14);

	adreno_regwrite(device, REG_RB_EDRAM_INFO, rb_edram_info.val);
}

static irqreturn_t adreno_isr(int irq, void *data)
{
	irqreturn_t result;
	struct kgsl_device *device = data;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

	result = adreno_dev->gpudev->irq_handler(adreno_dev);

	if (device->requested_state == KGSL_STATE_NONE) {
		if (device->pwrctrl.nap_allowed == true) {
			kgsl_pwrctrl_request_state(device, KGSL_STATE_NAP);
			queue_work(device->work_queue, &device->idle_check_ws);
		} else if (device->pwrscale.policy != NULL) {
			queue_work(device->work_queue, &device->idle_check_ws);
		}
	}

	/* Reset the time-out in our idle timer */
	mod_timer_pending(&device->idle_timer,
		jiffies + device->pwrctrl.interval_timeout);
	return result;
}

static void adreno_cleanup_pt(struct kgsl_device *device,
			struct kgsl_pagetable *pagetable)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct adreno_ringbuffer *rb = &adreno_dev->ringbuffer;

	kgsl_mmu_unmap(pagetable, &rb->buffer_desc);

	kgsl_mmu_unmap(pagetable, &rb->memptrs_desc);

	kgsl_mmu_unmap(pagetable, &device->memstore);

	kgsl_mmu_unmap(pagetable, &device->mmu.setstate_memory);
}

static int adreno_setup_pt(struct kgsl_device *device,
			struct kgsl_pagetable *pagetable)
{
	int result = 0;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct adreno_ringbuffer *rb = &adreno_dev->ringbuffer;

	result = kgsl_mmu_map_global(pagetable, &rb->buffer_desc,
				     GSL_PT_PAGE_RV);
	if (result)
		goto error;

	result = kgsl_mmu_map_global(pagetable, &rb->memptrs_desc,
				     GSL_PT_PAGE_RV | GSL_PT_PAGE_WV);
	if (result)
		goto unmap_buffer_desc;

	result = kgsl_mmu_map_global(pagetable, &device->memstore,
				     GSL_PT_PAGE_RV | GSL_PT_PAGE_WV);
	if (result)
		goto unmap_memptrs_desc;

	result = kgsl_mmu_map_global(pagetable, &device->mmu.setstate_memory,
				     GSL_PT_PAGE_RV | GSL_PT_PAGE_WV);
	if (result)
		goto unmap_memstore_desc;

	return result;

unmap_memstore_desc:
	kgsl_mmu_unmap(pagetable, &device->memstore);

unmap_memptrs_desc:
	kgsl_mmu_unmap(pagetable, &rb->memptrs_desc);

unmap_buffer_desc:
	kgsl_mmu_unmap(pagetable, &rb->buffer_desc);

error:
	return result;
}

static void adreno_setstate(struct kgsl_device *device,
					unsigned int context_id,
					uint32_t flags)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	unsigned int link[32];
	unsigned int *cmds = &link[0];
	int sizedwords = 0;
	unsigned int mh_mmu_invalidate = 0x00000003; /*invalidate all and tc */
	struct kgsl_context *context;
	struct adreno_context *adreno_ctx = NULL;

	/*
	 * If possible, then set the state via the command stream to avoid
	 * a CPU idle.  Otherwise, use the default setstate which uses register
	 * writes For CFF dump we must idle and use the registers so that it is
	 * easier to filter out the mmu accesses from the dump
	 */
	if (!kgsl_cff_dump_enable && adreno_dev->drawctxt_active) {
		context = idr_find(&device->context_idr, context_id);
		adreno_ctx = context->devctxt;

		if (flags & KGSL_MMUFLAGS_PTUPDATE) {
			/* wait for graphics pipe to be idle */
			*cmds++ = cp_type3_packet(CP_WAIT_FOR_IDLE, 1);
			*cmds++ = 0x00000000;

			/* set page table base */
			*cmds++ = cp_type0_packet(MH_MMU_PT_BASE, 1);
			*cmds++ = kgsl_pt_get_base_addr(
					device->mmu.hwpagetable);
			sizedwords += 4;
		}

		if (flags & KGSL_MMUFLAGS_TLBFLUSH) {
			if (!(flags & KGSL_MMUFLAGS_PTUPDATE)) {
				*cmds++ = cp_type3_packet(CP_WAIT_FOR_IDLE,
								1);
				*cmds++ = 0x00000000;
				sizedwords += 2;
			}
			*cmds++ = cp_type0_packet(MH_MMU_INVALIDATE, 1);
			*cmds++ = mh_mmu_invalidate;
			sizedwords += 2;
		}

		if (flags & KGSL_MMUFLAGS_PTUPDATE &&
			adreno_is_a20x(adreno_dev)) {
			/* HW workaround: to resolve MMU page fault interrupts
			* caused by the VGT.It prevents the CP PFP from filling
			* the VGT DMA request fifo too early,thereby ensuring
			* that the VGT will not fetch vertex/bin data until
			* after the page table base register has been updated.
			*
			* Two null DRAW_INDX_BIN packets are inserted right
			* after the page table base update, followed by a
			* wait for idle. The null packets will fill up the
			* VGT DMA request fifo and prevent any further
			* vertex/bin updates from occurring until the wait
			* has finished. */
			*cmds++ = cp_type3_packet(CP_SET_CONSTANT, 2);
			*cmds++ = (0x4 << 16) |
				(REG_PA_SU_SC_MODE_CNTL - 0x2000);
			*cmds++ = 0;	  /* disable faceness generation */
			*cmds++ = cp_type3_packet(CP_SET_BIN_BASE_OFFSET, 1);
			*cmds++ = device->mmu.setstate_memory.gpuaddr;
			*cmds++ = cp_type3_packet(CP_DRAW_INDX_BIN, 6);
			*cmds++ = 0;	  /* viz query info */
			*cmds++ = 0x0003C004; /* draw indicator */
			*cmds++ = 0;	  /* bin base */
			*cmds++ = 3;	  /* bin size */
			*cmds++ =
			device->mmu.setstate_memory.gpuaddr; /* dma base */
			*cmds++ = 6;	  /* dma size */
			*cmds++ = cp_type3_packet(CP_DRAW_INDX_BIN, 6);
			*cmds++ = 0;	  /* viz query info */
			*cmds++ = 0x0003C004; /* draw indicator */
			*cmds++ = 0;	  /* bin base */
			*cmds++ = 3;	  /* bin size */
			/* dma base */
			*cmds++ = device->mmu.setstate_memory.gpuaddr;
			*cmds++ = 6;	  /* dma size */
			*cmds++ = cp_type3_packet(CP_WAIT_FOR_IDLE, 1);
			*cmds++ = 0x00000000;
			sizedwords += 21;
		}


		if (flags & (KGSL_MMUFLAGS_PTUPDATE | KGSL_MMUFLAGS_TLBFLUSH)) {
			*cmds++ = cp_type3_packet(CP_INVALIDATE_STATE, 1);
			*cmds++ = 0x7fff; /* invalidate all base pointers */
			sizedwords += 2;
		}

		adreno_ringbuffer_issuecmds(device, adreno_ctx,
					KGSL_CMD_FLAGS_PMODE,
					&link[0], sizedwords);
	} else {
		kgsl_mmu_device_setstate(device, flags);
	}
}

static unsigned int
adreno_getchipid(struct kgsl_device *device)
{
	unsigned int chipid = 0;
	unsigned int coreid, majorid, minorid, patchid, revid;
	uint32_t soc_platform_version = socinfo_get_version();

	adreno_regread(device, REG_RBBM_PERIPHID1, &coreid);
	adreno_regread(device, REG_RBBM_PERIPHID2, &majorid);
	adreno_regread(device, REG_RBBM_PATCH_RELEASE, &revid);

	/*
	* adreno 22x gpus are indicated by coreid 2,
	* but REG_RBBM_PERIPHID1 always contains 0 for this field
	*/
	if (cpu_is_msm8960() || cpu_is_msm8x60() || cpu_is_msm8930())
		chipid = 2 << 24;
	else
		chipid = (coreid & 0xF) << 24;

	chipid |= ((majorid >> 4) & 0xF) << 16;

	minorid = ((revid >> 0)  & 0xFF);

	patchid = ((revid >> 16) & 0xFF);

	/* 8x50 returns 0 for patch release, but it should be 1 */
	/* 8960v3 returns 5 for patch release, but it should be 6 */
	if (cpu_is_qsd8x50())
		patchid = 1;
	else if (cpu_is_msm8960() &&
			SOCINFO_VERSION_MAJOR(soc_platform_version) == 3)
		patchid = 6;

	chipid |= (minorid << 8) | patchid;

	return chipid;
}

static inline bool _rev_match(unsigned int id, unsigned int entry)
{
	return (entry == ANY_ID || entry == id);
}

static void
adreno_identify_gpu(struct adreno_device *adreno_dev)
{
	unsigned int i, core, major, minor, patchid;

	adreno_dev->chip_id = adreno_getchipid(&adreno_dev->dev);

	core = (adreno_dev->chip_id >> 24) & 0xff;
	major = (adreno_dev->chip_id >> 16) & 0xff;
	minor = (adreno_dev->chip_id >> 8) & 0xff;
	patchid = (adreno_dev->chip_id & 0xff);

	for (i = 0; i < ARRAY_SIZE(adreno_gpulist); i++) {
		if (core == adreno_gpulist[i].core &&
		    _rev_match(major, adreno_gpulist[i].major) &&
		    _rev_match(minor, adreno_gpulist[i].minor) &&
		    _rev_match(patchid, adreno_gpulist[i].patchid))
			break;
	}

	if (i == ARRAY_SIZE(adreno_gpulist)) {
		adreno_dev->gpurev = ADRENO_REV_UNKNOWN;
		return;
	}

	adreno_dev->gpurev = adreno_gpulist[i].gpurev;
	adreno_dev->gpudev = adreno_gpulist[i].gpudev;
	adreno_dev->pfp_fwfile = adreno_gpulist[i].pfpfw;
	adreno_dev->pm4_fwfile = adreno_gpulist[i].pm4fw;
	adreno_dev->istore_size = adreno_gpulist[i].istore_size;
	adreno_dev->pix_shader_start = adreno_gpulist[i].pix_shader_start;
}

static int __devinit
adreno_probe(struct platform_device *pdev)
{
	struct kgsl_device *device;
	struct adreno_device *adreno_dev;
	int status = -EINVAL;

	device = (struct kgsl_device *)pdev->id_entry->driver_data;
	adreno_dev = ADRENO_DEVICE(device);
	device->parentdev = &pdev->dev;

	init_completion(&device->recovery_gate);

	status = adreno_ringbuffer_init(device);
	if (status != 0)
		goto error;

	status = kgsl_device_platform_probe(device, adreno_isr);
	if (status)
		goto error_close_rb;

	adreno_debugfs_init(device);

	kgsl_pwrscale_init(device);
	kgsl_pwrscale_attach_policy(device, ADRENO_DEFAULT_PWRSCALE_POLICY);

	INIT_WORK(&device->print_fault_ib, adreno_print_fault_ib_work);

	device->flags &= ~KGSL_FLAGS_SOFT_RESET;
	return 0;

error_close_rb:
	adreno_ringbuffer_close(&adreno_dev->ringbuffer);
error:
	device->parentdev = NULL;
	return status;
}

static int __devexit adreno_remove(struct platform_device *pdev)
{
	struct kgsl_device *device;
	struct adreno_device *adreno_dev;

	device = (struct kgsl_device *)pdev->id_entry->driver_data;
	adreno_dev = ADRENO_DEVICE(device);

	kgsl_pwrscale_detach_policy(device);
	kgsl_pwrscale_close(device);

	adreno_ringbuffer_close(&adreno_dev->ringbuffer);
	kgsl_device_platform_remove(device);

	return 0;
}

static int adreno_start(struct kgsl_device *device, unsigned int init_ram)
{
	int status = -EINVAL;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	int init_reftimestamp = 0x7fffffff;

	if (KGSL_STATE_DUMP_AND_RECOVER != device->state)
		kgsl_pwrctrl_set_state(device, KGSL_STATE_INIT);

	/* Power up the device */
	kgsl_pwrctrl_enable(device);

	/* Identify the specific GPU */
	adreno_identify_gpu(adreno_dev);

	if (adreno_dev->gpurev == ADRENO_REV_UNKNOWN) {
		KGSL_DRV_ERR(device, "Unknown chip ID %x\n",
			adreno_dev->chip_id);
		goto error_clk_off;
	}

	if (adreno_is_a20x(adreno_dev)) {
		/*
		 * the MH_CLNT_INTF_CTRL_CONFIG registers aren't present
		 * on older gpus
		 */
		device->mh.mh_intf_cfg1 = 0;
		device->mh.mh_intf_cfg2 = 0;
	}

	kgsl_mh_start(device);

	if (kgsl_mmu_start(device))
		goto error_clk_off;

	/*We need to make sure all blocks are powered up and clocked before
	*issuing a soft reset.  The overrides will then be turned off (set to 0)
	*/
	adreno_regwrite(device, REG_RBBM_PM_OVERRIDE1, 0xfffffffe);
	adreno_regwrite(device, REG_RBBM_PM_OVERRIDE2, 0xffffffff);

	/* Only reset CP block if all blocks have previously been reset */
	if (!(device->flags & KGSL_FLAGS_SOFT_RESET) ||
		!adreno_is_a22x(adreno_dev)) {
		adreno_regwrite(device, REG_RBBM_SOFT_RESET, 0xFFFFFFFF);
		device->flags |= KGSL_FLAGS_SOFT_RESET;
	} else
		adreno_regwrite(device, REG_RBBM_SOFT_RESET, 0x00000001);

	/* The core is in an indeterminate state until the reset completes
	 * after 30ms.
	 */
	msleep(30);

	adreno_regwrite(device, REG_RBBM_SOFT_RESET, 0x00000000);

	adreno_regwrite(device, REG_RBBM_CNTL, 0x00004442);

	if (adreno_is_a225(adreno_dev)) {
		/* Enable large instruction store for A225 */
		adreno_regwrite(device, REG_SQ_FLOW_CONTROL, 0x18000000);
	}

	adreno_regwrite(device, REG_SQ_VS_PROGRAM, 0x00000000);
	adreno_regwrite(device, REG_SQ_PS_PROGRAM, 0x00000000);

	if (cpu_is_msm8960() || cpu_is_msm8930())
		adreno_regwrite(device, REG_RBBM_PM_OVERRIDE1, 0x200);
	else
		adreno_regwrite(device, REG_RBBM_PM_OVERRIDE1, 0);

	if (!adreno_is_a22x(adreno_dev))
		adreno_regwrite(device, REG_RBBM_PM_OVERRIDE2, 0);
	else
		adreno_regwrite(device, REG_RBBM_PM_OVERRIDE2, 0x80);

	kgsl_sharedmem_set(&device->memstore, 0, 0, device->memstore.size);

	kgsl_sharedmem_writel(&device->memstore,
			      KGSL_DEVICE_MEMSTORE_OFFSET(ref_wait_ts),
			      init_reftimestamp);

	adreno_regwrite(device, REG_RBBM_DEBUG, 0x00080000);

	/* Make sure interrupts are disabled */

	adreno_regwrite(device, REG_RBBM_INT_CNTL, 0);
	adreno_regwrite(device, REG_CP_INT_CNTL, 0);
	adreno_regwrite(device, REG_SQ_INT_CNTL, 0);

	if (adreno_is_a22x(adreno_dev))
		adreno_dev->gmemspace.sizebytes = SZ_512K;
	else
		adreno_dev->gmemspace.sizebytes = SZ_256K;
	adreno_gmeminit(adreno_dev);

	kgsl_pwrctrl_irq(device, KGSL_PWRFLAGS_ON);
	device->ftbl->irqctrl(device, 1);

	status = adreno_ringbuffer_start(&adreno_dev->ringbuffer, init_ram);

	if (status == 0) {
		/* While recovery is on we do not want timer to
		 * fire and attempt to change any device state */
		if (KGSL_STATE_DUMP_AND_RECOVER != device->state)
			mod_timer(&device->idle_timer, jiffies + FIRST_TIMEOUT);
		return 0;
	}

	kgsl_pwrctrl_irq(device, KGSL_PWRFLAGS_OFF);
	kgsl_mmu_stop(device);
error_clk_off:
	kgsl_pwrctrl_disable(device);

	return status;
}

static int adreno_stop(struct kgsl_device *device)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

	adreno_dev->drawctxt_active = NULL;

	adreno_ringbuffer_stop(&adreno_dev->ringbuffer);

	kgsl_mmu_stop(device);

	device->ftbl->irqctrl(device, 0);
	kgsl_pwrctrl_irq(device, KGSL_PWRFLAGS_OFF);
	del_timer_sync(&device->idle_timer);

	/* Power down the device */
	kgsl_pwrctrl_disable(device);

	return 0;
}

static void adreno_mark_context_status(struct kgsl_device *device,
					int recovery_status)
{
	struct kgsl_context *context;
	int next = 0;
	/*
	 * Set the reset status of all contexts to
	 * INNOCENT_CONTEXT_RESET_EXT except for the bad context
	 * since thats the guilty party, if recovery failed then
	 * mark all as guilty
	 */
	while ((context = idr_get_next(&device->context_idr, &next))) {
		struct adreno_context *adreno_context = context->devctxt;
		if (recovery_status) {
			context->reset_status =
					KGSL_CTX_STAT_GUILTY_CONTEXT_RESET_EXT;
			adreno_context->flags |= CTXT_FLAGS_GPU_HANG;
		} else if (KGSL_CTX_STAT_GUILTY_CONTEXT_RESET_EXT !=
			context->reset_status) {
			if (adreno_context->flags & (CTXT_FLAGS_GPU_HANG ||
				CTXT_FLAGS_GPU_HANG_RECOVERED))
				context->reset_status =
				KGSL_CTX_STAT_GUILTY_CONTEXT_RESET_EXT;
			else
				context->reset_status =
				KGSL_CTX_STAT_INNOCENT_CONTEXT_RESET_EXT;
		}
		next = next + 1;
	}
}

static int
adreno_recover_hang(struct kgsl_device *device,
			struct adreno_recovery_data *rec_data)
{
	int ret;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct adreno_ringbuffer *rb = &adreno_dev->ringbuffer;
	unsigned int timestamp;
	struct kgsl_context *context;
	struct adreno_context *adreno_context;

	KGSL_DRV_ERR(device,
	"Starting recovery from 3D GPU hang. Recovery parameters: IB1: 0x%X, "
	"Bad context_id: %u, global_eop: 0x%x\n", rec_data->ib1,
	rec_data->context_id, rec_data->global_eop);

	context = idr_find(&device->context_idr, rec_data->context_id);
	if (context == NULL) {
		KGSL_DRV_ERR(device, "Last context unknown id:%d\n",
				rec_data->context_id);
		rec_data->context_id = 0;
	} else {
		adreno_context = context->devctxt;
		adreno_context->flags |= CTXT_FLAGS_GPU_HANG;
	}
	/* Extract valid contents from rb which can still be executed after
	 * hang */
	ret = adreno_ringbuffer_extract(rb, rec_data);
	if (ret)
		goto done;

	timestamp = rb->timestamp;
	KGSL_DRV_ERR(device, "Last issued global timestamp: %x\n", timestamp);

	/* Make sure memory is synchronized before restarting the GPU */
	mb();

	/* restart device */
	ret = adreno_stop(device);
	if (ret)
		goto done;
	ret = adreno_start(device, true);
	if (ret)
		goto done;
	KGSL_DRV_ERR(device, "Device has been restarted after hang\n");

	/* Restore valid commands in ringbuffer */
	adreno_ringbuffer_restore(rb, rec_data->rb_buffer, rec_data->rb_size);
	rb->timestamp = timestamp;

	/* wait for idle */
	ret = adreno_idle(device);
done:
	kgsl_sharedmem_writel(&device->memstore,
			KGSL_DEVICE_MEMSTORE_OFFSET(eoptimestamp),
			rb->timestamp);
	adreno_mark_context_status(device, ret);
	return ret;
}

static void adreno_destroy_recovery_data(struct adreno_recovery_data *rec_data)
{
	vfree(rec_data->rb_buffer);
	vfree(rec_data->bad_rb_buffer);
}

static int adreno_setup_recovery_data(struct kgsl_device *device,
					struct adreno_recovery_data *rec_data)
{
	int ret = 0;
	unsigned int ib1_sz, ib2_sz;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct adreno_ringbuffer *rb = &adreno_dev->ringbuffer;

	memset(rec_data, 0, sizeof(*rec_data));

	adreno_regread(device, REG_CP_IB1_BUFSZ, &ib1_sz);
	adreno_regread(device, REG_CP_IB2_BUFSZ, &ib2_sz);
	if (ib1_sz || ib2_sz)
		adreno_regread(device, REG_CP_IB1_BASE, &rec_data->ib1);

	kgsl_sharedmem_readl(&device->memstore, &rec_data->context_id,
			KGSL_DEVICE_MEMSTORE_OFFSET(current_context));

	kgsl_sharedmem_readl(&device->memstore,
				&rec_data->global_eop,
				KGSL_DEVICE_MEMSTORE_OFFSET(eoptimestamp));

	rec_data->rb_buffer = vmalloc(rb->buffer_desc.size);
	if (!rec_data->rb_buffer) {
		KGSL_MEM_ERR(device, "vmalloc(%d) failed\n",
				rb->buffer_desc.size);
		return -ENOMEM;
	}

	rec_data->bad_rb_buffer = vmalloc(rb->buffer_desc.size);
	if (!rec_data->bad_rb_buffer) {
		KGSL_MEM_ERR(device, "vmalloc(%d) failed\n",
				rb->buffer_desc.size);
		ret = -ENOMEM;
		goto done;
	}

done:
	if (ret) {
		vfree(rec_data->rb_buffer);
		vfree(rec_data->bad_rb_buffer);
	}
	return ret;
}

int adreno_dump_and_recover(struct kgsl_device *device)
{
	int result = -ETIMEDOUT;
	struct adreno_recovery_data rec_data;

	if (device->state == KGSL_STATE_HUNG)
		goto done;
	if (device->state == KGSL_STATE_DUMP_AND_RECOVER) {
		mutex_unlock(&device->mutex);
		wait_for_completion(&device->recovery_gate);
		mutex_lock(&device->mutex);
		if (device->state != KGSL_STATE_HUNG)
			result = 0;
	} else {
		kgsl_pwrctrl_set_state(device, KGSL_STATE_DUMP_AND_RECOVER);
		INIT_COMPLETION(device->recovery_gate);
		/* Detected a hang */

		/* Get the recovery data as soon as hang is detected */
		result = adreno_setup_recovery_data(device, &rec_data);
		/*
		 * Trigger an automatic dump of the state to
		 * the console
		 */
		kgsl_postmortem_dump(device, 0);

		/*
		 * Make a GPU snapshot.  For now, do it after the PM dump so we
		 * can at least be sure the PM dump will work as it always has
		 */
		kgsl_device_snapshot(device, 1);

		result = adreno_recover_hang(device, &rec_data);
		adreno_destroy_recovery_data(&rec_data);
		if (result) {
			kgsl_pwrctrl_set_state(device, KGSL_STATE_HUNG);
		} else {
			kgsl_pwrctrl_set_state(device, KGSL_STATE_ACTIVE);
			mod_timer(&device->idle_timer, jiffies + FIRST_TIMEOUT);
		}
		complete_all(&device->recovery_gate);
	}
done:
	return result;
}
EXPORT_SYMBOL(adreno_dump_and_recover);

static int adreno_getproperty(struct kgsl_device *device,
				enum kgsl_property_type type,
				void *value,
				unsigned int sizebytes)
{
	int status = -EINVAL;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

	switch (type) {
	case KGSL_PROP_DEVICE_INFO:
		{
			struct kgsl_devinfo devinfo;

			if (sizebytes != sizeof(devinfo)) {
				status = -EINVAL;
				break;
			}

			memset(&devinfo, 0, sizeof(devinfo));
			devinfo.device_id = device->id+1;
			devinfo.chip_id = adreno_dev->chip_id;
			devinfo.mmu_enabled = kgsl_mmu_enabled();
			devinfo.gpu_id = adreno_dev->gpurev;
			devinfo.gmem_gpubaseaddr = adreno_dev->gmemspace.
					gpu_base;
			devinfo.gmem_sizebytes = adreno_dev->gmemspace.
					sizebytes;

			if (copy_to_user(value, &devinfo, sizeof(devinfo)) !=
					0) {
				status = -EFAULT;
				break;
			}
			status = 0;
		}
		break;
	case KGSL_PROP_DEVICE_SHADOW:
		{
			struct kgsl_shadowprop shadowprop;

			if (sizebytes != sizeof(shadowprop)) {
				status = -EINVAL;
				break;
			}
			memset(&shadowprop, 0, sizeof(shadowprop));
			if (device->memstore.hostptr) {
				/*NOTE: with mmu enabled, gpuaddr doesn't mean
				 * anything to mmap().
				 */
				shadowprop.gpuaddr = device->memstore.physaddr;
				shadowprop.size = device->memstore.size;
				/* GSL needs this to be set, even if it
				   appears to be meaningless */
				shadowprop.flags = KGSL_FLAGS_INITIALIZED;
			}
			if (copy_to_user(value, &shadowprop,
				sizeof(shadowprop))) {
				status = -EFAULT;
				break;
			}
			status = 0;
		}
		break;
	case KGSL_PROP_MMU_ENABLE:
		{
			int mmu_prop = kgsl_mmu_enabled();

			if (sizebytes != sizeof(int)) {
				status = -EINVAL;
				break;
			}
			if (copy_to_user(value, &mmu_prop, sizeof(mmu_prop))) {
				status = -EFAULT;
				break;
			}
			status = 0;
		}
		break;
	case KGSL_PROP_INTERRUPT_WAITS:
		{
			int int_waits = 1;
			if (sizebytes != sizeof(int)) {
				status = -EINVAL;
				break;
			}
			if (copy_to_user(value, &int_waits, sizeof(int))) {
				status = -EFAULT;
				break;
			}
			status = 0;
		}
		break;
	default:
		status = -EINVAL;
	}

	return status;
}

static inline void adreno_poke(struct kgsl_device *device)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	adreno_regwrite(device, REG_CP_RB_WPTR, adreno_dev->ringbuffer.wptr);
}

static int adreno_ringbuffer_drain(struct kgsl_device *device,
	unsigned int *regs)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct adreno_ringbuffer *rb = &adreno_dev->ringbuffer;
	unsigned long wait;
	unsigned long timeout = jiffies + msecs_to_jiffies(ADRENO_IDLE_TIMEOUT);

	if (!(rb->flags & KGSL_FLAGS_STARTED))
		return 0;

	/*
	 * The first time into the loop, wait for 100 msecs and kick wptr again
	 * to ensure that the hardware has updated correctly.  After that, kick
	 * it periodically every KGSL_TIMEOUT_PART msecs until the timeout
	 * expires
	 */

	wait = jiffies + msecs_to_jiffies(100);

	adreno_poke(device);

	do {
		if (time_after(jiffies, wait)) {
			adreno_poke(device);

			/* Check to see if the core is hung */
			if (adreno_hang_detect(device, regs))
				return -ETIMEDOUT;

			wait = jiffies + msecs_to_jiffies(KGSL_TIMEOUT_PART);
		}
		GSL_RB_GET_READPTR(rb, &rb->rptr);

		if (time_after(jiffies, timeout)) {
			KGSL_DRV_ERR(device, "rptr: %x, wptr: %x\n",
				rb->rptr, rb->wptr);
			return -ETIMEDOUT;
		}
	} while (rb->rptr != rb->wptr);

	return 0;
}

/* Caller must hold the device mutex. */
int adreno_idle(struct kgsl_device *device)
{
	unsigned int rbbm_status;
	unsigned long wait_time;
	unsigned long wait_time_part;
	unsigned int prev_reg_val[hang_detect_regs_count];

	memset(prev_reg_val, 0, sizeof(prev_reg_val));

	kgsl_cffdump_regpoll(device->id, REG_RBBM_STATUS << 2,
		0x00000000, 0x80000000);

retry:
	/* First, wait for the ringbuffer to drain */
	if (adreno_ringbuffer_drain(device, prev_reg_val))
		goto err;

	/* now, wait for the GPU to finish its operations */
	wait_time = jiffies + msecs_to_jiffies(ADRENO_IDLE_TIMEOUT);
	wait_time_part = jiffies + msecs_to_jiffies(KGSL_TIMEOUT_PART);

	while (time_before(jiffies, wait_time)) {
		adreno_regread(device, REG_RBBM_STATUS, &rbbm_status);
		if (rbbm_status == 0x110)
			return 0;

		/* Dont wait for timeout, detect hang faster.
		 */
		if (time_after(jiffies, wait_time_part)) {
				wait_time_part = jiffies +
					msecs_to_jiffies(KGSL_TIMEOUT_PART);
				if ((adreno_hang_detect(device, prev_reg_val)))
					goto err;
		}

	}

err:
	KGSL_DRV_ERR(device, "spun too long waiting for RB to idle\n");
	if (KGSL_STATE_DUMP_AND_RECOVER != device->state &&
		!adreno_dump_and_recover(device)) {
		wait_time = jiffies + msecs_to_jiffies(ADRENO_IDLE_TIMEOUT);
		goto retry;
	}
	return -ETIMEDOUT;
}

static unsigned int adreno_isidle(struct kgsl_device *device)
{
	int status = false;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct adreno_ringbuffer *rb = &adreno_dev->ringbuffer;
	unsigned int rbbm_status;

	WARN_ON(device->state == KGSL_STATE_INIT);
	/* If the device isn't active, don't force it on. */
	if (device->state == KGSL_STATE_ACTIVE) {
		/* Is the ring buffer is empty? */
		GSL_RB_GET_READPTR(rb, &rb->rptr);
		if (!device->active_cnt && (rb->rptr == rb->wptr)) {
			/* Is the core idle? */
			adreno_regread(device, REG_RBBM_STATUS,
					    &rbbm_status);
			if (rbbm_status == 0x110)
				status = true;
		}
	} else {
		status = true;
	}
	return status;
}

/* Caller must hold the device mutex. */
static int adreno_suspend_context(struct kgsl_device *device)
{
	int status = 0;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

	/* switch to NULL ctxt */
	if (adreno_dev->drawctxt_active != NULL) {
		adreno_drawctxt_switch(adreno_dev, NULL, 0);
		status = adreno_idle(device);
	}

	return status;
}

struct kgsl_memdesc *adreno_find_region(struct kgsl_device *device,
						unsigned int pt_base,
						unsigned int gpuaddr,
						unsigned int size)
{
	struct kgsl_memdesc *result = NULL;
	struct kgsl_mem_entry *entry;
	struct kgsl_process_private *priv;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct adreno_ringbuffer *ringbuffer = &adreno_dev->ringbuffer;
	struct kgsl_context *context;
	int next = 0;

	if (kgsl_gpuaddr_in_memdesc(&ringbuffer->buffer_desc, gpuaddr, size))
		return &ringbuffer->buffer_desc;

	if (kgsl_gpuaddr_in_memdesc(&ringbuffer->memptrs_desc, gpuaddr, size))
		return &ringbuffer->memptrs_desc;

	if (kgsl_gpuaddr_in_memdesc(&device->memstore, gpuaddr, size))
		return &device->memstore;

	mutex_lock(&kgsl_driver.process_mutex);
	list_for_each_entry(priv, &kgsl_driver.process_list, list) {
		if (!kgsl_mmu_pt_equal(priv->pagetable, pt_base))
			continue;
		spin_lock(&priv->mem_lock);
		entry = kgsl_sharedmem_find_region(priv, gpuaddr, size);
		if (entry) {
			result = &entry->memdesc;
			spin_unlock(&priv->mem_lock);
			mutex_unlock(&kgsl_driver.process_mutex);
			return result;
		}
		spin_unlock(&priv->mem_lock);
	}
	mutex_unlock(&kgsl_driver.process_mutex);

	while (1) {
		struct adreno_context *adreno_context = NULL;
		context = idr_get_next(&device->context_idr, &next);
		if (context == NULL)
			break;

		adreno_context = (struct adreno_context *)context->devctxt;

		if (kgsl_mmu_pt_equal(adreno_context->pagetable, pt_base)) {
			struct kgsl_memdesc *desc;

			desc = &adreno_context->gpustate;
			if (kgsl_gpuaddr_in_memdesc(desc, gpuaddr, size)) {
				result = desc;
				return result;
			}

			desc = &adreno_context->context_gmem_shadow.gmemshadow;
			if (kgsl_gpuaddr_in_memdesc(desc, gpuaddr, size)) {
				result = desc;
				return result;
			}
		}
		next = next + 1;
	}

	return NULL;

}

uint8_t *adreno_convertaddr(struct kgsl_device *device, unsigned int pt_base,
			    unsigned int gpuaddr, unsigned int size)
{
	struct kgsl_memdesc *memdesc;

	memdesc = adreno_find_region(device, pt_base, gpuaddr, size);

	return memdesc ? kgsl_gpuaddr_to_vaddr(memdesc, gpuaddr) : NULL;
}

void adreno_regread(struct kgsl_device *device, unsigned int offsetwords,
				unsigned int *value)
{
	unsigned int *reg;
	BUG_ON(offsetwords*sizeof(uint32_t) >= device->regspace.sizebytes);
	reg = (unsigned int *)(device->regspace.mmio_virt_base
				+ (offsetwords << 2));

	if (!in_interrupt())
		kgsl_pre_hwaccess(device);

	/*ensure this read finishes before the next one.
	 * i.e. act like normal readl() */
	*value = __raw_readl(reg);
	rmb();
}

void adreno_regwrite(struct kgsl_device *device, unsigned int offsetwords,
				unsigned int value)
{
	unsigned int *reg;

	BUG_ON(offsetwords*sizeof(uint32_t) >= device->regspace.sizebytes);

	if (!in_interrupt())
		kgsl_pre_hwaccess(device);

	kgsl_cffdump_regwrite(device->id, offsetwords << 2, value);
	reg = (unsigned int *)(device->regspace.mmio_virt_base
				+ (offsetwords << 2));

	/*ensure previous writes post before this one,
	 * i.e. act like normal writel() */
	wmb();
	__raw_writel(value, reg);
}

static int kgsl_check_interrupt_timestamp(struct kgsl_device *device,
					unsigned int timestamp)
{
	int status;
	unsigned int ref_ts, enableflag;

	status = kgsl_check_timestamp(device, timestamp);
	if (!status) {
		mutex_lock(&device->mutex);
		kgsl_sharedmem_readl(&device->memstore, &enableflag,
			KGSL_DEVICE_MEMSTORE_OFFSET(ts_cmp_enable));
		mb();

		if (enableflag) {
			kgsl_sharedmem_readl(&device->memstore, &ref_ts,
				KGSL_DEVICE_MEMSTORE_OFFSET(ref_wait_ts));
			mb();
			if (timestamp_cmp(ref_ts, timestamp) >= 0) {
				kgsl_sharedmem_writel(&device->memstore,
				KGSL_DEVICE_MEMSTORE_OFFSET(ref_wait_ts),
				timestamp);
				wmb();
			}
		} else {
			unsigned int cmds[2];
			kgsl_sharedmem_writel(&device->memstore,
				KGSL_DEVICE_MEMSTORE_OFFSET(ref_wait_ts),
				timestamp);
			enableflag = 1;
			kgsl_sharedmem_writel(&device->memstore,
				KGSL_DEVICE_MEMSTORE_OFFSET(ts_cmp_enable),
				enableflag);
			wmb();
			/* submit a dummy packet so that even if all
			* commands upto timestamp get executed we will still
			* get an interrupt */
			cmds[0] = cp_type3_packet(CP_NOP, 1);
			cmds[1] = 0;

			adreno_ringbuffer_issuecmds(device,
				NULL, KGSL_CMD_FLAGS_NONE,
				&cmds[0], 2);
		}
		mutex_unlock(&device->mutex);
	}

	return status;
}

/*
 wait_event_interruptible_timeout checks for the exit condition before
 placing a process in wait q. For conditional interrupts we expect the
 process to already be in its wait q when its exit condition checking
 function is called.
*/
#define kgsl_wait_event_interruptible_timeout(wq, condition, timeout, io)\
({									\
	long __ret = timeout;						\
	if (io)						\
		__wait_io_event_interruptible_timeout(wq, condition, __ret);\
	else						\
		__wait_event_interruptible_timeout(wq, condition, __ret);\
	__ret;								\
})



unsigned int adreno_hang_detect(struct kgsl_device *device,
						unsigned int *prev_reg_val)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	unsigned int curr_reg_val[hang_detect_regs_count];
	unsigned int hang_detected = 1;
	unsigned int i;

	if (!adreno_dev->fast_hang_detect)
		return 0;

	for (i = 0; i < hang_detect_regs_count; i++) {
		adreno_regread(device, hang_detect_regs[i],
					   &curr_reg_val[i]);
		if (curr_reg_val[i] != prev_reg_val[i]) {
			prev_reg_val[i] = curr_reg_val[i];
			hang_detected = 0;
		}
	}

	return hang_detected;
}


/* MUST be called with the device mutex held */
static int adreno_waittimestamp(struct kgsl_device *device,
				unsigned int timestamp,
				unsigned int msecs)
{
	long status = 0;
	uint io = 1;
	static uint io_cnt;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;
	int retries = 0;
	unsigned int time_elapsed = 0;
	unsigned int prev_reg_val[hang_detect_regs_count];
	unsigned int wait;

	memset(prev_reg_val, 0, sizeof(prev_reg_val));

	/* Don't wait forever, set a max value for now */
	if (msecs == KGSL_TIMEOUT_DEFAULT)
		msecs = adreno_dev->wait_timeout;

	if (timestamp_cmp(timestamp, adreno_dev->ringbuffer.timestamp) > 0) {
		KGSL_DRV_ERR(device, "Cannot wait for invalid ts: %x, "
			"rb->timestamp: %x\n",
			timestamp, adreno_dev->ringbuffer.timestamp);
		status = -EINVAL;
		goto done;
	}

	/*
	 * Make the first timeout interval 100 msecs and then try to kick the
	 * wptr again.  This helps to ensure the wptr is updated properly.  If
	 * the requested timeout is less than 100 msecs, then wait 20msecs which
	 * is the minimum amount of time we can safely wait at 100HZ
	 */

	if (msecs == 0 || msecs >= 100)
		wait = 100;
	else
		wait = 20;

	do {
		if (kgsl_check_timestamp(device, timestamp)) {
			/* if the timestamp happens while we're not
			 * waiting, there's a chance that an interrupt
			 * will not be generated and thus the timestamp
			 * work needs to be queued.
			 */
			queue_work(device->work_queue, &device->ts_expired_ws);
			status = 0;
			goto done;
		}
		adreno_poke(device);
		io_cnt = (io_cnt + 1) % 100;
		if (io_cnt <
		    pwr->pwrlevels[pwr->active_pwrlevel].io_fraction)
			io = 0;

		if ((retries > 0) &&
			(adreno_hang_detect(device, prev_reg_val)))
			goto hang_dump;

		mutex_unlock(&device->mutex);
		/* We need to make sure that the process is
		 * placed in wait-q before its condition is called
		 */
		status = kgsl_wait_event_interruptible_timeout(
				device->wait_queue,
				kgsl_check_interrupt_timestamp(device,
					timestamp),
				msecs_to_jiffies(wait), io);

		mutex_lock(&device->mutex);

		if (status > 0) {
			/*completed before the wait finished */
			status = 0;
			goto done;
		} else if (status < 0) {
			/*an error occurred*/
			goto done;
		}
		/*this wait timed out*/

		time_elapsed += wait;
		wait = KGSL_TIMEOUT_PART;

		retries++;

	} while (!msecs || time_elapsed < msecs);

hang_dump:
	/* Check if timestamp has retired here because we may have hit
	 * recovery which can take some time and cause waiting threads
	 * to timeout
	 */
	if (kgsl_check_timestamp(device, timestamp))
		goto done;
	status = -ETIMEDOUT;
	KGSL_DRV_ERR(device,
		     "Device hang detected while waiting for timestamp: %x,"
		      "last submitted(rb->timestamp): %x, wptr: %x\n",
		      timestamp, adreno_dev->ringbuffer.timestamp,
		      adreno_dev->ringbuffer.wptr);
	if (!adreno_dump_and_recover(device)) {
		/* The timestamp that this process wanted
		 * to wait on may be invalid or expired now
		 * after successful recovery */
			status = 0;
	}
done:
	return (int)status;
}

static unsigned int adreno_readtimestamp(struct kgsl_device *device,
			     enum kgsl_timestamp_type type)
{
	unsigned int timestamp = 0;

	if (type == KGSL_TIMESTAMP_CONSUMED)
		adreno_regread(device, REG_CP_TIMESTAMP, &timestamp);
	else if (type == KGSL_TIMESTAMP_RETIRED)
		kgsl_sharedmem_readl(&device->memstore, &timestamp,
				 KGSL_DEVICE_MEMSTORE_OFFSET(eoptimestamp));
	rmb();

	return timestamp;
}

static long adreno_ioctl(struct kgsl_device_private *dev_priv,
			      unsigned int cmd, void *data)
{
	int result = 0;
	struct kgsl_drawctxt_set_bin_base_offset *binbase;
	struct kgsl_context *context;

	switch (cmd) {
	case IOCTL_KGSL_DRAWCTXT_SET_BIN_BASE_OFFSET:
		binbase = data;

		context = kgsl_find_context(dev_priv, binbase->drawctxt_id);
		if (context) {
			adreno_drawctxt_set_bin_base_offset(
				dev_priv->device, context, binbase->offset);
		} else {
			result = -EINVAL;
			KGSL_DRV_ERR(dev_priv->device,
				"invalid drawctxt drawctxt_id %d "
				"device_id=%d\n",
				binbase->drawctxt_id, dev_priv->device->id);
		}
		break;

	default:
		KGSL_DRV_INFO(dev_priv->device,
			"invalid ioctl code %08x\n", cmd);
		result = -EINVAL;
		break;
	}
	return result;

}

static inline s64 adreno_ticks_to_us(u32 ticks, u32 gpu_freq)
{
	gpu_freq /= 1000000;
	return ticks / gpu_freq;
}

static void adreno_power_stats(struct kgsl_device *device,
				struct kgsl_power_stats *stats)
{
	unsigned int reg;
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;

	/* In order to calculate idle you have to have run the algorithm *
	 * at least once to get a start time. */
	if (pwr->time != 0) {
		s64 tmp;
		/* Stop the performance moniter and read the current *
		 * busy cycles. */
		adreno_regwrite(device,
			REG_CP_PERFMON_CNTL,
			REG_PERF_MODE_CNT |
			REG_PERF_STATE_FREEZE);
		adreno_regread(device, REG_RBBM_PERFCOUNTER1_LO, &reg);
		tmp = ktime_to_us(ktime_get());
		stats->total_time = tmp - pwr->time;
		pwr->time = tmp;
		stats->busy_time = adreno_ticks_to_us(reg, device->pwrctrl.
				pwrlevels[device->pwrctrl.active_pwrlevel].
				gpu_freq);

		adreno_regwrite(device,
			REG_CP_PERFMON_CNTL,
			REG_PERF_MODE_CNT |
			REG_PERF_STATE_RESET);
	} else {
		stats->total_time = 0;
		stats->busy_time = 0;
		pwr->time = ktime_to_us(ktime_get());
	}

	/* re-enable the performance moniters */
	adreno_regread(device, REG_RBBM_PM_OVERRIDE2, &reg);
	adreno_regwrite(device, REG_RBBM_PM_OVERRIDE2, (reg | 0x40));
	adreno_regwrite(device, REG_RBBM_PERFCOUNTER1_SELECT, 0x1);
	adreno_regwrite(device,
		REG_CP_PERFMON_CNTL,
		REG_PERF_MODE_CNT | REG_PERF_STATE_ENABLE);
}

void adreno_irqctrl(struct kgsl_device *device, int state)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	adreno_dev->gpudev->irq_control(adreno_dev, state);
}

static unsigned int adreno_gpuid(struct kgsl_device *device)
{
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

	/* Standard KGSL gpuid format:
	 * top word is 0x0002 for 2D or 0x0003 for 3D
	 * Bottom word is core specific identifer
	 */

	return (0x0003 << 16) | ((int) adreno_dev->gpurev);
}

static const struct kgsl_functable adreno_functable = {
	/* Mandatory functions */
	.regread = adreno_regread,
	.regwrite = adreno_regwrite,
	.idle = adreno_idle,
	.isidle = adreno_isidle,
	.suspend_context = adreno_suspend_context,
	.start = adreno_start,
	.stop = adreno_stop,
	.getproperty = adreno_getproperty,
	.waittimestamp = adreno_waittimestamp,
	.readtimestamp = adreno_readtimestamp,
	.issueibcmds = adreno_ringbuffer_issueibcmds,
	.ioctl = adreno_ioctl,
	.setup_pt = adreno_setup_pt,
	.cleanup_pt = adreno_cleanup_pt,
	.power_stats = adreno_power_stats,
	.irqctrl = adreno_irqctrl,
	.gpuid = adreno_gpuid,
	.snapshot = adreno_snapshot,
	/* Optional functions */
	.setstate = adreno_setstate,
	.drawctxt_create = adreno_drawctxt_create,
	.drawctxt_destroy = adreno_drawctxt_destroy,
	.postmortem_dump = adreno_dump,
};

static struct platform_device_id adreno_id_table[] = {
	{ DEVICE_3D0_NAME, (kernel_ulong_t)&device_3d0.dev, },
	{ },
};
MODULE_DEVICE_TABLE(platform, adreno_id_table);

static struct platform_driver adreno_platform_driver = {
	.probe = adreno_probe,
	.remove = __devexit_p(adreno_remove),
	.suspend = kgsl_suspend_driver,
	.resume = kgsl_resume_driver,
	.id_table = adreno_id_table,
	.driver = {
		.owner = THIS_MODULE,
		.name = DEVICE_3D_NAME,
		.pm = &kgsl_pm_ops,
	}
};

static int __init kgsl_3d_init(void)
{
	return platform_driver_register(&adreno_platform_driver);
}

static void __exit kgsl_3d_exit(void)
{
	platform_driver_unregister(&adreno_platform_driver);
}

module_init(kgsl_3d_init);
module_exit(kgsl_3d_exit);

MODULE_DESCRIPTION("3D Graphics driver");
MODULE_VERSION("1.2");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:kgsl_3d");
