/*
 * Copyright (C) 2006-2008 Google. All Rights Reserved.
 * Amit Singh <singh@>
 */

#include "fuse_device.h"

#include "fuse_internal.h"
#include "fuse_ipc.h"
#include "fuse_locking.h"

#include <fuse_ioctl.h>

#include <miscfs/devfs/devfs.h>
#include <stdbool.h>
#include <sys/queue.h>

#if M_OSXFUSE_ENABLE_DSELECT
#  include <sys/select.h>
#endif

#define FUSE_DEVICE_GLOBAL_LOCK()   fuse_lck_mtx_lock(fuse_device_mutex)
#define FUSE_DEVICE_GLOBAL_UNLOCK() fuse_lck_mtx_unlock(fuse_device_mutex)
#define FUSE_DEVICE_LOCAL_LOCK(d)   fuse_lck_mtx_lock((d)->mtx)
#define FUSE_DEVICE_LOCAL_UNLOCK(d) fuse_lck_mtx_unlock((d)->mtx)

static int    fuse_cdev_major          = -1;
static UInt32 fuse_interface_available = FALSE;

struct fuse_device {
    lck_mtx_t        *mtx;
    int               usecount;
    pid_t             pid;
    uint32_t          random;
    dev_t             dev;
    void             *cdev;
    struct fuse_data *data;
};

static struct fuse_device fuse_device_table[OSXFUSE_NDEVICES];

#define FUSE_DEVICE_FROM_UNIT_FAST(u) (fuse_device_t)&(fuse_device_table[(u)])

/* Interface for VFS */

/* Doesn't need lock. */
fuse_device_t
fuse_device_get(dev_t dev)
{
    int unit = minor(dev);

    if ((unit < 0) || (unit >= OSXFUSE_NDEVICES)) {
        return NULL;
    }

    return FUSE_DEVICE_FROM_UNIT_FAST(unit);
}

__inline__
void
fuse_device_lock(fuse_device_t fdev)
{
    FUSE_DEVICE_LOCAL_LOCK(fdev);
}

__inline__
void
fuse_device_unlock(fuse_device_t fdev)
{
    FUSE_DEVICE_LOCAL_UNLOCK(fdev);
}

/* Must be called under lock. */
__inline__
struct fuse_data *
fuse_device_get_mpdata(fuse_device_t fdev)
{
    return fdev->data;
}

/* Must be called under lock. */
__inline__
uint32_t
fuse_device_get_random(fuse_device_t fdev)
{
    return fdev->random;
}

/* Must be called under lock. */
__inline__
void
fuse_device_close_final(fuse_device_t fdev)
{
    if (fdev) {
        fdata_destroy(fdev->data);
        fdev->data   = NULL;
        fdev->pid    = -1;
        fdev->random = 0;
    }
}

static __inline__
void
fuse_reject_answers(struct fuse_data *data)
{
    struct fuse_ticket *ftick;

    fuse_lck_mtx_lock(data->aw_mtx);
    while ((ftick = fuse_aw_pop(data))) {
        fuse_lck_mtx_lock(ftick->tk_aw_mtx);
        fticket_set_answered(ftick);
        ftick->tk_aw_errno = ENOTCONN;
        fuse_wakeup(ftick);
        fuse_lck_mtx_unlock(ftick->tk_aw_mtx);
        fuse_ticket_release(ftick);
    }
    fuse_lck_mtx_unlock(data->aw_mtx);
}

/* /dev/osxfuseN implementation */

d_open_t   fuse_device_open;
d_close_t  fuse_device_close;
d_read_t   fuse_device_read;
d_write_t  fuse_device_write;
d_ioctl_t  fuse_device_ioctl;

#if M_OSXFUSE_ENABLE_DSELECT
d_select_t fuse_device_select;
#else
#  define fuse_device_select (d_select_t*)enodev
#endif /* M_OSXFUSE_ENABLE_DSELECT */

