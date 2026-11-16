// SPDX-License-Identifier: GPL-2.0-only
/*
 * kylin_used_engine - framework core.
 *
 * Creates /proc/kylin_engine/ (read channel) and /sys/kernel/kylin_engine/
 * (write channel) and manages per-feature nodes.  See
 * include/linux/kylin_used_engine.h for the registration API.
 */
#define pr_fmt(fmt) "kylin_engine: " fmt

#include <linux/kylin_used_engine.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>

struct ke_stat_node {
	struct kylin_engine_feature *f;
	const struct kylin_engine_stat *s;	/* NULL -> all_show */
};

struct ke_attr_wrap {
	struct kobj_attribute kattr;
	struct kylin_engine_feature *f;
	const struct kylin_engine_attr *a;
};

static struct proc_dir_entry *ke_proc_root;
static struct kobject *ke_kobj;
static DEFINE_MUTEX(ke_lock);
static LIST_HEAD(ke_features);
static bool ke_ready;

/* ---------------- proc backend ---------------- */

static int ke_stat_seq(struct seq_file *m, void *v)
{
	/* single_open() passes the iterator sentinel as @v; the node we
	 * registered lives in m->private */
	struct ke_stat_node *n = m->private;

	if (n->s)
		n->s->show(n->f, m, n->s->data);
	else if (n->f->all_show)
		n->f->all_show(n->f, m);
	return 0;
}

static int ke_stat_open(struct inode *inode, struct file *file)
{
	return single_open(file, ke_stat_seq, PDE_DATA(inode));
}

static const struct proc_ops ke_stat_proc_ops = {
	.proc_open	= ke_stat_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int ke_features_seq(struct seq_file *m, void *v)
{
	struct kylin_engine_feature *pos;

	mutex_lock(&ke_lock);
	list_for_each_entry(pos, &ke_features, list)
		seq_printf(m, "%s\n", pos->name);
	mutex_unlock(&ke_lock);
	return 0;
}

static int ke_features_open(struct inode *inode, struct file *file)
{
	return single_open(file, ke_features_seq, NULL);
}

static const struct proc_ops ke_features_proc_ops = {
	.proc_open	= ke_features_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

/* ---------------- sysfs backend ---------------- */

static ssize_t ke_attr_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	struct ke_attr_wrap *w = container_of(attr, struct ke_attr_wrap, kattr);

	if (!w->a->show)
		return -EOPNOTSUPP;
	return w->a->show(w->f, buf);
}

static ssize_t ke_attr_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t len)
{
	struct ke_attr_wrap *w = container_of(attr, struct ke_attr_wrap, kattr);
	ssize_t ret;

	if (!w->a->store)
		return -EOPNOTSUPP;
	ret = w->a->store(w->f, buf, len);
	return ret < 0 ? ret : len;
}

/* ---------------- registration ---------------- */

