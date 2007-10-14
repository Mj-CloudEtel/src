/*-
 * Copyright (c) 1992, 1993, 1994, 1995 Jan-Simon Pendry.
 * Copyright (c) 1992, 1993, 1994, 1995
 *      The Regents of the University of California.
 * Copyright (c) 2005, 2006 Masanori Ozawa <ozawa@ongs.co.jp>, ONGS Inc.
 * Copyright (c) 2006 Daichi Goto <daichi@freebsd.org>
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Jan-Simon Pendry.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)union_vnops.c	8.32 (Berkeley) 6/23/95
 * $FreeBSD$
 *
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/mutex.h>
#include <sys/namei.h>
#include <sys/sysctl.h>
#include <sys/vnode.h>
#include <sys/kdb.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/dirent.h>
#include <sys/proc.h>
#include <sys/bio.h>
#include <sys/buf.h>

#include <fs/unionfs/union.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_object.h>
#include <vm/vnode_pager.h>

#if 0
#define UNIONFS_INTERNAL_DEBUG(msg, args...)    printf(msg, ## args)
#define UNIONFS_IDBG_RENAME
#else
#define UNIONFS_INTERNAL_DEBUG(msg, args...)
#endif

/* lockmgr lock <-> reverse table */
struct lk_lr_table {
	int	lock;
	int	revlock;
};

static struct lk_lr_table un_llt[] = {
	{LK_SHARED, LK_RELEASE},
	{LK_EXCLUSIVE, LK_RELEASE},
	{LK_UPGRADE, LK_DOWNGRADE},
	{LK_EXCLUPGRADE, LK_DOWNGRADE},
	{LK_DOWNGRADE, LK_UPGRADE},
	{0, 0}
};


static int
unionfs_lookup(struct vop_lookup_args *ap)
{
	int		iswhiteout;
	int		lockflag;
	int		error , uerror, lerror;
	u_long		nameiop;
	u_long		cnflags, cnflagsbk;
	struct unionfs_node *dunp;
	struct vnode   *dvp, *udvp, *ldvp, *vp, *uvp, *lvp, *dtmpvp;
	struct vattr	va;
	struct componentname *cnp;
	struct thread  *td;

	iswhiteout = 0;
	lockflag = 0;
	error = uerror = lerror = ENOENT;
	cnp = ap->a_cnp;
	nameiop = cnp->cn_nameiop;
	cnflags = cnp->cn_flags;
	dvp = ap->a_dvp;
	dunp = VTOUNIONFS(dvp);
	udvp = dunp->un_uppervp;
	ldvp = dunp->un_lowervp;
	vp = uvp = lvp = NULLVP;
	td = curthread;
	*(ap->a_vpp) = NULLVP;

	UNIONFS_INTERNAL_DEBUG("unionfs_lookup: enter: nameiop=%ld, flags=%lx, path=%s\n", nameiop, cnflags, cnp->cn_nameptr);

	if (dvp->v_type != VDIR)
		return (ENOTDIR);

	/*
	 * If read-only and op is not LOOKUP, will return EROFS.
	 */
	if ((cnflags & ISLASTCN) &&
	    (dvp->v_mount->mnt_flag & MNT_RDONLY) &&
	    LOOKUP != nameiop)
		return (EROFS);

	/*
	 * lookup dotdot
	 */
	if (cnflags & ISDOTDOT) {
		if (LOOKUP != nameiop && udvp == NULLVP)
			return (EROFS);

		if (udvp != NULLVP) {
			dtmpvp = udvp;
			if (ldvp != NULLVP)
				VOP_UNLOCK(ldvp, 0, td);
		}
		else
			dtmpvp = ldvp;

		error = VOP_LOOKUP(dtmpvp, &vp, cnp);

		if (dtmpvp == udvp && ldvp != NULLVP) {
			VOP_UNLOCK(udvp, 0, td);
			vn_lock(dvp, LK_EXCLUSIVE | LK_RETRY, td);
		}

		if (error == 0) {
			/*
			 * Exchange lock and reference from vp to
			 * dunp->un_dvp. vp is upper/lower vnode, but it
			 * will need to return the unionfs vnode.
			 */
			if (nameiop == DELETE  || nameiop == RENAME ||
			    (cnp->cn_lkflags & LK_TYPE_MASK))
				VOP_UNLOCK(vp, 0, td);
			vrele(vp);

			VOP_UNLOCK(dvp, 0, td);
			*(ap->a_vpp) = dunp->un_dvp;
			vref(dunp->un_dvp);

			if (nameiop == DELETE || nameiop == RENAME)
				vn_lock(dunp->un_dvp, LK_EXCLUSIVE | LK_RETRY, td);
			else if (cnp->cn_lkflags & LK_TYPE_MASK)
				vn_lock(dunp->un_dvp, cnp->cn_lkflags | LK_RETRY, td);

			vn_lock(dvp, LK_EXCLUSIVE | LK_RETRY, td);
		}

		UNIONFS_INTERNAL_DEBUG("unionfs_lookup: leave (%d)\n", error);

		return (error);
	}

	/*
	 * lookup upper layer
	 */
	if (udvp != NULLVP) {
		uerror = VOP_LOOKUP(udvp, &uvp, cnp);

		if (uerror == 0) {
			if (udvp == uvp) {	/* is dot */
				vrele(uvp);
				*(ap->a_vpp) = dvp;
				vref(dvp);

				UNIONFS_INTERNAL_DEBUG("unionfs_lookup: leave (%d)\n", uerror);

				return (uerror);
			}
			if (nameiop == DELETE || nameiop == RENAME ||
			    (cnp->cn_lkflags & LK_TYPE_MASK))
				VOP_UNLOCK(uvp, 0, td);
		}

		/* check whiteout */
		if (uerror == ENOENT || uerror == EJUSTRETURN)
			if (cnp->cn_flags & ISWHITEOUT)
				iswhiteout = 1;	/* don't lookup lower */
		if (iswhiteout == 0 && ldvp != NULLVP)
			if (VOP_GETATTR(udvp, &va, cnp->cn_cred, td) == 0 &&
			    (va.va_flags & OPAQUE))
				iswhiteout = 1;	/* don't lookup lower */
#if 0
		UNIONFS_INTERNAL_DEBUG("unionfs_lookup: debug: whiteout=%d, path=%s\n", iswhiteout, cnp->cn_nameptr);
#endif
	}

	/*
	 * lookup lower layer
	 */
	if (ldvp != NULLVP && !(cnflags & DOWHITEOUT) && iswhiteout == 0) {
		/* always op is LOOKUP */
		cnp->cn_nameiop = LOOKUP;
		cnflagsbk = cnp->cn_flags;
		cnp->cn_flags = cnflags;

		lerror = VOP_LOOKUP(ldvp, &lvp, cnp);

		cnp->cn_nameiop = nameiop;
		if (udvp != NULLVP && (uerror == 0 || uerror == EJUSTRETURN))
			cnp->cn_flags = cnflagsbk;

		if (lerror == 0) {
			if (ldvp == lvp) {	/* is dot */
				if (uvp != NULLVP)
					vrele(uvp);	/* no need? */
				vrele(lvp);
				*(ap->a_vpp) = dvp;
				vref(dvp);

				UNIONFS_INTERNAL_DEBUG("unionfs_lookup: leave (%d)\n", lerror);

				return (lerror);
			}
			if (cnp->cn_lkflags & LK_TYPE_MASK)
				VOP_UNLOCK(lvp, 0, td);
		}
	}

	/*
	 * check lookup result
	 */
	if (uvp == NULLVP && lvp == NULLVP) {
		UNIONFS_INTERNAL_DEBUG("unionfs_lookup: leave (%d)\n",
		    (udvp != NULLVP ? uerror : lerror));
		return (udvp != NULLVP ? uerror : lerror);
	}

	/*
	 * check vnode type
	 */
	if (uvp != NULLVP && lvp != NULLVP && uvp->v_type != lvp->v_type) {
		vrele(lvp);
		lvp = NULLVP;
	}

	/*
	 * check shadow dir
	 */
	if (uerror != 0 && uerror != EJUSTRETURN && udvp != NULLVP &&
	    lerror == 0 && lvp != NULLVP && lvp->v_type == VDIR &&
	    !(dvp->v_mount->mnt_flag & MNT_RDONLY) &&
	    (1 < cnp->cn_namelen || '.' != *(cnp->cn_nameptr))) {
		/* get unionfs vnode in order to create a new shadow dir. */
		error = unionfs_nodeget(dvp->v_mount, NULLVP, lvp, dvp, &vp,
		    cnp, td);
		if (error != 0)
			goto unionfs_lookup_out;

		if (LK_SHARED == (cnp->cn_lkflags & LK_TYPE_MASK))
			VOP_UNLOCK(vp, 0, td);
		if (LK_EXCLUSIVE != VOP_ISLOCKED(vp, td)) {
			vn_lock(vp, LK_EXCLUSIVE | LK_RETRY, td);
			lockflag = 1;
		}
		error = unionfs_mkshadowdir(MOUNTTOUNIONFSMOUNT(dvp->v_mount),
		    udvp, VTOUNIONFS(vp), cnp, td);
		if (lockflag != 0)
			VOP_UNLOCK(vp, 0, td);
		if (error != 0) {
			UNIONFSDEBUG("unionfs_lookup: Unable to create shadow dir.");
			if ((cnp->cn_lkflags & LK_TYPE_MASK) == LK_EXCLUSIVE)
				vput(vp);
			else
				vrele(vp);
			goto unionfs_lookup_out;
		}
		if ((cnp->cn_lkflags & LK_TYPE_MASK) == LK_SHARED)
			vn_lock(vp, LK_SHARED | LK_RETRY, td);
	}
	/*
	 * get unionfs vnode.
	 */
	else {
		if (uvp != NULLVP)
			error = uerror;
		else
			error = lerror;
		if (error != 0)
			goto unionfs_lookup_out;
		error = unionfs_nodeget(dvp->v_mount, uvp, lvp, dvp, &vp,
		    cnp, td);
		if (error != 0) {
			UNIONFSDEBUG("unionfs_lookup: Unable to create unionfs vnode.");
			goto unionfs_lookup_out;
		}
		if ((nameiop == DELETE || nameiop == RENAME) &&
		    (cnp->cn_lkflags & LK_TYPE_MASK) == 0)
			vn_lock(vp, LK_EXCLUSIVE | LK_RETRY, td);
	}

	*(ap->a_vpp) = vp;

unionfs_lookup_out:
	if (uvp != NULLVP)
		vrele(uvp);
	if (lvp != NULLVP)
		vrele(lvp);

	UNIONFS_INTERNAL_DEBUG("unionfs_lookup: leave (%d)\n", error);

	return (error);
}

