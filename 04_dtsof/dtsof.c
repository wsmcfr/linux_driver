#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

static int __init dtsof_init(void)
{
	int ret = 0;
	struct device_node *of_node = NULL;
	struct device_node *supply_np = NULL;
	const char *compatible = NULL;
	const char *status = NULL;
	const char *clk_name = NULL;
	u32 reg[2];
	int count, i;

	pr_info("dtsof_init enter\n");

	/* 1. 通过路径找节点 */
	of_node = of_find_node_by_path("/soc/dsi@5a000000");
	if (!of_node) {
		pr_err("cannot find node /soc/dsi@5a000000\n");
		return -EINVAL;
	}

	pr_info("find node ok: %s\n", of_node->full_name);

	/* 2. 读取 compatible */
	ret = of_property_read_string(of_node, "compatible", &compatible);
	if (!ret)
		pr_info("compatible = %s\n", compatible);
	else
		pr_err("read compatible failed: %d\n", ret);

	/* 3. 读取 status */
	ret = of_property_read_string(of_node, "status", &status);
	if (!ret)
		pr_info("status = %s\n", status);
	else
		pr_info("status not found, ret=%d\n", ret);

	/* 4. 读取 reg = <addr size> */
	ret = of_property_read_u32_array(of_node, "reg", reg, 2);
	if (!ret)
		pr_info("reg = <0x%x 0x%x>\n", reg[0], reg[1]);
	else
		pr_err("read reg failed: %d\n", ret);

	/* 5. 读取 clock-names 字符串数组 */
	count = of_property_count_strings(of_node, "clock-names");
	if (count < 0) {
		pr_err("count clock-names failed: %d\n", count);
	} else {
		for (i = 0; i < count; i++) {
			ret = of_property_read_string_index(of_node, "clock-names", i, &clk_name);
			if (!ret)
				pr_info("clock-names[%d] = %s\n", i, clk_name);
		}
	}

	/* 6. 解析 phy-dsi-supply = <&reg18> */
	supply_np = of_parse_phandle(of_node, "phy-dsi-supply", 0);
	if (supply_np) {
		pr_info("phy-dsi-supply node = %s\n", supply_np->full_name);
		of_node_put(supply_np);
	} else {
		pr_err("parse phy-dsi-supply failed\n");
	}

	of_node_put(of_node);
	return 0;
}

static void __exit dtsof_exit(void)
{
	pr_info("dtsof_exit\n");
}

module_init(dtsof_init);
module_exit(dtsof_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CFR");