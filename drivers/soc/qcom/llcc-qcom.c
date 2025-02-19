// SPDX-License-Identifier: GPL-2.0
/*
drivers/soc/qcom/llcc-slice.c
 * Copyright (c) 2017-2021, The Linux Foundation. All rights reserved.
drivers/soc/qcom/llcc-qcom.c
 *
 */

#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/soc/qcom/llcc-tcm.h>

#define ACTIVATE                      BIT(0)
#define DEACTIVATE                    BIT(1)
#define ACT_CTRL_OPCODE_ACTIVATE      BIT(0)
#define ACT_CTRL_OPCODE_DEACTIVATE    BIT(1)
#define ACT_CTRL_ACT_TRIG             BIT(0)
#define ACT_CTRL_OPCODE_SHIFT         0x01
#define ATTR1_PROBE_TARGET_WAYS_SHIFT 0x02
#define ATTR1_FIXED_SIZE_SHIFT        0x03
#define ATTR1_PRIORITY_SHIFT          0x04
#define ATTR1_MAX_CAP_SHIFT           0x10
#define ATTR0_RES_WAYS_MASK           GENMASK(11, 0)
#define ATTR0_BONUS_WAYS_MASK         GENMASK(27, 16)
#define ATTR0_BONUS_WAYS_SHIFT        0x10
#define LLCC_STATUS_READ_DELAY        100

#define CACHE_LINE_SIZE_SHIFT         6

#define LLCC_COMMON_STATUS0           0x0003000c
#define LLCC_LB_CNT_MASK              GENMASK(31, 28)
#define LLCC_LB_CNT_SHIFT             28

#define MAX_CAP_TO_BYTES(n)           (n * SZ_1K)
#define LLCC_TRP_ACT_CTRLn(n)         (n * SZ_4K)
#define LLCC_TRP_STATUSn(n)           (4 + n * SZ_4K)
#define LLCC_TRP_ATTR0_CFGn(n)        (0x21000 + SZ_8 * n)
#define LLCC_TRP_ATTR1_CFGn(n)        (0x21004 + SZ_8 * n)

#define LLCC_TRP_C_AS_NC	      0x22890
#define LLCC_TRP_NC_AS_C	      0x22894
#define LLCC_FEAC_C_AS_NC	      0x35030
#define LLCC_FEAC_NC_AS_C	      0x35034
#define LLCC_TRP_WRSC_EN              0x21F20
#define LLCC_WRSC_SCID_EN(n)          BIT(n)

#define LLCC_TRP_PCB_ACT	      0x21F04
#define LLCC_TRP_SCID_DIS_CAP_ALLOC   0x21F00

#define BANK_OFFSET_STRIDE            0x80000

static struct llcc_slice_config sdm845_data[] =  {
	{ LLCC_CPUSS,    1,  2816, 1, 0, 0xffc, 0x2,   0, 0, 1, 1, 1 },
	{ LLCC_VIDSC0,   2,  512,  2, 1, 0x0,   0x0f0, 0, 0, 1, 1, 0 },
	{ LLCC_VIDSC1,   3,  512,  2, 1, 0x0,   0x0f0, 0, 0, 1, 1, 0 },
	{ LLCC_ROTATOR,  4,  563,  2, 1, 0x0,   0x00e, 2, 0, 1, 1, 0 },
	{ LLCC_VOICE,    5,  2816, 1, 0, 0xffc, 0x2,   0, 0, 1, 1, 0 },
	{ LLCC_AUDIO,    6,  2816, 1, 0, 0xffc, 0x2,   0, 0, 1, 1, 0 },
	{ LLCC_MDMHPGRW, 7,  1024, 2, 0, 0xfc,  0xf00, 0, 0, 1, 1, 0 },
	{ LLCC_MDM,      8,  2816, 1, 0, 0xffc, 0x2,   0, 0, 1, 1, 0 },
	{ LLCC_CMPT,     10, 2816, 1, 0, 0xffc, 0x2,   0, 0, 1, 1, 0 },
	{ LLCC_GPUHTW,   11, 512,  1, 1, 0xc,   0x0,   0, 0, 1, 1, 0 },
	{ LLCC_GPU,      12, 2304, 1, 0, 0xff0, 0x2,   0, 0, 1, 1, 0 },
	{ LLCC_MMUHWT,   13, 256,  2, 0, 0x0,   0x1,   0, 0, 1, 0, 1 },
	{ LLCC_CMPTDMA,  15, 2816, 1, 0, 0xffc, 0x2,   0, 0, 1, 1, 0 },
	{ LLCC_DISP,     16, 2816, 1, 0, 0xffc, 0x2,   0, 0, 1, 1, 0 },
	{ LLCC_VIDFW,    17, 2816, 1, 0, 0xffc, 0x2,   0, 0, 1, 1, 0 },
	{ LLCC_MDMHPFX,  20, 1024, 2, 1, 0x0,   0xf00, 0, 0, 1, 1, 0 },
	{ LLCC_MDMPNG,   21, 1024, 0, 1, 0x1e,  0x0,   0, 0, 1, 1, 0 },
	{ LLCC_AUDHW,    22, 1024, 1, 1, 0xffc, 0x2,   0, 0, 1, 1, 0 },
};