static int
unionfs_create(struct vop_create_args *ap)
{
	struct unionfs_node *dunp;
	struct componentname *cnp;
	struct thread  *td;
	struct vnode   *udvp;
	struct vnode   *vp;
	int		error;

	UNIONFS_INTERNAL_DEBUG("unionfs_create: enter\n");

	dunp = VTOUNIONFS(ap->a_dvp);
	cnp = ap->a_cnp;
	td = curthread;
	udvp = dunp->un_uppervp;
	error = EROFS;

	if (udvp != NULLVP) {
		if ((error = VOP_CREATE(udvp, &vp, cnp, ap->a_vap)) == 0) {
			VOP_UNLOCK(vp, 0, td);
			error = unionfs_nodeget(ap->a_dvp->v_mount, vp, NULLVP,
			    ap->a_dvp, ap->a_vpp, cnp, td);
			vrele(vp);
		}
	}

	UNIONFS_INTERNAL_DEBUG("unionfs_create: leave (%d)\n", error);

	return (error);
}

static int
unionfs_whiteout(struct vop_whiteout_args *ap)
{
	struct unionfs_node *dunp;
	struct componentname *cnp;
	struct vnode   *udvp;
	int		error;

	UNIONFS_INTERNAL_DEBUG("unionfs_whiteout: enter\n");

	dunp = VTOUNIONFS(ap->a_dvp);
	cnp = ap->a_cnp;
	udvp = dunp->un_uppervp;
	error = EOPNOTSUPP;

	if (udvp != NULLVP) {
		switch (ap->a_flags) {
		case CREATE:
		case DELETE:
		case LOOKUP:
			error = VOP_WHITEOUT(udvp, cnp, ap->a_flags);
			break;
		default:
			error = EINVAL;
			break;
		}
	}

	UNIONFS_INTERNAL_DEBUG("unionfs_whiteout: leave (%d)\n", error);

	return (error);
}

static int
unionfs_mknod(struct vop_mknod_args *ap)
{
	struct unionfs_node *dunp;
	struct componentname *cnp;
	struct thread  *td;
	struct vnode   *udvp;
	struct vnode   *vp;
	int		error;

	UNIONFS_INTERNAL_DEBUG("unionfs_mknod: enter\n");

	dunp = VTOUNIONFS(ap->a_dvp);
	cnp = ap->a_cnp;
	td = curthread;
	udvp = dunp->un_uppervp;
	error = EROFS;

	if (udvp != NULLVP) {
		if ((error = VOP_MKNOD(udvp, &vp, cnp, ap->a_vap)) == 0) {
			VOP_UNLOCK(vp, 0, td);
			error = unionfs_nodeget(ap->a_dvp->v_mount, vp, NULLVP,
			    ap->a_dvp, ap->a_vpp, cnp, td);
			vrele(vp);
		}
	}

	UNIONFS_INTERNAL_DEBUG("unionfs_mknod: leave (%d)\n", error);

	return (error);
}

static int
unionfs_open(struct vop_open_args *ap)
{
	int		error;
	struct unionfs_node *unp;
	struct unionfs_node_status *unsp;
	struct vnode   *uvp;
	struct vnode   *lvp;
	struct vnode   *targetvp;
	struct ucred   *cred;
	struct thread  *td;

	UNIONFS_INTERNAL_DEBUG("unionfs_open: enter\n");

	error = 0;
	unp = VTOUNIONFS(ap->a_vp);
	uvp = unp->un_uppervp;
	lvp = unp->un_lowervp;
	targetvp = NULLVP;
	cred = ap->a_cred;
	td = ap->a_td;

	unionfs_get_node_status(unp, td, &unsp);

	if (unsp->uns_lower_opencnt > 0 || unsp->uns_upper_opencnt > 0) {
		/* vnode is already opend. */
		if (unsp->uns_upper_opencnt > 0)
			targetvp = uvp;
		else
			targetvp = lvp;

		if (targetvp == lvp &&
		    (ap->a_mode & FWRITE) && lvp->v_type == VREG)
			targetvp = NULLVP;
	}
	if (targetvp == NULLVP) {
		if (uvp == NULLVP) {
			if ((ap->a_mode & FWRITE) && lvp->v_type == VREG) {
				error = unionfs_copyfile(unp,
				    !(ap->a_mode & O_TRUNC), cred, td);
				if (error != 0)
					goto unionfs_open_abort;
				targetvp = uvp = unp->un_uppervp;
			} else
				targetvp = lvp;
		} else
			targetvp = uvp;
	}

	error = VOP_OPEN(targetvp, ap->a_mode, cred, td, ap->a_fp);
	if (error == 0) {
		if (targetvp == uvp) {
			if (uvp->v_type == VDIR && lvp != NULLVP &&
			    unsp->uns_lower_opencnt <= 0) {
				/* open lower for readdir */
				error = VOP_OPEN(lvp, FREAD, cred, td, NULL);
				if (error != 0) {
					VOP_CLOSE(uvp, ap->a_mode, cred, td);
					goto unionfs_open_abort;
				}
				unsp->uns_node_flag |= UNS_OPENL_4_READDIR;
				unsp->uns_lower_opencnt++;
			}
			unsp->uns_upper_opencnt++;
		} else {
			unsp->uns_lower_opencnt++;
			unsp->uns_lower_openmode = ap->a_mode;
		}
		ap->a_vp->v_object = targetvp->v_object;
	}

unionfs_open_abort:
	if (error != 0)
		unionfs_tryrem_node_status(unp, td, unsp);

	UNIONFS_INTERNAL_DEBUG("unionfs_open: leave (%d)\n", error);

	return (error);
}

static int
unionfs_close(struct vop_close_args *ap)
{
	int		error;
	int		locked;
	struct unionfs_node *unp;
	struct unionfs_node_status *unsp;
	struct ucred   *cred;
	struct thread  *td;
	struct vnode   *ovp;

	UNIONFS_INTERNAL_DEBUG("unionfs_close: enter\n");

	locked = 0;
	unp = VTOUNIONFS(ap->a_vp);
	cred = ap->a_cred;
	td = ap->a_td;

	if (VOP_ISLOCKED(ap->a_vp, td) != LK_EXCLUSIVE) {
		vn_lock(ap->a_vp, LK_EXCLUSIVE | LK_RETRY, td);
		locked = 1;
	}
	unionfs_get_node_status(unp, td, &unsp);

	if (unsp->uns_lower_opencnt <= 0 && unsp->uns_upper_opencnt <= 0) {
#ifdef DIAGNOSTIC
		printf("unionfs_close: warning: open count is 0\n");
#endif
		if (unp->un_uppervp != NULLVP)
			ovp = unp->un_uppervp;
		else
			ovp = unp->un_lowervp;
	} else if (unsp->uns_upper_opencnt > 0)
		ovp = unp->un_uppervp;
	else
		ovp = unp->un_lowervp;

	error = VOP_CLOSE(ovp, ap->a_fflag, cred, td);

	if (error != 0)
		goto unionfs_close_abort;

	ap->a_vp->v_object = ovp->v_object;

	if (ovp == unp->un_uppervp) {
		unsp->uns_upper_opencnt--;
		if (unsp->uns_upper_opencnt == 0) {
			if (unsp->uns_node_flag & UNS_OPENL_4_READDIR) {
				VOP_CLOSE(unp->un_lowervp, FREAD, cred, td);
				unsp->uns_node_flag &= ~UNS_OPENL_4_READDIR;
				unsp->uns_lower_opencnt--;
			}
			if (unsp->uns_lower_opencnt > 0)
				ap->a_vp->v_object = unp->un_lowervp->v_object;
		}
	} else
		unsp->uns_lower_opencnt--;

unionfs_close_abort:
	unionfs_tryrem_node_status(unp, td, unsp);

	if (locked != 0)
		VOP_UNLOCK(ap->a_vp, 0, td);

	UNIONFS_INTERNAL_DEBUG("unionfs_close: leave (%d)\n", error);

	return (error);
}

/*
 * Check the access mode toward shadow file/dir.
 */
static int
unionfs_check_corrected_access(u_short mode,
			     struct vattr *va,
			     struct ucred *cred)
{
	int		count;
	uid_t		uid;	/* upper side vnode's uid */
	gid_t		gid;	/* upper side vnode's gid */
	u_short		vmode;	/* upper side vnode's mode */
	gid_t          *gp;
	u_short		mask;