int kylin_engine_register_feature(struct kylin_engine_feature *f)
{
	struct ke_stat_node *nodes = NULL;
	struct ke_attr_wrap *wraps = NULL;
	struct kylin_engine_feature *pos;
	struct proc_dir_entry *pde = NULL;
	struct kobject *kobj = NULL;
	int nstats, i, ret = 0;

	if (WARN_ON(!f || !f->name))
		return -EINVAL;
	if (!ke_ready)
		return -ENODEV;

	mutex_lock(&ke_lock);
	list_for_each_entry(pos, &ke_features, list) {
		if (!strcmp(pos->name, f->name)) {
			ret = -EEXIST;
			goto out_unlock;
		}
	}

	INIT_LIST_HEAD(&f->list);

	pde = proc_mkdir(f->name, ke_proc_root);
	if (!pde) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	nstats = f->nr_stats + (f->all_show ? 1 : 0);
	if (nstats) {
		nodes = kcalloc(nstats, sizeof(*nodes), GFP_KERNEL);
		if (!nodes) {
			ret = -ENOMEM;
			goto out_pde;
		}
		for (i = 0; i < f->nr_stats; i++) {
			nodes[i].f = f;
			nodes[i].s = &f->stats[i];
			if (!proc_create_data(f->stats[i].name, 0444, pde,
					      &ke_stat_proc_ops, &nodes[i])) {
				ret = -ENOMEM;
				goto out_nodes;
			}
		}
		if (f->all_show) {
			nodes[f->nr_stats].f = f;
			nodes[f->nr_stats].s = NULL;
			if (!proc_create_data("all", 0444, pde,
					      &ke_stat_proc_ops,
					      &nodes[f->nr_stats])) {
				ret = -ENOMEM;
				goto out_nodes;
			}
		}
	}

	if (f->nr_attrs) {
		kobj = kobject_create_and_add(f->name, ke_kobj);
		if (!kobj) {
			ret = -ENOMEM;
			goto out_nodes;
		}
		wraps = kcalloc(f->nr_attrs, sizeof(*wraps), GFP_KERNEL);
		if (!wraps) {
			ret = -ENOMEM;
			goto out_kobj;
		}
		for (i = 0; i < f->nr_attrs; i++) {
			const struct kylin_engine_attr *a = &f->attrs[i];

			wraps[i].f = f;
			wraps[i].a = a;
			wraps[i].kattr.attr.name = a->name;
			wraps[i].kattr.attr.mode = a->mode ? a->mode : 0644;
			wraps[i].kattr.show = ke_attr_show;
			wraps[i].kattr.store = ke_attr_store;
			ret = sysfs_create_file(kobj, &wraps[i].kattr.attr);
			if (ret)
				goto out_wraps;
		}
	}

	f->kobj = kobj;
	f->pde = pde;
	f->_nodes = nodes;
	f->_wraps = wraps;
	f->registered = true;
	list_add_tail(&f->list, &ke_features);
	mutex_unlock(&ke_lock);
	pr_info("feature '%s' registered\n", f->name);
	return 0;

out_wraps:
	kfree(wraps);
out_kobj:
	kobject_put(kobj);
out_nodes:
	proc_remove(pde);
	kfree(nodes);
out_pde:
out_unlock:
	mutex_unlock(&ke_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(kylin_engine_register_feature);

void kylin_engine_unregister_feature(struct kylin_engine_feature *f)
{
	if (WARN_ON(!f))
		return;

	mutex_lock(&ke_lock);
	if (!f->registered) {
		mutex_unlock(&ke_lock);
		return;
	}
	f->registered = false;
	list_del(&f->list);
	mutex_unlock(&ke_lock);

	proc_remove(f->pde);
	kfree(f->_nodes);
	kobject_put(f->kobj);
	kfree(f->_wraps);
	f->kobj = NULL;
	f->pde = NULL;
	f->_nodes = NULL;
	f->_wraps = NULL;
	pr_info("feature '%s' unregistered\n", f->name);
}
EXPORT_SYMBOL_GPL(kylin_engine_unregister_feature);

static int __init kylin_engine_init(void)
{
	ke_proc_root = proc_mkdir("kylin_engine", NULL);
	if (!ke_proc_root)
		return -ENOMEM;

	ke_kobj = kobject_create_and_add("kylin_engine", kernel_kobj);
	if (!ke_kobj) {
		proc_remove(ke_proc_root);
		ke_proc_root = NULL;
		return -ENOMEM;
	}

	if (!proc_create("features", 0444, ke_proc_root,
			 &ke_features_proc_ops)) {
		kobject_put(ke_kobj);
		ke_kobj = NULL;
		proc_remove(ke_proc_root);
		ke_proc_root = NULL;
		return -ENOMEM;
	}

	ke_ready = true;
	pr_info("ready (/proc/kylin_engine, /sys/kernel/kylin_engine)\n");
	return 0;
}
subsys_initcall(kylin_engine_init);