static struct llcc_drv_data *drv_data = (void *) -EPROBE_DEFER;

static struct regmap_config llcc_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.fast_io = true,
};

/**
 * llcc_slice_getd - get llcc slice descriptor
 * @uid: usecase_id for the client
 *
 * A pointer to llcc slice descriptor will be returned on success and
 * and error pointer is returned on failure
 */
struct llcc_slice_desc *llcc_slice_getd(u32 uid)
{
	const struct llcc_slice_config *cfg;
	struct llcc_slice_desc *desc;
	u32 sz, count;

	if (IS_ERR(drv_data))
		return ERR_CAST(drv_data);

	cfg = drv_data->cfg;
	sz = drv_data->cfg_size;

	for (count = 0; cfg && count < sz; count++, cfg++)
		if (cfg->usecase_id == uid)
			break;

	if (count == sz || !cfg)
		return ERR_PTR(-ENODEV);

	desc = kzalloc(sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return ERR_PTR(-ENOMEM);

	desc->slice_id = cfg->slice_id;
	desc->slice_size = cfg->max_cap;

	return desc;
}
EXPORT_SYMBOL_GPL(llcc_slice_getd);

/**
 * llcc_slice_putd - llcc slice descritpor
 * @desc: Pointer to llcc slice descriptor
 */
void llcc_slice_putd(struct llcc_slice_desc *desc)
{
	if (!IS_ERR_OR_NULL(desc))
		kfree(desc);
}
EXPORT_SYMBOL_GPL(llcc_slice_putd);

static int llcc_update_act_ctrl(u32 sid,
				u32 act_ctrl_reg_val, u32 status)
{
	u32 act_ctrl_reg;
	u32 status_reg;
	u32 slice_status;
	int ret;

	if (IS_ERR(drv_data))
		return PTR_ERR(drv_data);

	act_ctrl_reg = LLCC_TRP_ACT_CTRLn(sid);
	status_reg = LLCC_TRP_STATUSn(sid);

	/* Set the ACTIVE trigger */
	act_ctrl_reg_val |= ACT_CTRL_ACT_TRIG;
	ret = regmap_write(drv_data->bcast_regmap, act_ctrl_reg,
				act_ctrl_reg_val);
	if (ret)
		return ret;

	/* Clear the ACTIVE trigger */
	act_ctrl_reg_val &= ~ACT_CTRL_ACT_TRIG;
	ret = regmap_write(drv_data->bcast_regmap, act_ctrl_reg,
				act_ctrl_reg_val);
	if (ret)
		return ret;

	ret = regmap_read_poll_timeout(drv_data->bcast_regmap, status_reg,
				      slice_status, !(slice_status & status),
				      0, LLCC_STATUS_READ_DELAY);
	return ret;
}

/**
 * llcc_slice_activate - Activate the llcc slice
 * @desc: Pointer to llcc slice descriptor
 *
 * A value of zero will be returned on success and a negative errno will
 * be returned in error cases
 */
int llcc_slice_activate(struct llcc_slice_desc *desc)
{
	int ret;
	u32 act_ctrl_val;

	if (IS_ERR(drv_data))
		return PTR_ERR(drv_data);

	if (IS_ERR_OR_NULL(desc))
		return -EINVAL;

	mutex_lock(&drv_data->lock);
	if (test_bit(desc->slice_id, drv_data->bitmap)) {
		mutex_unlock(&drv_data->lock);
		return 0;
	}

	act_ctrl_val = ACT_CTRL_OPCODE_ACTIVATE << ACT_CTRL_OPCODE_SHIFT;

	ret = llcc_update_act_ctrl(desc->slice_id, act_ctrl_val,
				  DEACTIVATE);
	if (ret) {
		mutex_unlock(&drv_data->lock);
		return ret;
	}

	__set_bit(desc->slice_id, drv_data->bitmap);
	mutex_unlock(&drv_data->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(llcc_slice_activate);

/**
 * llcc_slice_deactivate - Deactivate the llcc slice
 * @desc: Pointer to llcc slice descriptor
 *
 * A value of zero will be returned on success and a negative errno will
 * be returned in error cases
 */
int llcc_slice_deactivate(struct llcc_slice_desc *desc)
{
	u32 act_ctrl_val;
	int ret;

	if (IS_ERR(drv_data))
		return PTR_ERR(drv_data);

	if (IS_ERR_OR_NULL(desc))
		return -EINVAL;

	mutex_lock(&drv_data->lock);
	if (!test_bit(desc->slice_id, drv_data->bitmap)) {
		mutex_unlock(&drv_data->lock);
		return 0;
	}
	act_ctrl_val = ACT_CTRL_OPCODE_DEACTIVATE << ACT_CTRL_OPCODE_SHIFT;

	ret = llcc_update_act_ctrl(desc->slice_id, act_ctrl_val,
				  ACTIVATE);
	if (ret) {
		mutex_unlock(&drv_data->lock);
		return ret;
	}

	__clear_bit(desc->slice_id, drv_data->bitmap);
	mutex_unlock(&drv_data->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(llcc_slice_deactivate);

/**
 * llcc_get_slice_id - return the slice id
 * @desc: Pointer to llcc slice descriptor
 */
int llcc_get_slice_id(struct llcc_slice_desc *desc)
{
	if (IS_ERR_OR_NULL(desc))
		return -EINVAL;

	return desc->slice_id;
}
EXPORT_SYMBOL_GPL(llcc_get_slice_id);

/**
 * llcc_get_slice_size - return the slice id
 * @desc: Pointer to llcc slice descriptor
 */
size_t llcc_get_slice_size(struct llcc_slice_desc *desc)
{
	if (IS_ERR_OR_NULL(desc))
		return 0;

	return desc->slice_size;
}
EXPORT_SYMBOL_GPL(llcc_get_slice_size);

static int qcom_llcc_cfg_program(struct platform_device *pdev)
{
	int i;
	u32 attr1_cfg;
	u32 attr0_cfg;
	u32 attr1_val;
	u32 attr0_val;
	u32 max_cap_cacheline;
	u32 sz;
	u32 pcb = 0;
	u32 cad = 0;
	u32 wren = 0;
	int ret = 0;
	const struct llcc_slice_config *llcc_table;
	struct llcc_slice_desc desc;
	bool cap_based_alloc_and_pwr_collapse =
		drv_data->cap_based_alloc_and_pwr_collapse;
	int v2_ver = of_device_is_compatible(pdev->dev.of_node,
							 "qcom,llcc-v2");

	sz = drv_data->cfg_size;
	llcc_table = drv_data->cfg;

	for (i = 0; i < sz; i++) {
		attr1_cfg = LLCC_TRP_ATTR1_CFGn(llcc_table[i].slice_id);
		attr0_cfg = LLCC_TRP_ATTR0_CFGn(llcc_table[i].slice_id);

		attr1_val = llcc_table[i].cache_mode;
		attr1_val |= llcc_table[i].probe_target_ways <<
				ATTR1_PROBE_TARGET_WAYS_SHIFT;
		attr1_val |= llcc_table[i].fixed_size <<
				ATTR1_FIXED_SIZE_SHIFT;
		attr1_val |= llcc_table[i].priority <<
				ATTR1_PRIORITY_SHIFT;

		max_cap_cacheline = MAX_CAP_TO_BYTES(llcc_table[i].max_cap);

		/* LLCC instances can vary for each target.
		 * The SW writes to broadcast register which gets propagated
		 * to each llcc instace (llcc0,.. llccN).
		 * Since the size of the memory is divided equally amongst the
		 * llcc instances, we need to configure the max cap accordingly.
		 */
		max_cap_cacheline = max_cap_cacheline / drv_data->num_banks;
		max_cap_cacheline >>= CACHE_LINE_SIZE_SHIFT;
		attr1_val |= max_cap_cacheline << ATTR1_MAX_CAP_SHIFT;

		attr0_val = llcc_table[i].res_ways & ATTR0_RES_WAYS_MASK;
		attr0_val |= llcc_table[i].bonus_ways << ATTR0_BONUS_WAYS_SHIFT;

		ret = regmap_write(drv_data->bcast_regmap, attr1_cfg,
					attr1_val);
		if (ret)
			return ret;
		ret = regmap_write(drv_data->bcast_regmap, attr0_cfg,
					attr0_val);
		if (ret)
			return ret;

		if (v2_ver) {
			wren |= llcc_table[i].write_scid_en <<
						llcc_table[i].slice_id;
			ret = regmap_write(drv_data->bcast_regmap,
				LLCC_TRP_WRSC_EN, wren);
			if (ret)
				return ret;
		}

		if (cap_based_alloc_and_pwr_collapse) {
			cad |= llcc_table[i].dis_cap_alloc <<
				llcc_table[i].slice_id;
			ret = regmap_write(drv_data->bcast_regmap,
					LLCC_TRP_SCID_DIS_CAP_ALLOC, cad);
			if (ret)
				return ret;

			pcb |= llcc_table[i].retain_on_pc <<
					llcc_table[i].slice_id;
			ret = regmap_write(drv_data->bcast_regmap,
						LLCC_TRP_PCB_ACT, pcb);
			if (ret)
				return ret;
		}

		if (llcc_table[i].activate_on_init) {
			desc.slice_id = llcc_table[i].slice_id;
			ret = llcc_slice_activate(&desc);
		}
	}
	return ret;
}

static int qcom_llcc_remove(struct platform_device *pdev)
{
	/* Set the global pointer to a error code to avoid referencing it */
	drv_data = ERR_PTR(-ENODEV);
	return 0;
}

static struct regmap *qcom_llcc_init_mmio(struct platform_device *pdev,
		const char *name)
{
	struct resource *res;
	void __iomem *base;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!res)
		return ERR_PTR(-ENODEV);

	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base))
		return ERR_CAST(base);

	llcc_regmap_config.name = name;
	return devm_regmap_init_mmio(&pdev->dev, base, &llcc_regmap_config);
}

static int qcom_llcc_probe(struct platform_device *pdev,
			   const struct llcc_slice_config *llcc_cfg, u32 sz)
{
	u32 num_banks;
	struct device *dev = &pdev->dev;
	int ret, i;
	struct platform_device *llcc_edac, *llcc_perfmon;
	struct device_node *tcm_memory_node;

	if (!IS_ERR(drv_data))
		return -EBUSY;

	drv_data = devm_kzalloc(dev, sizeof(*drv_data), GFP_KERNEL);
	if (!drv_data) {
		ret = -ENOMEM;
		goto err;
	}

	drv_data->regmap = qcom_llcc_init_mmio(pdev, "llcc_base");
	if (IS_ERR(drv_data->regmap)) {
		ret = PTR_ERR(drv_data->regmap);
		goto err;
	}

	drv_data->bcast_regmap =
		qcom_llcc_init_mmio(pdev, "llcc_broadcast_base");

	if (PTR_ERR(drv_data->bcast_regmap) == -ENODEV)
		drv_data->bcast_regmap = drv_data->regmap;
	else if (IS_ERR(drv_data->bcast_regmap)) {
		ret = PTR_ERR(drv_data->bcast_regmap);
		goto err;
	}

	ret = regmap_read(drv_data->regmap, LLCC_COMMON_STATUS0,
						&num_banks);
	if (ret)
		goto err;

	num_banks &= LLCC_LB_CNT_MASK;
	num_banks >>= LLCC_LB_CNT_SHIFT;
	drv_data->num_banks = num_banks;

	for (i = 0; i < sz; i++)
		if (llcc_cfg[i].slice_id > drv_data->max_slices)
			drv_data->max_slices = llcc_cfg[i].slice_id;

	drv_data->offsets = devm_kcalloc(dev, num_banks, sizeof(u32),
							GFP_KERNEL);
	if (!drv_data->offsets) {
		ret = -ENOMEM;
		goto err;
	}

	drv_data->cap_based_alloc_and_pwr_collapse =
		of_property_read_bool(pdev->dev.of_node,
				      "cap-based-alloc-and-pwr-collapse");

	for (i = 0; i < num_banks; i++)
		drv_data->offsets[i] = i * BANK_OFFSET_STRIDE;

	drv_data->bitmap = devm_kcalloc(dev,
	BITS_TO_LONGS(drv_data->max_slices), sizeof(unsigned long),
						GFP_KERNEL);
	if (!drv_data->bitmap) {
		ret = -ENOMEM;
		goto err;
	}

	drv_data->cfg = llcc_cfg;
	drv_data->cfg_size = sz;
	mutex_init(&drv_data->lock);
	platform_set_drvdata(pdev, drv_data);

	ret = qcom_llcc_cfg_program(pdev);
	if (ret) {
		pr_err("llcc configuration failed!!\n");
		goto err;
	}

	drv_data->ecc_irq = platform_get_irq(pdev, 0);
	llcc_edac = platform_device_register_data(&pdev->dev,
					"qcom_llcc_edac", -1, drv_data,
					sizeof(*drv_data));
	if (IS_ERR(llcc_edac))
		dev_err(dev, "Failed to register llcc edac driver\n");

	llcc_perfmon = platform_device_register_data(&pdev->dev,
					"qcom_llcc_perfmon", -1,
					drv_data, sizeof(*drv_data));
	if (IS_ERR(llcc_perfmon))
		dev_err(dev, "Failed to register llcc perfmon device\n");

	tcm_memory_node = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (tcm_memory_node) {
		ret = qcom_llcc_tcm_probe(pdev, llcc_cfg, sz, tcm_memory_node);
		if (ret) {
			dev_err(dev, "Failed to probe TCM manager\n");
			goto err_dereg;
		}
	}

	return 0;

err_dereg:
	platform_device_unregister(llcc_edac);
	platform_device_unregister(llcc_perfmon);
err:
	drv_data = ERR_PTR(-ENODEV);
	return ret;
}

static int sdm845_qcom_llcc_remove(struct platform_device *pdev)
{
	return qcom_llcc_remove(pdev);
}

static int sdm845_qcom_llcc_probe(struct platform_device *pdev)
{
	return qcom_llcc_probe(pdev, sdm845_data, ARRAY_SIZE(sdm845_data));
}

static const struct of_device_id sdm845_qcom_llcc_of_match[] = {
	{ .compatible = "qcom,sdm845-llcc", },
	{ }
};

static struct platform_driver sdm845_qcom_llcc_driver = {
	.driver = {
		.name = "sdm845-llcc",
		.of_match_table = sdm845_qcom_llcc_of_match,
	},
	.probe = sdm845_qcom_llcc_probe,
	.remove = sdm845_qcom_llcc_remove,
};
module_platform_driver(sdm845_qcom_llcc_driver);

MODULE_DESCRIPTION("QCOM sdm845 LLCC driver");
MODULE_LICENSE("GPL v2");