	mask = 0;
	uid = va->va_uid;
	gid = va->va_gid;
	vmode = va->va_mode;

	/* check owner */
	if (cred->cr_uid == uid) {
		if (mode & VEXEC)
			mask |= S_IXUSR;
		if (mode & VREAD)
			mask |= S_IRUSR;
		if (mode & VWRITE)
			mask |= S_IWUSR;
		return ((vmode & mask) == mask ? 0 : EACCES);
	}

	/* check group */
	count = 0;
	gp = cred->cr_groups;
	for (; count < cred->cr_ngroups; count++, gp++) {
		if (gid == *gp) {
			if (mode & VEXEC)
				mask |= S_IXGRP;
			if (mode & VREAD)
				mask |= S_IRGRP;
			if (mode & VWRITE)
				mask |= S_IWGRP;
			return ((vmode & mask) == mask ? 0 : EACCES);
		}
	}

	/* check other */
	if (mode & VEXEC)
		mask |= S_IXOTH;
	if (mode & VREAD)
		mask |= S_IROTH;
	if (mode & VWRITE)
		mask |= S_IWOTH;

	return ((vmode & mask) == mask ? 0 : EACCES);
}

static int
unionfs_access(struct vop_access_args *ap)
{
	struct unionfs_mount *ump;
	struct unionfs_node *unp;
	struct vnode   *uvp;
	struct vnode   *lvp;
	struct thread  *td;
	struct vattr	va;
	int		mode;
	int		error;

	UNIONFS_INTERNAL_DEBUG("unionfs_access: enter\n");

	ump = MOUNTTOUNIONFSMOUNT(ap->a_vp->v_mount);
	unp = VTOUNIONFS(ap->a_vp);
	uvp = unp->un_uppervp;
	lvp = unp->un_lowervp;
	td = ap->a_td;
	mode = ap->a_mode;
	error = EACCES;

	if ((mode & VWRITE) &&
	    (ap->a_vp->v_mount->mnt_flag & MNT_RDONLY)) {
		switch (ap->a_vp->v_type) {
		case VREG:
		case VDIR:
		case VLNK:
			return (EROFS);
		default:
			break;
		}
	}

	if (uvp != NULLVP) {
		error = VOP_ACCESS(uvp, mode, ap->a_cred, td);

		UNIONFS_INTERNAL_DEBUG("unionfs_access: leave (%d)\n", error);

		return (error);
	}

	if (lvp != NULLVP) {
		if (mode & VWRITE) {
			if (ump->um_uppervp->v_mount->mnt_flag & MNT_RDONLY) {
				switch (ap->a_vp->v_type) {
				case VREG:
				case VDIR:
				case VLNK:
					return (EROFS);
				default:
					break;
				}
			} else if (ap->a_vp->v_type == VREG || ap->a_vp->v_type == VDIR) {
				/* check shadow file/dir */
				if (ump->um_copymode != UNIONFS_TRANSPARENT) {
					error = unionfs_create_uppervattr(ump,
					    lvp, &va, ap->a_cred, td);
					if (error != 0)
						return (error);

					error = unionfs_check_corrected_access(
					    mode, &va, ap->a_cred);
					if (error != 0)
						return (error);
				}
			}
			mode &= ~VWRITE;
			mode |= VREAD; /* will copy to upper */
		}
		error = VOP_ACCESS(lvp, mode, ap->a_cred, td);
	}

	UNIONFS_INTERNAL_DEBUG("unionfs_access: leave (%d)\n", error);

	return (error);
}

static int
unionfs_getattr(struct vop_getattr_args *ap)
{
	int		error;
	struct unionfs_node *unp;
	struct unionfs_mount *ump;
	struct vnode   *uvp;
	struct vnode   *lvp;
	struct thread  *td;
	struct vattr	va;

	UNIONFS_INTERNAL_DEBUG("unionfs_getattr: enter\n");

	unp = VTOUNIONFS(ap->a_vp);
	ump = MOUNTTOUNIONFSMOUNT(ap->a_vp->v_mount);
	uvp = unp->un_uppervp;
	lvp = unp->un_lowervp;
	td = ap->a_td;

	if (uvp != NULLVP) {
		if ((error = VOP_GETATTR(uvp, ap->a_vap, ap->a_cred, td)) == 0)
			ap->a_vap->va_fsid = ap->a_vp->v_mount->mnt_stat.f_fsid.val[0];

		UNIONFS_INTERNAL_DEBUG("unionfs_getattr: leave mode=%o, uid=%d, gid=%d (%d)\n",
		    ap->a_vap->va_mode, ap->a_vap->va_uid,
		    ap->a_vap->va_gid, error);

		return (error);
	}

	error = VOP_GETATTR(lvp, ap->a_vap, ap->a_cred, td);

	if (error == 0 && !(ump->um_uppervp->v_mount->mnt_flag & MNT_RDONLY)) {
		/* correct the attr toward shadow file/dir. */
		if (ap->a_vp->v_type == VREG || ap->a_vp->v_type == VDIR) {
			unionfs_create_uppervattr_core(ump, ap->a_vap, &va, td);
			ap->a_vap->va_mode = va.va_mode;
			ap->a_vap->va_uid = va.va_uid;
			ap->a_vap->va_gid = va.va_gid;
		}
	}

	if (error == 0)
		ap->a_vap->va_fsid = ap->a_vp->v_mount->mnt_stat.f_fsid.val[0];

	UNIONFS_INTERNAL_DEBUG("unionfs_getattr: leave mode=%o, uid=%d, gid=%d (%d)\n",
	    ap->a_vap->va_mode, ap->a_vap->va_uid, ap->a_vap->va_gid, error);

	return (error);
}

static int
unionfs_setattr(struct vop_setattr_args *ap)
{
	int		error;
	struct unionfs_node *unp;
	struct vnode   *uvp;
	struct vnode   *lvp;
	struct thread  *td;
	struct vattr   *vap;

	UNIONFS_INTERNAL_DEBUG("unionfs_setattr: enter\n");

	error = EROFS;
	unp = VTOUNIONFS(ap->a_vp);
	uvp = unp->un_uppervp;
	lvp = unp->un_lowervp;
	td = ap->a_td;
	vap = ap->a_vap;

	if ((ap->a_vp->v_mount->mnt_flag & MNT_RDONLY) &&
	    (vap->va_flags != VNOVAL || vap->va_uid != (uid_t)VNOVAL ||
	     vap->va_gid != (gid_t)VNOVAL || vap->va_atime.tv_sec != VNOVAL ||
	     vap->va_mtime.tv_sec != VNOVAL || vap->va_mode != (mode_t)VNOVAL))
		return (EROFS);

	if (uvp == NULLVP && lvp->v_type == VREG) {
		error = unionfs_copyfile(unp, (vap->va_size != 0),
		    ap->a_cred, td);
		if (error != 0)
			return (error);
		uvp = unp->un_uppervp;
	}

	if (uvp != NULLVP)
		error = VOP_SETATTR(uvp, vap, ap->a_cred, td);

	UNIONFS_INTERNAL_DEBUG("unionfs_setattr: leave (%d)\n", error);

	return (error);
}

static int
unionfs_read(struct vop_read_args *ap)
{
	int		error;
	struct unionfs_node *unp;
	struct vnode   *tvp;

	/* UNIONFS_INTERNAL_DEBUG("unionfs_read: enter\n"); */

	unp = VTOUNIONFS(ap->a_vp);
	tvp = (unp->un_uppervp != NULLVP ? unp->un_uppervp : unp->un_lowervp);

	error = VOP_READ(tvp, ap->a_uio, ap->a_ioflag, ap->a_cred);

	/* UNIONFS_INTERNAL_DEBUG("unionfs_read: leave (%d)\n", error); */

	return (error);
}

static int
unionfs_write(struct vop_write_args *ap)
{
	int		error;
	struct unionfs_node *unp;
	struct vnode   *tvp;

	/* UNIONFS_INTERNAL_DEBUG("unionfs_write: enter\n"); */

	unp = VTOUNIONFS(ap->a_vp);
	tvp = (unp->un_uppervp != NULLVP ? unp->un_uppervp : unp->un_lowervp);

	error = VOP_WRITE(tvp, ap->a_uio, ap->a_ioflag, ap->a_cred);

	/* UNIONFS_INTERNAL_DEBUG("unionfs_write: leave (%d)\n", error); */

	return (error);
}

static int
unionfs_lease(struct vop_lease_args *ap)
{
	int error;
	struct unionfs_node *unp;
	struct vnode   *vp;

	UNIONFS_INTERNAL_DEBUG("unionfs_lease: enter\n");

	unp = VTOUNIONFS(ap->a_vp);
	vp = (unp->un_uppervp != NULLVP ? unp->un_uppervp : unp->un_lowervp);

	error = VOP_LEASE(vp, ap->a_td, ap->a_cred, ap->a_flag);

	UNIONFS_INTERNAL_DEBUG("unionfs_lease: lease (%d)\n", error);

	return (error);
}

