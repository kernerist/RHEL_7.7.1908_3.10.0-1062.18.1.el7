#include <linux/fanotify.h>
#include <linux/fdtable.h>
#include <linux/fsnotify_backend.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h> /* UINT_MAX */
#include <linux/mount.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/audit.h>

#include "fanotify.h"

static bool should_merge(struct fsnotify_event *old_fsn,
			 struct fsnotify_event *new_fsn)
{
	struct fanotify_event_info *old, *new;

	pr_debug("%s: old=%p new=%p\n", __func__, old_fsn, new_fsn);
	old = FANOTIFY_E(old_fsn);
	new = FANOTIFY_E(new_fsn);

	if (old_fsn->inode == new_fsn->inode && old->tgid == new->tgid &&
	    old->path.mnt == new->path.mnt &&
	    old->path.dentry == new->path.dentry)
		return true;
	return false;
}

/* and the list better be locked by something too! */
static int fanotify_merge(struct list_head *list, struct fsnotify_event *event)
{
	struct fsnotify_event *test_event;

	pr_debug("%s: list=%p event=%p\n", __func__, list, event);

#ifdef CONFIG_FANOTIFY_ACCESS_PERMISSIONS
	/*
	 * Don't merge a permission event with any other event so that we know
	 * the event structure we have created in fanotify_handle_event() is the
	 * one we should check for permission response.
	 */
	if (event->mask & FAN_ALL_PERM_EVENTS)
		return 0;
#endif

	list_for_each_entry_reverse(test_event, list, list) {
		if (should_merge(test_event, event)) {
			test_event->mask |= event->mask;
			return 1;
		}
	}

	return 0;
}

#ifdef CONFIG_FANOTIFY_ACCESS_PERMISSIONS
static int fanotify_get_response(struct fsnotify_group *group,
				 struct fanotify_perm_event_info *event,
				 struct fsnotify_iter_info *iter_info)
{
	int ret;

	pr_debug("%s: group=%p event=%p\n", __func__, group, event);

	wait_event(group->fanotify_data.access_waitq, event->response);

	/* userspace responded, convert to something usable */
	switch (event->response & ~FAN_AUDIT) {
	case FAN_ALLOW:
		ret = 0;
		break;
	case FAN_DENY:
	default:
		ret = -EPERM;
	}

	/* Check if the response should be audited */
	if (event->response & FAN_AUDIT)
		audit_fanotify(event->response & ~FAN_AUDIT);

	event->response = 0;

	pr_debug("%s: group=%p event=%p about to return ret=%d\n", __func__,
		 group, event, ret);
	
	return ret;
}
#endif

static bool fanotify_should_send_event(struct fsnotify_iter_info *iter_info,
				       u32 event_mask, const void *data,
				       int data_type)
{
	__u32 marks_mask = 0, marks_ignored_mask = 0;
	const struct path *path = data;
	struct fsnotify_mark *mark;
	int type;

	pr_debug("%s: report_mask=%x mask=%x data=%p data_type=%d\n",
		 __func__, iter_info->report_mask, event_mask, data, data_type);

	/* if we don't have enough info to send an event to userspace say no */
	if (data_type != FSNOTIFY_EVENT_PATH)
		return false;

	/* sorry, fanotify only gives a damn about files and dirs */
	if (!S_ISREG(path->dentry->d_inode->i_mode) &&
	    !S_ISDIR(path->dentry->d_inode->i_mode))
		return false;

	fsnotify_foreach_obj_type(type) {
		if (!fsnotify_iter_should_report_type(iter_info, type))
			continue;
		mark = iter_info->marks[type];
		/*
		 * If the event is for a child and this mark doesn't care about
		 * events on a child, don't send it!
		 */
		if (event_mask & FS_EVENT_ON_CHILD &&
		    (type != FSNOTIFY_OBJ_TYPE_INODE ||
		     !(mark->mask & FS_EVENT_ON_CHILD)))
			continue;

		marks_mask |= mark->mask;
		marks_ignored_mask |= mark->ignored_mask;
	}

	if (S_ISDIR(path->dentry->d_inode->i_mode) &&
	    !(marks_mask & FS_ISDIR & ~marks_ignored_mask))
		return false;

	if (event_mask & FAN_ALL_OUTGOING_EVENTS & marks_mask &
				 ~marks_ignored_mask)
		return true;

	return false;
}

