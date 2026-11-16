# SPDX-License-Identifier: GPL-2.0-only
#
# VIP scheduler ported from xring dijun-v-oss (5.15 adaptation)
#
# Built-in only: it calls non-exported scheduler core symbols
# (deactivate_task/activate_task/set_task_cpu), which are not
# available to loadable modules.
obj-$(CONFIG_XRING_SCHED_VIP) += vip_sched.o