static int
unionfs_ioctl(struct vop_ioctl_args *ap)
{
	int error;
	struct unionfs_node *unp;
	struct unionfs_node_status *unsp;
	struct vnode   *ovp;

	UNIONFS_INTERNAL_DEBUG("unionfs_ioctl: enter\n");

 	vn_lock(ap->a_vp, LK_EXCLUSIVE | LK_RETRY, ap->a_td);
	unp = VTOUNIONFS(ap->a_vp);
	unionfs_get_node_status(unp, ap->a_td, &unsp);
	ovp = (unsp->uns_upper_opencnt ? unp->un_uppervp : unp->un_lowervp);
	unionfs_tryrem_node_status(unp, ap->a_td, unsp);
	VOP_UNLOCK(ap->a_vp, 0, ap->a_td);

	if (ovp == NULLVP)
		return (EBADF);

	error = VOP_IOCTL(ovp, ap->a_command, ap->a_data, ap->a_fflag,
	    ap->a_cred, ap->a_td);

	UNIONFS_INTERNAL_DEBUG("unionfs_ioctl: lease (%d)\n", error);

	return (error);
}

static int
unionfs_poll(struct vop_poll_args *ap)
{
	struct unionfs_node *unp;
	struct unionfs_node_status *unsp;
	struct vnode   *ovp;

 	vn_lock(ap->a_vp, LK_EXCLUSIVE | LK_RETRY, ap->a_td);
	unp = VTOUNIONFS(ap->a_vp);
	unionfs_get_node_status(unp, ap->a_td, &unsp);
	ovp = (unsp->uns_upper_opencnt ? unp->un_uppervp : unp->un_lowervp);
	unionfs_tryrem_node_status(unp, ap->a_td, unsp);
	VOP_UNLOCK(ap->a_vp, 0, ap->a_td);

	if (ovp == NULLVP)
		return (EBADF);

	return (VOP_POLL(ovp, ap->a_events, ap->a_cred, ap->a_td));
}

static int
unionfs_fsync(struct vop_fsync_args *ap)
{
	struct unionfs_node *unp;
	struct unionfs_node_status *unsp;
	struct vnode   *ovp;

	unp = VTOUNIONFS(ap->a_vp);
	unionfs_get_node_status(unp, ap->a_td, &unsp);
	ovp = (unsp->uns_upper_opencnt ? unp->un_uppervp : unp->un_lowervp);
	unionfs_tryrem_node_status(unp, ap->a_td, unsp);

	if (ovp == NULLVP)
		return (EBADF);

	return (VOP_FSYNC(ovp, ap->a_waitfor, ap->a_td));
}

static int
unionfs_remove(struct vop_remove_args *ap)
{
	int		error;
	struct unionfs_node *dunp;
	struct unionfs_node *unp;
	struct vnode   *udvp;
	struct vnode   *uvp;
	struct vnode   *lvp;
	struct componentname *cnp;
	struct thread  *td;

	UNIONFS_INTERNAL_DEBUG("unionfs_remove: enter\n");

	error = 0;
	dunp = VTOUNIONFS(ap->a_dvp);
	unp = VTOUNIONFS(ap->a_vp);
	udvp = dunp->un_uppervp;
	uvp = unp->un_uppervp;
	lvp = unp->un_lowervp;
	cnp = ap->a_cnp;
	td = curthread;

	if (udvp == NULLVP)
		return (EROFS);

	if (uvp != NULLVP) {
		cnp->cn_flags |= DOWHITEOUT;
		error = VOP_REMOVE(udvp, uvp, cnp);
	} else if (lvp != NULLVP)
		error = unionfs_mkwhiteout(udvp, cnp, td, unp->un_path);

	UNIONFS_INTERNAL_DEBUG("unionfs_remove: leave (%d)\n", error);

	return (error);
}

static int
unionfs_link(struct vop_link_args *ap)
{
	int		error;
	int		needrelookup;
	struct unionfs_node *dunp;
	struct unionfs_node *unp;
	struct vnode   *udvp;
	struct vnode   *uvp;
	struct componentname *cnp;
	struct thread  *td;

	UNIONFS_INTERNAL_DEBUG("unionfs_link: enter\n");

	error = 0;
	needrelookup = 0;
	dunp = VTOUNIONFS(ap->a_tdvp);
	unp = NULL;
	udvp = dunp->un_uppervp;
	uvp = NULLVP;
	cnp = ap->a_cnp;
	td = curthread;

	if (udvp == NULLVP)
		return (EROFS);

	if (ap->a_vp->v_op != &unionfs_vnodeops)
		uvp = ap->a_vp;
	else {
		unp = VTOUNIONFS(ap->a_vp);

		if (unp->un_uppervp == NULLVP) {
			if (ap->a_vp->v_type != VREG)
				return (EOPNOTSUPP);

			error = unionfs_copyfile(unp, 1, cnp->cn_cred, td);
			if (error != 0)
				return (error);
			needrelookup = 1;
		}
		uvp = unp->un_uppervp;
	}

	if (needrelookup != 0)
		error = unionfs_relookup_for_create(ap->a_tdvp, cnp, td);

	if (error == 0)
		error = VOP_LINK(udvp, uvp, cnp);

	UNIONFS_INTERNAL_DEBUG("unionfs_link: leave (%d)\n", error);

	return (error);
}

static int
unionfs_rename(struct vop_rename_args *ap)
{
	int		error;
	struct vnode   *fdvp;
	struct vnode   *fvp;
	struct componentname *fcnp;
	struct vnode   *tdvp;
	struct vnode   *tvp;
	struct componentname *tcnp;
	struct vnode   *ltdvp;
	struct vnode   *ltvp;
	struct thread  *td;

	/* rename target vnodes */
	struct vnode   *rfdvp;
	struct vnode   *rfvp;
	struct vnode   *rtdvp;
	struct vnode   *rtvp;

	int		needrelookup;
	struct unionfs_mount *ump;
	struct unionfs_node *unp;

	UNIONFS_INTERNAL_DEBUG("unionfs_rename: enter\n");

	error = 0;
	fdvp = ap->a_fdvp;
	fvp = ap->a_fvp;
	fcnp = ap->a_fcnp;
	tdvp = ap->a_tdvp;
	tvp = ap->a_tvp;
	tcnp = ap->a_tcnp;
	ltdvp = NULLVP;
	ltvp = NULLVP;
	td = curthread;
	rfdvp = fdvp;
	rfvp = fvp;
	rtdvp = tdvp;
	rtvp = tvp;
	needrelookup = 0;

#ifdef DIAGNOSTIC
	if (!(fcnp->cn_flags & HASBUF) || !(tcnp->cn_flags & HASBUF))
		panic("unionfs_rename: no name");
#endif

	/* check for cross device rename */
	if (fvp->v_mount != tdvp->v_mount ||
	    (tvp != NULLVP && fvp->v_mount != tvp->v_mount)) {
		error = EXDEV;
		goto unionfs_rename_abort;
	}

	/* Renaming a file to itself has no effect. */
	if (fvp == tvp)
		goto unionfs_rename_abort;

	/*
	 * from/to vnode is unionfs node.
	 */

	unp = VTOUNIONFS(fdvp);
#ifdef UNIONFS_IDBG_RENAME
	UNIONFS_INTERNAL_DEBUG("fdvp=%p, ufdvp=%p, lfdvp=%p\n", fdvp, unp->un_uppervp, unp->un_lowervp);
#endif
	if (unp->un_uppervp == NULLVP) {
		error = ENODEV;
		goto unionfs_rename_abort;
	}
	rfdvp = unp->un_uppervp;
	vref(rfdvp);

	unp = VTOUNIONFS(fvp);
#ifdef UNIONFS_IDBG_RENAME
	UNIONFS_INTERNAL_DEBUG("fvp=%p, ufvp=%p, lfvp=%p\n", fvp, unp->un_uppervp, unp->un_lowervp);
#endif
	ump = MOUNTTOUNIONFSMOUNT(fvp->v_mount);
	if (unp->un_uppervp == NULLVP) {
		switch (fvp->v_type) {
		case VREG:
			if ((error = vn_lock(fvp, LK_EXCLUSIVE, td)) != 0)
				goto unionfs_rename_abort;
			error = unionfs_copyfile(unp, 1, fcnp->cn_cred, td);
			VOP_UNLOCK(fvp, 0, td);
			if (error != 0)
				goto unionfs_rename_abort;
			break;
		case VDIR:
			if ((error = vn_lock(fvp, LK_EXCLUSIVE, td)) != 0)
				goto unionfs_rename_abort;
			error = unionfs_mkshadowdir(ump, rfdvp, unp, fcnp, td);
			VOP_UNLOCK(fvp, 0, td);
			if (error != 0)
				goto unionfs_rename_abort;
			break;
		default:
			error = ENODEV;
			goto unionfs_rename_abort;
		}

		needrelookup = 1;
	}

	if (unp->un_lowervp != NULLVP)
		fcnp->cn_flags |= DOWHITEOUT;
	rfvp = unp->un_uppervp;
	vref(rfvp);

	unp = VTOUNIONFS(tdvp);
#ifdef UNIONFS_IDBG_RENAME
	UNIONFS_INTERNAL_DEBUG("tdvp=%p, utdvp=%p, ltdvp=%p\n", tdvp, unp->un_uppervp, unp->un_lowervp);
#endif
	if (unp->un_uppervp == NULLVP) {
		error = ENODEV;
		goto unionfs_rename_abort;
	}
	rtdvp = unp->un_uppervp;
	ltdvp = unp->un_lowervp;
	vref(rtdvp);

	if (tdvp == tvp) {
		rtvp = rtdvp;
		vref(rtvp);
	} else if (tvp != NULLVP) {
		unp = VTOUNIONFS(tvp);
#ifdef UNIONFS_IDBG_RENAME
		UNIONFS_INTERNAL_DEBUG("tvp=%p, utvp=%p, ltvp=%p\n", tvp, unp->un_uppervp, unp->un_lowervp);
#endif
		if (unp->un_uppervp == NULLVP)
			rtvp = NULLVP;
		else {
			if (tvp->v_type == VDIR) {
				error = EINVAL;
				goto unionfs_rename_abort;
			}
			rtvp = unp->un_uppervp;
			ltvp = unp->un_lowervp;
			vref(rtvp);
		}
	}

	if (needrelookup != 0) {
		if ((error = vn_lock(fdvp, LK_EXCLUSIVE, td)) != 0)
			goto unionfs_rename_abort;
		error = unionfs_relookup_for_delete(fdvp, fcnp, td);
		VOP_UNLOCK(fdvp, 0, td);
		if (error != 0)
			goto unionfs_rename_abort;

		/* Locke of tvp is canceled in order to avoid recursive lock. */
		if (tvp != NULLVP && tvp != tdvp)
			VOP_UNLOCK(tvp, 0, td);
		error = unionfs_relookup_for_rename(tdvp, tcnp, td);
		if (tvp != NULLVP && tvp != tdvp)
			vn_lock(tvp, LK_EXCLUSIVE | LK_RETRY, td);
		if (error != 0)
			goto unionfs_rename_abort;
	}

	error = VOP_RENAME(rfdvp, rfvp, fcnp, rtdvp, rtvp, tcnp);

	if (fdvp != rfdvp)
		vrele(fdvp);
	if (fvp != rfvp)
		vrele(fvp);
	if (tdvp != rtdvp)
		vrele(tdvp);
	if (tvp != rtvp && tvp != NULLVP) {
		if (rtvp == NULLVP)
			vput(tvp);
		else
			vrele(tvp);
	}
	if (ltdvp != NULLVP)
		VOP_UNLOCK(ltdvp, 0, td);
	if (ltvp != NULLVP)
		VOP_UNLOCK(ltvp, 0, td);

	UNIONFS_INTERNAL_DEBUG("unionfs_rename: leave (%d)\n", error);

	return (error);

unionfs_rename_abort:
	if (fdvp != rfdvp)
		vrele(rfdvp);
	if (fvp != rfvp)
		vrele(rfvp);
	if (tdvp != rtdvp)
		vrele(rtdvp);
	vput(tdvp);
	if (tvp != rtvp && rtvp != NULLVP)
		vrele(rtvp);
	if (tvp != NULLVP) {
		if (tdvp != tvp)
			vput(tvp);
		else
			vrele(tvp);
	}
	vrele(fdvp);
	vrele(fvp);

	UNIONFS_INTERNAL_DEBUG("unionfs_rename: leave (%d)\n", error);

	return (error);
}