static struct cdevsw fuse_device_cdevsw = {
    /* open     */ fuse_device_open,
    /* close    */ fuse_device_close,
    /* read     */ fuse_device_read,
    /* write    */ fuse_device_write,
    /* ioctl    */ fuse_device_ioctl,
    /* stop     */ (d_stop_t *)enodev,
    /* reset    */ (d_reset_t *)enodev,
    /* ttys     */ 0,
    /* select   */ fuse_device_select,
    /* mmap     */ (d_mmap_t *)enodev,
    /* strategy */ (d_strategy_t *)enodev_strat,
#ifdef d_getc_t
    /* getc     */ (d_getc_t *)enodev,
#else
    /* reserved */ (void *)enodev,
#endif
#ifdef d_putc_t
    /* putc     */ (d_putc_t *)enodev,
#else
    /* reserved */ (void *)enodev,
#endif
    /* flags    */ D_TTY,
};

int
fuse_device_open(dev_t dev, __unused int flags, __unused int devtype,
                 struct proc *p)
{
    int unit;
    struct fuse_device *fdev;
    struct fuse_data   *fdata;

    fuse_trace_printf_func();

    if (fuse_interface_available == FALSE) {
        return ENOENT;
    }

    unit = minor(dev);
    if ((unit >= OSXFUSE_NDEVICES) || (unit < 0)) {
        FUSE_DEVICE_GLOBAL_UNLOCK();
        return ENOENT;
    }

    fdev = FUSE_DEVICE_FROM_UNIT_FAST(unit);
    if (!fdev) {
        FUSE_DEVICE_GLOBAL_UNLOCK();
        IOLog("OSXFUSE: device found with no softc\n");
        return ENXIO;
    }

    FUSE_DEVICE_GLOBAL_LOCK();

    if (fdev->usecount != 0) {
        FUSE_DEVICE_GLOBAL_UNLOCK();
        return EBUSY;
    }

    fdev->usecount++;

    FUSE_DEVICE_LOCAL_LOCK(fdev);

    FUSE_DEVICE_GLOBAL_UNLOCK();

    /* Could block. */
    fdata = fdata_alloc(p);

    if (fdev->data) {
        /*
         * This slot isn't currently open by a user daemon. However, it was
         * used earlier for a mount that's still lingering, even though the
         * user daemon is dead.
         */

        FUSE_DEVICE_GLOBAL_LOCK();

        fdev->usecount--;

        FUSE_DEVICE_LOCAL_UNLOCK(fdev);

        FUSE_DEVICE_GLOBAL_UNLOCK();

        fdata_destroy(fdata);

        return EBUSY;
    } else {
        fdata->dataflags |= FSESS_OPENED;
        fdata->fdev  = fdev;
        fdev->data   = fdata;
        fdev->pid    = proc_pid(p);
        fdev->random = random();
    }

    FUSE_DEVICE_LOCAL_UNLOCK(fdev);

    return KERN_SUCCESS;
}

int
fuse_device_close(dev_t dev, __unused int flags, __unused int devtype,
                  __unused struct proc *p)
{
    int unit;
    struct fuse_device *fdev;
    struct fuse_data   *data;

    fuse_trace_printf_func();

    unit = minor(dev);
    if (unit >= OSXFUSE_NDEVICES) {
        return ENOENT;
    }

    fdev = FUSE_DEVICE_FROM_UNIT_FAST(unit);
    if (!fdev) {
        return ENXIO;
    }

    data = fdev->data;
    if (!data) {
        panic("OSXFUSE: no device private data in device_close");
    }

    FUSE_DEVICE_LOCAL_LOCK(fdev);

    fdata_set_dead(data, true);

    data->dataflags &= ~FSESS_OPENED;

    fuse_reject_answers(data);

#if M_OSXFUSE_ENABLE_DSELECT
    selwakeup((struct selinfo*)&data->d_rsel);
#endif /* M_OSXFUSE_ENABLE_DSELECT */

    if (data->mount_state == FM_NOTMOUNTED) {
        /* We're not mounted. Can destroy mpdata. */
        fuse_device_close_final(fdev);
    }

    FUSE_DEVICE_LOCAL_UNLOCK(fdev);

    FUSE_DEVICE_GLOBAL_LOCK();

    /*
     * Even if usecount goes 0 here, at open time, we check if fdev->data
     * is non-NULL (that is, a lingering mount). If so, we return EBUSY.
     * We could make the usecount depend on both device-use and mount-state,
     * but I think this is truer to reality, if a bit more complex to maintain.
     */
    fdev->usecount--;

    FUSE_DEVICE_GLOBAL_UNLOCK();

    return KERN_SUCCESS;
}