struct fanotify_event_info *fanotify_alloc_event(struct inode *inode, u32 mask,
						 const struct path *path)
{
	struct fanotify_event_info *event;

#ifdef CONFIG_FANOTIFY_ACCESS_PERMISSIONS
	if (mask & FAN_ALL_PERM_EVENTS) {
		struct fanotify_perm_event_info *pevent;

		pevent = kmem_cache_alloc(fanotify_perm_event_cachep,
					  GFP_KERNEL);
		if (!pevent)
			return NULL;
		event = &pevent->fae;
		pevent->response = 0;
		goto init;
	}
#endif
	event = kmem_cache_alloc(fanotify_event_cachep, GFP_KERNEL);
	if (!event)
		return NULL;
init: __maybe_unused
	fsnotify_init_event(&event->fse, inode, mask);
	event->tgid = get_pid(task_tgid(current));
	if (path) {
		event->path = *path;
		path_get(&event->path);
	} else {
		event->path.mnt = NULL;
		event->path.dentry = NULL;
	}
	return event;
}

static int fanotify_handle_event(struct fsnotify_group *group,
				 struct inode *inode,
				 u32 mask, const void *data, int data_type,
				 const unsigned char *file_name, u32 cookie,
				 struct fsnotify_iter_info *iter_info)
{
	int ret = 0;
	struct fanotify_event_info *event;
	struct fsnotify_event *fsn_event;

	BUILD_BUG_ON(FAN_ACCESS != FS_ACCESS);
	BUILD_BUG_ON(FAN_MODIFY != FS_MODIFY);
	BUILD_BUG_ON(FAN_CLOSE_NOWRITE != FS_CLOSE_NOWRITE);
	BUILD_BUG_ON(FAN_CLOSE_WRITE != FS_CLOSE_WRITE);
	BUILD_BUG_ON(FAN_OPEN != FS_OPEN);
	BUILD_BUG_ON(FAN_EVENT_ON_CHILD != FS_EVENT_ON_CHILD);
	BUILD_BUG_ON(FAN_Q_OVERFLOW != FS_Q_OVERFLOW);
	BUILD_BUG_ON(FAN_OPEN_PERM != FS_OPEN_PERM);
	BUILD_BUG_ON(FAN_ACCESS_PERM != FS_ACCESS_PERM);
	BUILD_BUG_ON(FAN_ONDIR != FS_ISDIR);

	if (!fanotify_should_send_event(iter_info, mask, data, data_type))
		return 0;

	pr_debug("%s: group=%p inode=%p mask=%x\n", __func__, group, inode,
		 mask);

#ifdef CONFIG_FANOTIFY_ACCESS_PERMISSIONS
	if (mask & FAN_ALL_PERM_EVENTS) {
		/*
		 * fsnotify_prepare_user_wait() fails if we race with mark
		 * deletion.  Just let the operation pass in that case.
		 */
		if (!fsnotify_prepare_user_wait(iter_info))
			return 0;
	}
#endif

	event = fanotify_alloc_event(inode, mask, data);
	ret = -ENOMEM;
	if (unlikely(!event))
		goto finish;

	fsn_event = &event->fse;
	ret = fsnotify_add_event(group, fsn_event, fanotify_merge);
	if (ret) {
		/* Permission events shouldn't be merged */
		BUG_ON(ret == 1 && mask & FAN_ALL_PERM_EVENTS);
		/* Our event wasn't used in the end. Free it. */
		fsnotify_destroy_event(group, fsn_event);

		ret = 0;
		goto finish;
	}

#ifdef CONFIG_FANOTIFY_ACCESS_PERMISSIONS
	if (mask & FAN_ALL_PERM_EVENTS) {
		ret = fanotify_get_response(group, FANOTIFY_PE(fsn_event),
					    iter_info);
		fsnotify_destroy_event(group, fsn_event);
	}
finish:
	if (mask & FAN_ALL_PERM_EVENTS)
		fsnotify_finish_user_wait(iter_info);
#else
finish:
#endif
	return ret;
}

static void fanotify_free_group_priv(struct fsnotify_group *group)
{
	struct user_struct *user;

	user = group->fanotify_data.user;
	atomic_dec(&user->fanotify_listeners);
	free_uid(user);
}

static void fanotify_free_event(struct fsnotify_event *fsn_event)
{
	struct fanotify_event_info *event;

	event = FANOTIFY_E(fsn_event);
	path_put(&event->path);
	put_pid(event->tgid);
#ifdef CONFIG_FANOTIFY_ACCESS_PERMISSIONS
	if (fsn_event->mask & FAN_ALL_PERM_EVENTS) {
		kmem_cache_free(fanotify_perm_event_cachep,
				FANOTIFY_PE(fsn_event));
		return;
	}
#endif
	kmem_cache_free(fanotify_event_cachep, event);
}

static void fanotify_free_mark(struct fsnotify_mark *fsn_mark)
{
	kmem_cache_free(fanotify_mark_cache, fsn_mark);
}

const struct fsnotify_ops fanotify_fsnotify_ops = {
	.handle_event = fanotify_handle_event,
	.free_group_priv = fanotify_free_group_priv,
	.free_event = fanotify_free_event,
	.free_mark = fanotify_free_mark,
};