static int
unionfs_mkdir(struct vop_mkdir_args *ap)
{
	int		error;
	int		lkflags;
	struct unionfs_node *dunp;
	struct componentname *cnp;
	struct thread  *td;
	struct vnode   *udvp;
	struct vnode   *uvp;
	struct vattr	va;

	UNIONFS_INTERNAL_DEBUG("unionfs_mkdir: enter\n");

	error = EROFS;
	dunp = VTOUNIONFS(ap->a_dvp);
	cnp = ap->a_cnp;
	lkflags = cnp->cn_lkflags;
	td = curthread;
	udvp = dunp->un_uppervp;

	if (udvp != NULLVP) {
		/* check opaque */
		if (!(cnp->cn_flags & ISWHITEOUT)) {
			error = VOP_GETATTR(udvp, &va, cnp->cn_cred, td);
			if (error != 0)
				return (error);
			if (va.va_flags & OPAQUE) 
				cnp->cn_flags |= ISWHITEOUT;
		}

		if ((error = VOP_MKDIR(udvp, &uvp, cnp, ap->a_vap)) == 0) {
			VOP_UNLOCK(uvp, 0, td);
			cnp->cn_lkflags = LK_EXCLUSIVE;
			error = unionfs_nodeget(ap->a_dvp->v_mount, uvp, NULLVP,
			    ap->a_dvp, ap->a_vpp, cnp, td);
			cnp->cn_lkflags = lkflags;
			vrele(uvp);
		}
	}

	UNIONFS_INTERNAL_DEBUG("unionfs_mkdir: leave (%d)\n", error);

	return (error);
}

static int
unionfs_rmdir(struct vop_rmdir_args *ap)
{
	int		error;
	struct unionfs_node *dunp;
	struct unionfs_node *unp;
	struct componentname *cnp;
	struct thread  *td;
	struct vnode   *udvp;
	struct vnode   *uvp;
	struct vnode   *lvp;

	UNIONFS_INTERNAL_DEBUG("unionfs_rmdir: enter\n");

	error = 0;
	dunp = VTOUNIONFS(ap->a_dvp);
	unp = VTOUNIONFS(ap->a_vp);
	cnp = ap->a_cnp;
	td = curthread;
	udvp = dunp->un_uppervp;
	uvp = unp->un_uppervp;
	lvp = unp->un_lowervp;

	if (udvp == NULLVP)
		return (EROFS);

	if (udvp == uvp)
		return (EOPNOTSUPP);

	if (uvp != NULLVP) {
		if (lvp != NULLVP) {
			error = unionfs_check_rmdir(ap->a_vp, cnp->cn_cred, td);
			if (error != 0)
				return (error);
		}
		cnp->cn_flags |= DOWHITEOUT;
		error = VOP_RMDIR(udvp, uvp, cnp);
	}
	else if (lvp != NULLVP)
		error = unionfs_mkwhiteout(udvp, cnp, td, unp->un_path);

	UNIONFS_INTERNAL_DEBUG("unionfs_rmdir: leave (%d)\n", error);

	return (error);
}

static int
unionfs_symlink(struct vop_symlink_args *ap)
{
	int		error;
	int		lkflags;
	struct unionfs_node *dunp;
	struct componentname *cnp;
	struct thread  *td;
	struct vnode   *udvp;
	struct vnode   *uvp;

	UNIONFS_INTERNAL_DEBUG("unionfs_symlink: enter\n");

	error = EROFS;
	dunp = VTOUNIONFS(ap->a_dvp);
	cnp = ap->a_cnp;
	lkflags = cnp->cn_lkflags;
	td = curthread;
	udvp = dunp->un_uppervp;

	if (udvp != NULLVP) {
		error = VOP_SYMLINK(udvp, &uvp, cnp, ap->a_vap, ap->a_target);
		if (error == 0) {
			VOP_UNLOCK(uvp, 0, td);
			cnp->cn_lkflags = LK_EXCLUSIVE;
			error = unionfs_nodeget(ap->a_dvp->v_mount, uvp, NULLVP,
			    ap->a_dvp, ap->a_vpp, cnp, td);
			cnp->cn_lkflags = lkflags;
			vrele(uvp);
		}
	}

	UNIONFS_INTERNAL_DEBUG("unionfs_symlink: leave (%d)\n", error);

	return (error);
}