int
fuse_device_read(dev_t dev, uio_t uio, int ioflag)
{
    int err = 0;
    int i;

    size_t buflen[3];
    void *buf[] = { NULL, NULL, NULL };

    struct fuse_device *fdev;
    struct fuse_data   *data;
    struct fuse_ticket *ftick = NULL;

    fuse_trace_printf_func();

    fdev = FUSE_DEVICE_FROM_UNIT_FAST(minor(dev));
    if (!fdev) {
        return ENXIO;
    }

    data = fdev->data;

    /* The (non-)blocking read loop */
    fuse_lck_mtx_lock(data->ms_mtx);
    while (!ftick) {
        if (fdata_dead_get(data)) {
            err = ENODEV;
        }
        if (err) {
            fuse_lck_mtx_unlock(data->ms_mtx);
            return err;
        }

        ftick = fuse_ms_pop(data);
        if (!ftick) {
            if (ioflag & IO_NDELAY) {
                err = EAGAIN;
            } else {
                err = fuse_msleep(data, data->ms_mtx, PCATCH, "fu_msg", NULL, data);
            }
        }
    }
    fuse_lck_mtx_unlock(data->ms_mtx);

    /* Handle different message types */
    switch (ftick->tk_ms_type) {
        case FT_M_BUF:
            buf[1]    = ftick->tk_ms_bufdata;
            buflen[1] = ftick->tk_ms_bufsize;

        case FT_M_FIOV:
            buf[0]    = ftick->tk_ms_fiov.base;
            buflen[0] = ftick->tk_ms_fiov.len;
            break;

        default:
            panic("OSXFUSE: unknown message type %d for ticket %p", ftick->tk_ms_type, ftick);
    }

    fuse_lck_mtx_lock(ftick->tk_aw_mtx);

    if (fticket_answered(ftick)) {
        /*
         * Filter out tickets, that have been marked as answered by returning
         * EINTR. In case this ticket has been interrupted drop the interrrupt
         * ticket.
         */

        fuse_remove_callback(ftick);
        err = EINTR;

        if (ftick->tk_interrupt) {
            /* Set interrupt ticket to answered and remove its callback */
            fuse_internal_interrupt_remove(ftick->tk_interrupt);
        }
    } else {
        /*
         * Transfer the ticket's data to user space.
         *
         * Note: This needs to be done while holding tk_aw_mtx. Otherwise the
         * ticket's data buffer tk_ms_bufdata might disappear on us, resulting
         * in a kernel panic
         */

        for (i = 0; buf[i]; i++) {
            if (uio_resid(uio) < (user_ssize_t)buflen[i]) {
                fdata_set_dead(data, false);
                break;
            }

            err = uiomove(buf[i], (int)buflen[i], uio);
            if (err) {
                break;
            }
        }
    }

    fuse_lck_mtx_unlock(ftick->tk_aw_mtx);

    if (fdata_dead_get(data)) {
        err = ENODEV;
    }

    fuse_ticket_release(ftick);
    return err;
}