static int
unionfs_readdir(struct vop_readdir_args *ap)
{
	int		error;
	int		eofflag;
	int		locked;
	struct unionfs_node *unp;
	struct unionfs_node_status *unsp;
	struct uio     *uio;
	struct vnode   *uvp;
	struct vnode   *lvp;
	struct thread  *td;
	struct vattr    va;

	int		ncookies_bk;
	u_long         *cookies_bk;

	UNIONFS_INTERNAL_DEBUG("unionfs_readdir: enter\n");

	error = 0;
	eofflag = 0;
	locked = 0;
	unp = VTOUNIONFS(ap->a_vp);
	uio = ap->a_uio;
	uvp = unp->un_uppervp;
	lvp = unp->un_lowervp;
	td = uio->uio_td;
	ncookies_bk = 0;
	cookies_bk = NULL;

	if (ap->a_vp->v_type != VDIR)
		return (ENOTDIR);

	/* check opaque */
	if (uvp != NULLVP && lvp != NULLVP) {
		if ((error = VOP_GETATTR(uvp, &va, ap->a_cred, td)) != 0)
			goto unionfs_readdir_exit;
		if (va.va_flags & OPAQUE)
			lvp = NULLVP;
	}

	/* check the open count. unionfs needs to open before readdir. */
	if (VOP_ISLOCKED(ap->a_vp, td) != LK_EXCLUSIVE) {
		vn_lock(ap->a_vp, LK_UPGRADE | LK_RETRY, td);
		locked = 1;
	}
	unionfs_get_node_status(unp, td, &unsp);
	if ((uvp != NULLVP && unsp->uns_upper_opencnt <= 0) ||
	    (lvp != NULLVP && unsp->uns_lower_opencnt <= 0)) {
		unionfs_tryrem_node_status(unp, td, unsp);
		error = EBADF;
	}
	if (locked == 1)
		vn_lock(ap->a_vp, LK_DOWNGRADE | LK_RETRY, td);
	if (error != 0)
		goto unionfs_readdir_exit;

	/* upper only */
	if (uvp != NULLVP && lvp == NULLVP) {
		error = VOP_READDIR(uvp, uio, ap->a_cred, ap->a_eofflag,
		    ap->a_ncookies, ap->a_cookies);
		unsp->uns_readdir_status = 0;

		goto unionfs_readdir_exit;
	}

	/* lower only */
	if (uvp == NULLVP && lvp != NULLVP) {
		error = VOP_READDIR(lvp, uio, ap->a_cred, ap->a_eofflag,
		    ap->a_ncookies, ap->a_cookies);
		unsp->uns_readdir_status = 2;

		goto unionfs_readdir_exit;
	}

	/*
	 * readdir upper and lower
	 */
	if (uio->uio_offset == 0)
		unsp->uns_readdir_status = 0;

	if (unsp->uns_readdir_status == 0) {
		/* read upper */
		error = VOP_READDIR(uvp, uio, ap->a_cred, &eofflag,
				    ap->a_ncookies, ap->a_cookies);

		if (error != 0 || eofflag == 0)
			goto unionfs_readdir_exit;
		unsp->uns_readdir_status = 1;

		/*
		 * ufs(and other fs) needs size of uio_resid larger than
		 * DIRBLKSIZ.
		 * size of DIRBLKSIZ equals DEV_BSIZE.
		 * (see: ufs/ufs/ufs_vnops.c ufs_readdir func , ufs/ufs/dir.h)
		 */
		if (uio->uio_resid <= (uio->uio_resid & (DEV_BSIZE -1)))
			goto unionfs_readdir_exit;

		/*
		 * backup cookies
		 * It prepares to readdir in lower.
		 */
		if (ap->a_ncookies != NULL) {
			ncookies_bk = *(ap->a_ncookies);
			*(ap->a_ncookies) = 0;
		}
		if (ap->a_cookies != NULL) {
			cookies_bk = *(ap->a_cookies);
			*(ap->a_cookies) = NULL;
		}
	}

	/* initialize for readdir in lower */
	if (unsp->uns_readdir_status == 1) {
		unsp->uns_readdir_status = 2;
		uio->uio_offset = 0;
	}

	if (lvp == NULLVP) {
		error = EBADF;
		goto unionfs_readdir_exit;
	}
	/* read lower */
	error = VOP_READDIR(lvp, uio, ap->a_cred, ap->a_eofflag,
			    ap->a_ncookies, ap->a_cookies);

	if (cookies_bk != NULL) {
		/* merge cookies */
		int		size;
		u_long         *newcookies, *pos;

		size = *(ap->a_ncookies) + ncookies_bk;
		newcookies = (u_long *) malloc(size * sizeof(u_long),
		    M_TEMP, M_WAITOK);
		pos = newcookies;

		memcpy(pos, cookies_bk, ncookies_bk * sizeof(u_long));
		pos += ncookies_bk * sizeof(u_long);
		memcpy(pos, *(ap->a_cookies), *(ap->a_ncookies) * sizeof(u_long));
		free(cookies_bk, M_TEMP);
		free(*(ap->a_cookies), M_TEMP);
		*(ap->a_ncookies) = size;
		*(ap->a_cookies) = newcookies;
	}

unionfs_readdir_exit:
	if (error != 0 && ap->a_eofflag != NULL)
		*(ap->a_eofflag) = 1;

	UNIONFS_INTERNAL_DEBUG("unionfs_readdir: leave (%d)\n", error);

	return (error);
}

static int
unionfs_readlink(struct vop_readlink_args *ap)
{
	int error;
	struct unionfs_node *unp;
	struct vnode   *vp;

	UNIONFS_INTERNAL_DEBUG("unionfs_readlink: enter\n");

	unp = VTOUNIONFS(ap->a_vp);
	vp = (unp->un_uppervp != NULLVP ? unp->un_uppervp : unp->un_lowervp);

	error = VOP_READLINK(vp, ap->a_uio, ap->a_cred);

	UNIONFS_INTERNAL_DEBUG("unionfs_readlink: leave (%d)\n", error);

	return (error);
}

static int
unionfs_getwritemount(struct vop_getwritemount_args *ap)
{
	int		error;
	struct vnode   *uvp;
	struct vnode   *vp;

	UNIONFS_INTERNAL_DEBUG("unionfs_getwritemount: enter\n");

	error = 0;
	vp = ap->a_vp;

	if (vp == NULLVP || (vp->v_mount->mnt_flag & MNT_RDONLY))
		return (EACCES);

	uvp = UNIONFSVPTOUPPERVP(vp);
	if (uvp == NULLVP && VREG == vp->v_type)
		uvp = UNIONFSVPTOUPPERVP(VTOUNIONFS(vp)->un_dvp);

	if (uvp != NULLVP)
		error = VOP_GETWRITEMOUNT(uvp, ap->a_mpp);
	else {
		VI_LOCK(vp);
		if (vp->v_iflag & VI_FREE)
			error = EOPNOTSUPP;
		else
			error = EACCES;
		VI_UNLOCK(vp);
	}

	UNIONFS_INTERNAL_DEBUG("unionfs_getwritemount: leave (%d)\n", error);

	return (error);
}

static int
unionfs_inactive(struct vop_inactive_args *ap)
{
	struct unionfs_node *unp;

	unp = VTOUNIONFS(ap->a_vp);

	if (unp == NULL || !(unp->un_flag & UNIONFS_CACHED))
		vgone(ap->a_vp);

	return (0);
}

static int
unionfs_reclaim(struct vop_reclaim_args *ap)
{
	/* UNIONFS_INTERNAL_DEBUG("unionfs_reclaim: enter\n"); */

	unionfs_hashrem(ap->a_vp, ap->a_td);

	/* UNIONFS_INTERNAL_DEBUG("unionfs_reclaim: leave\n"); */

	return (0);
}

static int
unionfs_print(struct vop_print_args *ap)
{
	struct unionfs_node *unp;
	/* struct unionfs_node_status *unsp; */

	unp = VTOUNIONFS(ap->a_vp);
	/* unionfs_get_node_status(unp, curthread, &unsp); */

	printf("unionfs_vp=%p, uppervp=%p, lowervp=%p\n",
	    ap->a_vp, unp->un_uppervp, unp->un_lowervp);
	/*
	printf("unionfs opencnt: uppervp=%d, lowervp=%d\n",
	    unsp->uns_upper_opencnt, unsp->uns_lower_opencnt);
	*/

	if (unp->un_uppervp != NULLVP)
		vprint("unionfs: upper", unp->un_uppervp);
	if (unp->un_lowervp != NULLVP)
		vprint("unionfs: lower", unp->un_lowervp);

	return (0);
}

static int
unionfs_get_llt_revlock(int flags)
{
	int count;

	flags &= LK_TYPE_MASK;
	for (count = 0; un_llt[count].lock != 0; count++) {
		if (flags == un_llt[count].lock) {
			return un_llt[count].revlock;
		}
	}

	return 0;
}

static int
unionfs_lock(struct vop_lock1_args *ap)
{
	int		error;
	int		flags;
	int		revlock;
	int		uhold;
	struct unionfs_mount *ump;
	struct unionfs_node *unp;
	struct vnode   *vp;
	struct vnode   *uvp;
	struct vnode   *lvp;
	struct thread  *td;

	error = 0;
	uhold = 0;
	flags = ap->a_flags;
	vp = ap->a_vp;
	td = ap->a_td;

	if (LK_RELEASE == (flags & LK_TYPE_MASK) || !(flags & LK_TYPE_MASK))
		return (VOP_UNLOCK(vp, flags, td));

	if ((revlock = unionfs_get_llt_revlock(flags)) == 0)
		panic("unknown lock type: 0x%x", flags & LK_TYPE_MASK);

	if (!(flags & LK_INTERLOCK))
		VI_LOCK(vp);

	ump = MOUNTTOUNIONFSMOUNT(vp->v_mount);
	unp = VTOUNIONFS(vp);
	if (NULL == unp)
		goto unionfs_lock_null_vnode;
	lvp = unp->un_lowervp;
	uvp = unp->un_uppervp;

	/*
	 * Sometimes, lower or upper is already exclusive locked.
	 * (ex. vfs_domount: mounted vnode is already locked.)
	 */
	if ((flags & LK_TYPE_MASK) == LK_EXCLUSIVE &&
	    vp == ump->um_rootvp)
		flags |= LK_CANRECURSE;

	if (lvp != NULLVP) {
		VI_LOCK_FLAGS(lvp, MTX_DUPOK);
		flags |= LK_INTERLOCK;
		vholdl(lvp);

		VI_UNLOCK(vp);
		ap->a_flags &= ~LK_INTERLOCK;

		error = VOP_LOCK(lvp, flags, td);

		VI_LOCK(vp);
		unp = VTOUNIONFS(vp);
		if (unp == NULL) {
			if (error == 0) 
				VOP_UNLOCK(lvp, 0, td);
			VI_UNLOCK(vp);
			vdrop(lvp);
			return (vop_stdlock(ap));
		}
	}

	if (error == 0 && uvp != NULLVP) {
		VI_LOCK_FLAGS(uvp, MTX_DUPOK);
		flags |= LK_INTERLOCK;
		vholdl(uvp);
		uhold = 1;

		VI_UNLOCK(vp);
		ap->a_flags &= ~LK_INTERLOCK;

		error = VOP_LOCK(uvp, flags, td);

		VI_LOCK(vp);
		unp = VTOUNIONFS(vp);
		if (unp == NULL) {
			if (error == 0) {
				VOP_UNLOCK(uvp, 0, td);
				if (lvp != NULLVP)
					VOP_UNLOCK(lvp, 0, td);
			}
			VI_UNLOCK(vp);
			if (lvp != NULLVP)
				vdrop(lvp);
			vdrop(uvp);
			return (vop_stdlock(ap));
		}

		if (error != 0 && lvp != NULLVP)
			vn_lock(lvp, revlock | LK_RETRY, td);
	}

	VI_UNLOCK(vp);
	if (lvp != NULLVP)
		vdrop(lvp);
	if (uhold != 0)
		vdrop(uvp);

	return (error);

unionfs_lock_null_vnode:
	ap->a_flags |= LK_INTERLOCK;
	return (vop_stdlock(ap));
}