int
fuse_device_write(dev_t dev, uio_t uio, __unused int ioflag)
{
    int err = 0;
    bool found = false;

    struct fuse_device    *fdev;
    struct fuse_data      *data;
    struct fuse_ticket    *ftick;
    struct fuse_ticket    *x_ftick;
    struct fuse_out_header ohead;

    fuse_trace_printf_func();

    fdev = FUSE_DEVICE_FROM_UNIT_FAST(minor(dev));
    if (!fdev) {
        return ENXIO;
    }

    data = fdev->data;
    if (fdata_dead_get(data)) {
        return ENOTCONN;
    }

    if (uio_resid(uio) < (user_ssize_t)sizeof(struct fuse_out_header)) {
        return EINVAL;
    }

    if ((err = uiomove((caddr_t)&ohead, (int)sizeof(struct fuse_out_header), uio))) {
        return err;
    }

    /* begin audit */

    if (uio_resid(uio) + sizeof(struct fuse_out_header) != ohead.len) {
        IOLog("OSXFUSE: message body size does not match that in the header\n");
        return EINVAL;
    }

    if (uio_resid(uio) && ohead.error) {
        IOLog("OSXFUSE: non-zero error for a message with a body\n");
        return EINVAL;
    }

    ohead.error = -(ohead.error);

    /* end audit */

    fuse_lck_mtx_lock(data->aw_mtx);
    TAILQ_FOREACH_SAFE(ftick, &data->aw_head, tk_aw_link, x_ftick) {
        if (ftick->tk_unique == ohead.unique) {
            found = true;
            fuse_aw_remove(ftick);
            break;
        }
    }
    fuse_lck_mtx_unlock(data->aw_mtx);

    if (found) {
        if (ftick->tk_aw_handler) {
            memcpy(&ftick->tk_aw_ohead, &ohead, sizeof(ohead));
            err = ftick->tk_aw_handler(ftick, uio);
        }

        fuse_ticket_release(ftick);
    }

    return err;
}

int
fuse_devices_start(void)
{
    int i = 0;

    fuse_trace_printf_func();

    bzero((void *)fuse_device_table, sizeof(fuse_device_table));

    if ((fuse_cdev_major = cdevsw_add(-1, &fuse_device_cdevsw)) == -1) {
        goto error;
    }

    for (i = 0; i < OSXFUSE_NDEVICES; i++) {

        dev_t dev = makedev(fuse_cdev_major, i);
        fuse_device_table[i].cdev = devfs_make_node(
                                        dev,
                                        DEVFS_CHAR,
                                        UID_ROOT,
                                        GID_OPERATOR,
                                        0666,
                                        OSXFUSE_DEVICE_BASENAME "%d",
                                        i);
        if (fuse_device_table[i].cdev == NULL) {
            goto error;
        }

        fuse_device_table[i].data     = NULL;
        fuse_device_table[i].dev      = dev;
        fuse_device_table[i].pid      = -1;
        fuse_device_table[i].random   = 0;
        fuse_device_table[i].usecount = 0;
        fuse_device_table[i].mtx      = lck_mtx_alloc_init(fuse_lock_group,
                                                           fuse_lock_attr);
    }

    fuse_interface_available = TRUE;

    return KERN_SUCCESS;

error:
    for (--i; i >= 0; i--) {
        devfs_remove(fuse_device_table[i].cdev);
        fuse_device_table[i].cdev = NULL;
        fuse_device_table[i].dev  = 0;
        lck_mtx_free(fuse_device_table[i].mtx, fuse_lock_group);
    }

    (void)cdevsw_remove(fuse_cdev_major, &fuse_device_cdevsw);
    fuse_cdev_major = -1;

    return KERN_FAILURE;
}

int
fuse_devices_stop(void)
{
    int i, ret;

    fuse_trace_printf_func();

    fuse_interface_available = FALSE;

    FUSE_DEVICE_GLOBAL_LOCK();

    if (fuse_cdev_major == -1) {
        FUSE_DEVICE_GLOBAL_UNLOCK();
        return KERN_SUCCESS;
    }

    for (i = 0; i < OSXFUSE_NDEVICES; i++) {

        char p_comm[MAXCOMLEN + 1] = { '?', '\0' };

        if (fuse_device_table[i].usecount != 0) {
            fuse_interface_available = TRUE;
            FUSE_DEVICE_GLOBAL_UNLOCK();
            proc_name(fuse_device_table[i].pid, p_comm, MAXCOMLEN + 1);
            IOLog("OSXFUSE: /dev/osxfuse%d is still active (pid=%d %s)\n",
                  i, fuse_device_table[i].pid, p_comm);
            return KERN_FAILURE;
        }

        if (fuse_device_table[i].data != NULL) {
            fuse_interface_available = TRUE;
            FUSE_DEVICE_GLOBAL_UNLOCK();
            proc_name(fuse_device_table[i].pid, p_comm, MAXCOMLEN + 1);
            /* The pid can't possibly be active here. */
            IOLog("OSXFUSE: /dev/osxfuse%d has a lingering mount (pid=%d, %s)\n",
                  i, fuse_device_table[i].pid, p_comm);
            return KERN_FAILURE;
        }
    }

    /* No device is in use. */

    for (i = 0; i < OSXFUSE_NDEVICES; i++) {
        devfs_remove(fuse_device_table[i].cdev);
        lck_mtx_free(fuse_device_table[i].mtx, fuse_lock_group);
        fuse_device_table[i].cdev   = NULL;
        fuse_device_table[i].dev    = 0;
        fuse_device_table[i].pid    = -1;
        fuse_device_table[i].random = 0;
        fuse_device_table[i].mtx    = NULL;
    }

    ret = cdevsw_remove(fuse_cdev_major, &fuse_device_cdevsw);
    if (ret != fuse_cdev_major) {
        IOLog("OSXFUSE: fuse_cdev_major != return from cdevsw_remove()\n");
    }

    fuse_cdev_major = -1;

    FUSE_DEVICE_GLOBAL_UNLOCK();

    return KERN_SUCCESS;
}

/* Control/Debug Utilities */

int
fuse_device_ioctl(dev_t dev, u_long cmd, caddr_t udata,
                  __unused int flags, __unused proc_t proc)
{
    int ret = EINVAL;
    struct fuse_device *fdev;
    struct fuse_data   *data;

    fuse_trace_printf_func();

    fdev = FUSE_DEVICE_FROM_UNIT_FAST(minor(dev));
    if (!fdev) {
        return ENXIO;
    }

    FUSE_DEVICE_LOCAL_LOCK(fdev);

    data = fdev->data;
    if (!data) {
        FUSE_DEVICE_LOCAL_UNLOCK(fdev);
        return ENXIO;
    }

    switch (cmd) {
    case FUSEDEVIOCSETIMPLEMENTEDBITS:
        ret = fuse_set_implemented_custom(data, *(uint64_t *)udata);
        break;

    case FUSEDEVIOCGETHANDSHAKECOMPLETE:
        if (data->mount_state == FM_NOTMOUNTED) {
            ret = ENXIO;
        } else {
            *(u_int32_t *)udata = (data->dataflags & FSESS_INITED);
            ret = 0;
        }
        break;

    case FUSEDEVIOCSETDAEMONDEAD:
        fdata_set_dead(data, true);
        fuse_lck_mtx_lock(data->timeout_mtx);
        data->timeout_status = FUSE_DAEMON_TIMEOUT_DEAD;
        fuse_lck_mtx_unlock(data->timeout_mtx);
        ret = 0;
        break;

    case FUSEDEVIOCGETRANDOM:
        *(u_int32_t *)udata = fdev->random;
        ret = 0;
        break;

    /*
     * The 'AVFI' (alter-vnode-for-inode) ioctls all require an inode number
     * as an argument. In the user-space library, you can get the inode number
     * from a path by using fuse_lookup_inode_by_path_np() [lib/fuse.c].
     *
     * To see an example of using this, see the implementation of
     * fuse_purge_path_np() in lib/fuse_darwin.c.
     */
    case FUSEDEVIOCALTERVNODEFORINODE:
        {
            HNodeRef hn;
            vnode_t  vn;
            fuse_device_t dummy_device = data->fdev;

            struct fuse_avfi_ioctl *avfi = (struct fuse_avfi_ioctl *)udata;

            ret = (int)HNodeLookupRealQuickIfExists(dummy_device,
                                                    (ino_t)avfi->inode,
                                                    0, /* fork index */
                                                    &hn,
                                                    &vn);
            if (ret) {
                break;
            }

            assert(vn != NULL);

            ret = fuse_internal_ioctl_avfi(vn, (vfs_context_t)0, avfi);

            if (vn) {
                vnode_put(vn);
            }
        }
        break;

    default:
        break;

    }

    FUSE_DEVICE_LOCAL_UNLOCK(fdev);

    return ret;
}