static int
unionfs_unlock(struct vop_unlock_args *ap)
{
	int		error;
	int		flags;
	int		mtxlkflag;
	int		uhold;
	struct vnode   *vp;
	struct vnode   *lvp;
	struct vnode   *uvp;
	struct unionfs_node *unp;

	error = 0;
	mtxlkflag = 0;
	uhold = 0;
	flags = ap->a_flags | LK_RELEASE;
	vp = ap->a_vp;

	if (flags & LK_INTERLOCK)
		mtxlkflag = 1;
	else if (mtx_owned(VI_MTX(vp)) == 0) {
		VI_LOCK(vp);
		mtxlkflag = 2;
	}

	unp = VTOUNIONFS(vp);
	if (unp == NULL)
		goto unionfs_unlock_null_vnode;
	lvp = unp->un_lowervp;
	uvp = unp->un_uppervp;

	if (lvp != NULLVP) {
		VI_LOCK_FLAGS(lvp, MTX_DUPOK);
		flags |= LK_INTERLOCK;
		vholdl(lvp);

		VI_UNLOCK(vp);
		ap->a_flags &= ~LK_INTERLOCK;

		error = VOP_UNLOCK(lvp, flags, ap->a_td);

		VI_LOCK(vp);
	}

	if (error == 0 && uvp != NULLVP) {
		VI_LOCK_FLAGS(uvp, MTX_DUPOK);
		flags |= LK_INTERLOCK;
		vholdl(uvp);
		uhold = 1;

		VI_UNLOCK(vp);
		ap->a_flags &= ~LK_INTERLOCK;

		error = VOP_UNLOCK(uvp, flags, ap->a_td);

		VI_LOCK(vp);
	}

	VI_UNLOCK(vp);
	if (lvp != NULLVP)
		vdrop(lvp);
	if (uhold != 0)
		vdrop(uvp);
	if (mtxlkflag == 0)
		VI_LOCK(vp);

	return error;

unionfs_unlock_null_vnode:
	if (mtxlkflag == 2)
		VI_UNLOCK(vp);
	return (vop_stdunlock(ap));
}

static int
unionfs_pathconf(struct vop_pathconf_args *ap)
{
	struct unionfs_node *unp;
	struct vnode   *vp;

	unp = VTOUNIONFS(ap->a_vp);
	vp = (unp->un_uppervp != NULLVP ? unp->un_uppervp : unp->un_lowervp);

	return (VOP_PATHCONF(vp, ap->a_name, ap->a_retval));
}

static int
unionfs_advlock(struct vop_advlock_args *ap)
{
	int error;
	struct unionfs_node *unp;
	struct unionfs_node_status *unsp;
	struct vnode   *vp;
	struct vnode   *uvp;
	struct thread  *td;

	UNIONFS_INTERNAL_DEBUG("unionfs_advlock: enter\n");

	vp = ap->a_vp;
	td = curthread;

	vn_lock(vp, LK_EXCLUSIVE | LK_RETRY, td);

	unp = VTOUNIONFS(ap->a_vp);
	uvp = unp->un_uppervp;

	if (uvp == NULLVP) {
		error = unionfs_copyfile(unp, 1, td->td_ucred, td);
		if (error != 0)
			goto unionfs_advlock_abort;
		uvp = unp->un_uppervp;

		unionfs_get_node_status(unp, td, &unsp);
		if (unsp->uns_lower_opencnt > 0) {
			/* try reopen the vnode */
			error = VOP_OPEN(uvp, unsp->uns_lower_openmode,
				td->td_ucred, td, NULL);
			if (error)
				goto unionfs_advlock_abort;
			unsp->uns_upper_opencnt++;
			VOP_CLOSE(unp->un_lowervp, unsp->uns_lower_openmode, td->td_ucred, td);
			unsp->uns_lower_opencnt--;
		} else
			unionfs_tryrem_node_status(unp, td, unsp);
	}

	VOP_UNLOCK(vp, 0, td);

	error = VOP_ADVLOCK(uvp, ap->a_id, ap->a_op, ap->a_fl, ap->a_flags);

	UNIONFS_INTERNAL_DEBUG("unionfs_advlock: leave (%d)\n", error);

	return error;

unionfs_advlock_abort:
	VOP_UNLOCK(vp, 0, td);

	UNIONFS_INTERNAL_DEBUG("unionfs_advlock: leave (%d)\n", error);

	return error;
}

static int
unionfs_strategy(struct vop_strategy_args *ap)
{
	struct unionfs_node *unp;
	struct vnode   *vp;

	unp = VTOUNIONFS(ap->a_vp);
	vp = (unp->un_uppervp != NULLVP ? unp->un_uppervp : unp->un_lowervp);

#ifdef DIAGNOSTIC
	if (vp == NULLVP)
		panic("unionfs_strategy: nullvp");

	if (ap->a_bp->b_iocmd == BIO_WRITE && vp == unp->un_lowervp)
		panic("unionfs_strategy: writing to lowervp");
#endif

	return (VOP_STRATEGY(vp, ap->a_bp));
}

static int
unionfs_getacl(struct vop_getacl_args *ap)
{
	int		error;
	struct unionfs_node *unp;
	struct vnode   *vp;

	unp = VTOUNIONFS(ap->a_vp);
	vp = (unp->un_uppervp != NULLVP ? unp->un_uppervp : unp->un_lowervp);

	UNIONFS_INTERNAL_DEBUG("unionfs_getacl: enter\n");

	error = VOP_GETACL(vp, ap->a_type, ap->a_aclp, ap->a_cred, ap->a_td);

	UNIONFS_INTERNAL_DEBUG("unionfs_getacl: leave (%d)\n", error);

	return (error);
}

static int
unionfs_setacl(struct vop_setacl_args *ap)
{
	int		error;
	struct unionfs_node *unp;
	struct vnode   *uvp;
	struct vnode   *lvp;
	struct thread  *td;

	UNIONFS_INTERNAL_DEBUG("unionfs_setacl: enter\n");

	error = EROFS;
	unp = VTOUNIONFS(ap->a_vp);
	uvp = unp->un_uppervp;
	lvp = unp->un_lowervp;
	td = ap->a_td;

	if (ap->a_vp->v_mount->mnt_flag & MNT_RDONLY)
		return (EROFS);

	if (uvp == NULLVP && lvp->v_type == VREG) {
		if ((error = unionfs_copyfile(unp, 1, ap->a_cred, td)) != 0)
			return (error);
		uvp = unp->un_uppervp;
	}

	if (uvp != NULLVP)
		error = VOP_SETACL(uvp, ap->a_type, ap->a_aclp, ap->a_cred, td);

	UNIONFS_INTERNAL_DEBUG("unionfs_setacl: leave (%d)\n", error);

	return (error);
}

static int
unionfs_aclcheck(struct vop_aclcheck_args *ap)
{
	int		error;
	struct unionfs_node *unp;
	struct vnode   *vp;

	UNIONFS_INTERNAL_DEBUG("unionfs_aclcheck: enter\n");

	unp = VTOUNIONFS(ap->a_vp);
	vp = (unp->un_uppervp != NULLVP ? unp->un_uppervp : unp->un_lowervp);

	error = VOP_ACLCHECK(vp, ap->a_type, ap->a_aclp, ap->a_cred, ap->a_td);

	UNIONFS_INTERNAL_DEBUG("unionfs_aclcheck: leave (%d)\n", error);

	return (error);
}

static int
unionfs_openextattr(struct vop_openextattr_args *ap)
{
	int		error;
	struct unionfs_node *unp;
	struct vnode   *vp;

	unp = VTOUNIONFS(ap->a_vp);
	vp = (unp->un_uppervp != NULLVP ? unp->un_uppervp : unp->un_lowervp);

	if ((vp == unp->un_uppervp && (unp->un_flag & UNIONFS_OPENEXTU)) ||
	    (vp == unp->un_lowervp && (unp->un_flag & UNIONFS_OPENEXTL)))
		return (EBUSY);

	error = VOP_OPENEXTATTR(vp, ap->a_cred, ap->a_td);

	if (error == 0) {
		if (vp == unp->un_uppervp)
			unp->un_flag |= UNIONFS_OPENEXTU;
		else
			unp->un_flag |= UNIONFS_OPENEXTL;
	}

	return (error);
}

static int
unionfs_closeextattr(struct vop_closeextattr_args *ap)
{
	int		error;
	struct unionfs_node *unp;
	struct vnode   *vp;

	unp = VTOUNIONFS(ap->a_vp);
	vp = NULLVP;

	if (unp->un_flag & UNIONFS_OPENEXTU)
		vp = unp->un_uppervp;
	else if (unp->un_flag & UNIONFS_OPENEXTL)
		vp = unp->un_lowervp;

	if (vp == NULLVP)
		return (EOPNOTSUPP);

	error = VOP_CLOSEEXTATTR(vp, ap->a_commit, ap->a_cred, ap->a_td);

	if (error == 0) {
		if (vp == unp->un_uppervp)
			unp->un_flag &= ~UNIONFS_OPENEXTU;
		else
			unp->un_flag &= ~UNIONFS_OPENEXTL;
	}

	return (error);
}