#if M_OSXFUSE_ENABLE_DSELECT

int
fuse_device_select(dev_t dev, int which, void *wql, struct proc *p)
{
    int unit, res = 0;
    struct fuse_device *fdev;
    struct fuse_data  *data;

    fuse_trace_printf_func();

    unit = minor(dev);
    if (unit >= OSXFUSE_NDEVICES) {
        return 1;
    }

    fdev = FUSE_DEVICE_FROM_UNIT_FAST(unit);
    if (!fdev) {
        return 1;
    }

    data = fdev->data;
    if (!data) {
        panic("OSXFUSE: no device private data in device_select");
    }

    switch (which) {
    case FREAD:
        fuse_lck_mtx_lock(data->ms_mtx);
        if (fdata_dead_get(data) || STAILQ_FIRST(&data->ms_head)) {
            res = 1;
        } else {
            selrecord((proc_t)p, (struct selinfo*)&data->d_rsel, wql);
        }
        fuse_lck_mtx_unlock(data->ms_mtx);
        break;

    case FWRITE:
        res = 1;
        break;

    case 0: /* Exceptional condition */
        fuse_lck_mtx_lock(data->ms_mtx);
        if (fdata_dead_get(data)) {
            res = 1;
        }
        fuse_lck_mtx_unlock(data->ms_mtx);
        break;

    default:
        break;
    }

    return res;
}

#endif /* M_OSXFUSE_ENABLE_DSELECT */

int
fuse_device_kill(int unit, struct proc *p)
{
    int error = ENOENT;

    struct fuse_device *fdev;
    struct fuse_data   *data;

    if ((unit < 0) || (unit >= OSXFUSE_NDEVICES)) {
        return EINVAL;
    }

    fdev = FUSE_DEVICE_FROM_UNIT_FAST(unit);
    if (!fdev) {
        return ENOENT;
    }

    FUSE_DEVICE_LOCAL_LOCK(fdev);

    data = fdev->data;
    if (data) {
        error = EPERM;
        if (p) {
            kauth_cred_t request_cred = kauth_cred_proc_ref(p);
            if ((kauth_cred_getuid(request_cred) == 0) ||
                (fuse_match_cred(data->daemoncred, request_cred) == 0)) {

                /* The following can block. */
                fdata_set_dead(data, true);

                fuse_reject_answers(data);
                error = 0;
            }
            kauth_cred_unref(&request_cred);
        }
    }

    FUSE_DEVICE_LOCAL_UNLOCK(fdev);

    return error;
}

int
fuse_device_print_vnodes(int unit_flags, struct proc *p)
{
    int error = ENOENT;
    struct fuse_device *fdev;

    int unit = unit_flags;

    if ((unit < 0) || (unit >= OSXFUSE_NDEVICES)) {
        return EINVAL;
    }

    fdev = FUSE_DEVICE_FROM_UNIT_FAST(unit);
    if (!fdev) {
        return ENOENT;
    }

    FUSE_DEVICE_LOCAL_LOCK(fdev);

    struct fuse_data *data = fdev->data;
    if (data) {
        mount_t mp = data->mp;

        if (vfs_busy(mp, LK_NOWAIT)) {
            FUSE_DEVICE_LOCAL_UNLOCK(fdev);
            return EBUSY;
        }

        error = EPERM;
        if (p) {
            kauth_cred_t request_cred = kauth_cred_proc_ref(p);
            if ((kauth_cred_getuid(request_cred) == 0) ||
                (fuse_match_cred(data->daemoncred, request_cred) == 0)) {
                fuse_internal_print_vnodes(mp);
                error = 0;
            }
            kauth_cred_unref(&request_cred);
        }

        vfs_unbusy(mp);
    }

    FUSE_DEVICE_LOCAL_UNLOCK(fdev);

    return error;
}