static int
unionfs_getextattr(struct vop_getextattr_args *ap)
{
	struct unionfs_node *unp;
	struct vnode   *vp;

	unp = VTOUNIONFS(ap->a_vp);
	vp = NULLVP;

	if (unp->un_flag & UNIONFS_OPENEXTU)
		vp = unp->un_uppervp;
	else if (unp->un_flag & UNIONFS_OPENEXTL)
		vp = unp->un_lowervp;

	if (vp == NULLVP)
		return (EOPNOTSUPP);

	return (VOP_GETEXTATTR(vp, ap->a_attrnamespace, ap->a_name,
	    ap->a_uio, ap->a_size, ap->a_cred, ap->a_td));
}

static int
unionfs_setextattr(struct vop_setextattr_args *ap)
{
	int		error;
	struct unionfs_node *unp;
	struct vnode   *uvp;
	struct vnode   *lvp;
	struct vnode   *ovp;
	struct ucred   *cred;
	struct thread  *td;

	error = EROFS;
	unp = VTOUNIONFS(ap->a_vp);
	uvp = unp->un_uppervp;
	lvp = unp->un_lowervp;
	ovp = NULLVP;
	cred = ap->a_cred;
	td = ap->a_td;

	UNIONFS_INTERNAL_DEBUG("unionfs_setextattr: enter (un_flag=%x)\n", unp->un_flag);

	if (ap->a_vp->v_mount->mnt_flag & MNT_RDONLY)
		return (EROFS);

	if (unp->un_flag & UNIONFS_OPENEXTU)
		ovp = unp->un_uppervp;
	else if (unp->un_flag & UNIONFS_OPENEXTL)
		ovp = unp->un_lowervp;

	if (ovp == NULLVP)
		return (EOPNOTSUPP);

	if (ovp == lvp && lvp->v_type == VREG) {
		VOP_CLOSEEXTATTR(lvp, 0, cred, td);
		if (uvp == NULLVP &&
		    (error = unionfs_copyfile(unp, 1, cred, td)) != 0) {
unionfs_setextattr_reopen:
			if ((unp->un_flag & UNIONFS_OPENEXTL) &&
			    VOP_OPENEXTATTR(lvp, cred, td)) {
#ifdef DIAGNOSTIC
				panic("unionfs: VOP_OPENEXTATTR failed");
#endif
				unp->un_flag &= ~UNIONFS_OPENEXTL;
			}
			goto unionfs_setextattr_abort;
		}
		uvp = unp->un_uppervp;
		if ((error = VOP_OPENEXTATTR(uvp, cred, td)) != 0)
			goto unionfs_setextattr_reopen;
		unp->un_flag &= ~UNIONFS_OPENEXTL;
		unp->un_flag |= UNIONFS_OPENEXTU;
		ovp = uvp;
	}

	if (ovp == uvp)
		error = VOP_SETEXTATTR(ovp, ap->a_attrnamespace, ap->a_name,
		    ap->a_uio, cred, td);

unionfs_setextattr_abort:
	UNIONFS_INTERNAL_DEBUG("unionfs_setextattr: leave (%d)\n", error);

	return (error);
}

static int
unionfs_listextattr(struct vop_listextattr_args *ap)
{
	struct unionfs_node *unp;
	struct vnode   *vp;

	unp = VTOUNIONFS(ap->a_vp);
	vp = NULLVP;

	if (unp->un_flag & UNIONFS_OPENEXTU)
		vp = unp->un_uppervp;
	else if (unp->un_flag & UNIONFS_OPENEXTL)
		vp = unp->un_lowervp;

	if (vp == NULLVP)
		return (EOPNOTSUPP);

	return (VOP_LISTEXTATTR(vp, ap->a_attrnamespace, ap->a_uio,
	    ap->a_size, ap->a_cred, ap->a_td));
}

static int
unionfs_deleteextattr(struct vop_deleteextattr_args *ap)
{
	int		error;
	struct unionfs_node *unp;
	struct vnode   *uvp;
	struct vnode   *lvp;
	struct vnode   *ovp;
	struct ucred   *cred;
	struct thread  *td;

	error = EROFS;
	unp = VTOUNIONFS(ap->a_vp);
	uvp = unp->un_uppervp;
	lvp = unp->un_lowervp;
	ovp = NULLVP;
	cred = ap->a_cred;
	td = ap->a_td;

	UNIONFS_INTERNAL_DEBUG("unionfs_deleteextattr: enter (un_flag=%x)\n", unp->un_flag);

	if (ap->a_vp->v_mount->mnt_flag & MNT_RDONLY)
		return (EROFS);

	if (unp->un_flag & UNIONFS_OPENEXTU)
		ovp = unp->un_uppervp;
	else if (unp->un_flag & UNIONFS_OPENEXTL)
		ovp = unp->un_lowervp;

	if (ovp == NULLVP)
		return (EOPNOTSUPP);

	if (ovp == lvp && lvp->v_type == VREG) {
		VOP_CLOSEEXTATTR(lvp, 0, cred, td);
		if (uvp == NULLVP &&
		    (error = unionfs_copyfile(unp, 1, cred, td)) != 0) {
unionfs_deleteextattr_reopen:
			if ((unp->un_flag & UNIONFS_OPENEXTL) &&
			    VOP_OPENEXTATTR(lvp, cred, td)) {
#ifdef DIAGNOSTIC
				panic("unionfs: VOP_OPENEXTATTR failed");
#endif
				unp->un_flag &= ~UNIONFS_OPENEXTL;
			}
			goto unionfs_deleteextattr_abort;
		}
		uvp = unp->un_uppervp;
		if ((error = VOP_OPENEXTATTR(uvp, cred, td)) != 0)
			goto unionfs_deleteextattr_reopen;
		unp->un_flag &= ~UNIONFS_OPENEXTL;
		unp->un_flag |= UNIONFS_OPENEXTU;
		ovp = uvp;
	}

	if (ovp == uvp)
		error = VOP_DELETEEXTATTR(ovp, ap->a_attrnamespace, ap->a_name,
		    ap->a_cred, ap->a_td);

unionfs_deleteextattr_abort:
	UNIONFS_INTERNAL_DEBUG("unionfs_deleteextattr: leave (%d)\n", error);

	return (error);
}

static int
unionfs_setlabel(struct vop_setlabel_args *ap)
{
	int		error;
	struct unionfs_node *unp;
	struct vnode   *uvp;
	struct vnode   *lvp;
	struct thread  *td;

	UNIONFS_INTERNAL_DEBUG("unionfs_setlabel: enter\n");

	error = EROFS;
	unp = VTOUNIONFS(ap->a_vp);
	uvp = unp->un_uppervp;
	lvp = unp->un_lowervp;
	td = ap->a_td;

	if (ap->a_vp->v_mount->mnt_flag & MNT_RDONLY)
		return (EROFS);

	if (uvp == NULLVP && lvp->v_type == VREG) {
		if ((error = unionfs_copyfile(unp, 1, ap->a_cred, td)) != 0)
			return (error);
		uvp = unp->un_uppervp;
	}

	if (uvp != NULLVP)
		error = VOP_SETLABEL(uvp, ap->a_label, ap->a_cred, td);

	UNIONFS_INTERNAL_DEBUG("unionfs_setlabel: leave (%d)\n", error);

	return (error);
}

static int
unionfs_vptofh(struct vop_vptofh_args *ap)
{
	return (EOPNOTSUPP);
}

struct vop_vector unionfs_vnodeops = {
	.vop_default =		&default_vnodeops,

	.vop_access =		unionfs_access,
	.vop_aclcheck =		unionfs_aclcheck,
	.vop_advlock =		unionfs_advlock,
	.vop_bmap =		VOP_EOPNOTSUPP,
	.vop_close =		unionfs_close,
	.vop_closeextattr =	unionfs_closeextattr,
	.vop_create =		unionfs_create,
	.vop_deleteextattr =	unionfs_deleteextattr,
	.vop_fsync =		unionfs_fsync,
	.vop_getacl =		unionfs_getacl,
	.vop_getattr =		unionfs_getattr,
	.vop_getextattr =	unionfs_getextattr,
	.vop_getwritemount =	unionfs_getwritemount,
	.vop_inactive =		unionfs_inactive,
	.vop_ioctl =		unionfs_ioctl,
	.vop_lease =		unionfs_lease,
	.vop_link =		unionfs_link,
	.vop_listextattr =	unionfs_listextattr,
	.vop_lock1 =		unionfs_lock,
	.vop_lookup =		unionfs_lookup,
	.vop_mkdir =		unionfs_mkdir,
	.vop_mknod =		unionfs_mknod,
	.vop_open =		unionfs_open,
	.vop_openextattr =	unionfs_openextattr,
	.vop_pathconf =		unionfs_pathconf,
	.vop_poll =		unionfs_poll,
	.vop_print =		unionfs_print,
	.vop_read =		unionfs_read,
	.vop_readdir =		unionfs_readdir,
	.vop_readlink =		unionfs_readlink,
	.vop_reclaim =		unionfs_reclaim,
	.vop_remove =		unionfs_remove,
	.vop_rename =		unionfs_rename,
	.vop_rmdir =		unionfs_rmdir,
	.vop_setacl =		unionfs_setacl,
	.vop_setattr =		unionfs_setattr,
	.vop_setextattr =	unionfs_setextattr,
	.vop_setlabel =		unionfs_setlabel,
	.vop_strategy =		unionfs_strategy,
	.vop_symlink =		unionfs_symlink,
	.vop_unlock =		unionfs_unlock,
	.vop_whiteout =		unionfs_whiteout,
	.vop_write =		unionfs_write,
	.vop_vptofh =		unionfs_vptofh,
};
