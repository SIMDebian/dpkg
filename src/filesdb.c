/*
 * dpkg - main program for package management
 * filesdb.c - management of database of files installed on system
 *
 * Copyright © 1995 Ian Jackson <ijackson@chiark.greenend.org.uk>
 * Copyright © 2000,2001 Wichert Akkerman <wakkerma@debian.org>
 * Copyright © 2008-2014 Guillem Jover <guillem@debian.org>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <compat.h>

#ifdef HAVE_LINUX_FIEMAP_H
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/vfs.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <pwd.h>
#include <grp.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

#include <dpkg/i18n.h>
#include <dpkg/dpkg.h>
#include <dpkg/dpkg-db.h>
#include <dpkg/string.h>
#include <dpkg/path.h>
#include <dpkg/dir.h>
#include <dpkg/fdio.h>
#include <dpkg/pkg-array.h>
#include <dpkg/progress.h>

#include "filesdb.h"
#include "infodb.h"
#include "main.h"

/*** filepackages support for tracking packages owning a file. ***/

struct filepackages_iterator {
  struct pkg_list *pkg_node;
};

struct filepackages_iterator *
filepackages_iter_new(struct filenamenode *fnn)
{
  struct filepackages_iterator *iter;

  iter = m_malloc(sizeof(*iter));
  iter->pkg_node = fnn->packages;

  return iter;
}

struct pkginfo *
filepackages_iter_next(struct filepackages_iterator *iter)
{
  struct pkg_list *pkg_node;

  if (iter->pkg_node == NULL)
    return NULL;

  pkg_node = iter->pkg_node;
  iter->pkg_node = pkg_node->next;

  return pkg_node->pkg;
}

void
filepackages_iter_free(struct filepackages_iterator *iter)
{
  free(iter);
}

/*** Generic data structures and routines. ***/

static bool allpackagesdone = false;
static int nfiles= 0;

void
ensure_package_clientdata(struct pkginfo *pkg)
{
  if (pkg->clientdata)
    return;
  pkg->clientdata = nfmalloc(sizeof(struct perpackagestate));
  pkg->clientdata->istobe = PKG_ISTOBE_NORMAL;
  pkg->clientdata->color = PKG_CYCLE_WHITE;
  pkg->clientdata->enqueued = false;
  pkg->clientdata->fileslistvalid = false;
  pkg->clientdata->files = NULL;
  pkg->clientdata->replacingfilesandsaid = 0;
  pkg->clientdata->cmdline_seen = 0;
  pkg->clientdata->listfile_phys_offs = 0;
  pkg->clientdata->trigprocdeferred = NULL;
}

void note_must_reread_files_inpackage(struct pkginfo *pkg) {
  allpackagesdone = false;
  ensure_package_clientdata(pkg);
  pkg->clientdata->fileslistvalid = false;
}

enum pkg_filesdb_load_status {
  PKG_FILESDB_LOAD_NONE = 0,
  PKG_FILESDB_LOAD_INPROGRESS = 1,
  PKG_FILESDB_LOAD_DONE = 2,
};

static enum pkg_filesdb_load_status saidread = PKG_FILESDB_LOAD_NONE;

/**
 * Erase the files saved in pkg.
 */
static void
pkg_files_blank(struct pkginfo *pkg)
{
  struct fileinlist *current;

  /* Anything to empty? */
  if (!pkg->clientdata)
    return;

  for (current= pkg->clientdata->files;
       current;
       current= current->next) {
    struct pkg_list *pkg_node, *pkg_prev = NULL;

    /* For each file that used to be in the package,
     * go through looking for this package's entry in the list
     * of packages containing this file, and blank it out. */
    for (pkg_node = current->namenode->packages;
         pkg_node;
         pkg_node = pkg_node->next) {
      if (pkg_node->pkg == pkg) {
        if (pkg_prev)
          pkg_prev->next = pkg_node->next;
        else
          current->namenode->packages = pkg_node->next;

        /* The actual filelist links were allocated using nfmalloc, so
         * we shouldn't free them. */
        break;
      }
      pkg_prev = pkg_node;
    }
  }
  pkg->clientdata->files = NULL;
}

static struct fileinlist **
pkg_files_add_file(struct pkginfo *pkg, struct filenamenode *namenode,
                   struct fileinlist **file_tail)
{
  struct fileinlist *newent;
  struct pkg_list *pkg_node;

  ensure_package_clientdata(pkg);

  if (file_tail == NULL)
    file_tail = &pkg->clientdata->files;

  /* Make sure we're at the end. */
  while ((*file_tail) != NULL) {
    file_tail = &((*file_tail)->next);
  }

  /* Create a new node. */
  newent = nfmalloc(sizeof(struct fileinlist));
  newent->namenode = namenode;
  newent->next = NULL;
  *file_tail = newent;
  file_tail = &newent->next;

  /* Add pkg to newent's package list. */
  pkg_node = nfmalloc(sizeof(*pkg_node));
  pkg_node->pkg = pkg;
  pkg_node->next = newent->namenode->packages;
  newent->namenode->packages = pkg_node;

  /* Return the position for the next guy. */
  return file_tail;
}

/**
 * Load the list of files in this package into memory, or update the
 * list if it is there but stale.
 */
void
ensure_packagefiles_available(struct pkginfo *pkg)
{
  static int fd;
  const char *filelistfile;
  struct fileinlist **lendp;
  struct stat stat_buf;
  char *loaded_list, *loaded_list_end, *thisline, *nextline, *ptr;

  if (pkg->clientdata && pkg->clientdata->fileslistvalid)
    return;
  ensure_package_clientdata(pkg);

  /* Throw away any stale data, if there was any. */
  pkg_files_blank(pkg);

  /* Packages which aren't installed don't have a files list. */
  if (pkg->status == PKG_STAT_NOTINSTALLED) {
    pkg->clientdata->fileslistvalid = true;
    return;
  }

  filelistfile = pkg_infodb_get_file(pkg, &pkg->installed, LISTFILE);

  onerr_abort++;

  fd= open(filelistfile,O_RDONLY);

  if (fd==-1) {
    if (errno != ENOENT)
      ohshite(_("unable to open files list file for package '%.250s'"),
              pkg_name(pkg, pnaw_nonambig));
    onerr_abort--;
    if (pkg->status != PKG_STAT_CONFIGFILES &&
        dpkg_version_is_informative(&pkg->configversion)) {
      warning(_("files list file for package '%.250s' missing; assuming "
                "package has no files currently installed"),
              pkg_name(pkg, pnaw_nonambig));
    }
    pkg->clientdata->files = NULL;
    pkg->clientdata->fileslistvalid = true;
    return;
  }

  push_cleanup(cu_closefd, ehflag_bombout, NULL, 0, 1, &fd);

  if (fstat(fd, &stat_buf))
    ohshite(_("unable to stat files list file for package '%.250s'"),
            pkg_name(pkg, pnaw_nonambig));

  if (!S_ISREG(stat_buf.st_mode))
    ohshit(_("files list for package '%.250s' is not a regular file"),
           pkg_name(pkg, pnaw_nonambig));

  if (stat_buf.st_size) {
    loaded_list = nfmalloc(stat_buf.st_size);
    loaded_list_end = loaded_list + stat_buf.st_size;

    if (fd_read(fd, loaded_list, stat_buf.st_size) < 0)
      ohshite(_("reading files list for package '%.250s'"),
              pkg_name(pkg, pnaw_nonambig));

    lendp= &pkg->clientdata->files;
    thisline = loaded_list;
    while (thisline < loaded_list_end) {
      struct filenamenode *namenode;

      ptr = memchr(thisline, '\n', loaded_list_end - thisline);
      if (ptr == NULL)
        ohshit(_("files list file for package '%.250s' is missing final newline"),
               pkg_name(pkg, pnaw_nonambig));
      /* Where to start next time around. */
      nextline = ptr + 1;
      /* Strip trailing ‘/’. */
      if (ptr > thisline && ptr[-1] == '/') ptr--;
      /* Add the file to the list. */
      if (ptr == thisline)
        ohshit(_("files list file for package '%.250s' contains empty filename"),
               pkg_name(pkg, pnaw_nonambig));
      *ptr = '\0';

      namenode = findnamenode(thisline, fnn_nocopy);
      lendp = pkg_files_add_file(pkg, namenode, lendp);
      thisline = nextline;
    }
  }
  pop_cleanup(ehflag_normaltidy); /* fd = open() */
  if (close(fd))
    ohshite(_("error closing files list file for package '%.250s'"),
            pkg_name(pkg, pnaw_nonambig));

  onerr_abort--;

  pkg->clientdata->fileslistvalid = true;
}

#if defined(HAVE_LINUX_FIEMAP_H)
static int
pkg_sorter_by_listfile_phys_offs(const void *a, const void *b)
{
  const struct pkginfo *pa = *(const struct pkginfo **)a;
  const struct pkginfo *pb = *(const struct pkginfo **)b;

  /* We can't simply subtract, because the difference may be greater than
   * INT_MAX. */
  if (pa->clientdata->listfile_phys_offs < pb->clientdata->listfile_phys_offs)
    return -1;
  else if (pa->clientdata->listfile_phys_offs > pb->clientdata->listfile_phys_offs)
    return 1;
  else
    return 0;
}

static void
pkg_files_optimize_load(struct pkg_array *array)
{
  struct statfs fs;
  int i;

  /* Get the filesystem block size. */
  if (statfs(pkg_infodb_get_dir(), &fs) < 0)
    return;

  /* Sort packages by the physical location of their list files, so that
   * scanning them later will minimize disk drive head movements. */
  for (i = 0; i < array->n_pkgs; i++) {
    struct pkginfo *pkg = array->pkgs[i];
    struct {
      struct fiemap fiemap;
      struct fiemap_extent extent;
    } fm;
    const char *listfile;
    int fd;

    ensure_package_clientdata(pkg);

    if (pkg->status == PKG_STAT_NOTINSTALLED ||
        pkg->clientdata->listfile_phys_offs != 0)
      continue;

    pkg->clientdata->listfile_phys_offs = -1;

    listfile = pkg_infodb_get_file(pkg, &pkg->installed, LISTFILE);

    fd = open(listfile, O_RDONLY);
    if (fd < 0)
      continue;

    memset(&fm, 0, sizeof(fm));
    fm.fiemap.fm_start = 0;
    fm.fiemap.fm_length = fs.f_bsize;
    fm.fiemap.fm_flags = 0;
    fm.fiemap.fm_extent_count = 1;

    if (ioctl(fd, FS_IOC_FIEMAP, (unsigned long)&fm) == 0)
      pkg->clientdata->listfile_phys_offs = fm.fiemap.fm_extents[0].fe_physical;

    close(fd);
  }

  pkg_array_sort(array, pkg_sorter_by_listfile_phys_offs);
}
#elif defined(HAVE_POSIX_FADVISE)
static void
pkg_files_optimize_load(struct pkg_array *array)
{
  int i;

  /* Ask the kernel to start preloading the list files, so as to get a
   * boost when later we actually load them. */
  for (i = 0; i < array->n_pkgs; i++) {
    struct pkginfo *pkg = array->pkgs[i];
    const char *listfile;
    int fd;

    listfile = pkg_infodb_get_file(pkg, &pkg->installed, LISTFILE);

    fd = open(listfile, O_RDONLY | O_NONBLOCK);
    if (fd != -1) {
      posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED);
      close(fd);
    }
  }
}
#else
static void
pkg_files_optimize_load(struct pkg_array *array)
{
}
#endif

void ensure_allinstfiles_available(void) {
  struct pkg_array array;
  struct pkginfo *pkg;
  struct progress progress;
  int i;

  if (allpackagesdone) return;
  if (saidread < PKG_FILESDB_LOAD_DONE) {
    int max = pkg_db_count_pkg();

    saidread = PKG_FILESDB_LOAD_INPROGRESS;
    progress_init(&progress, _("(Reading database ... "), max);
  }

  pkg_array_init_from_db(&array);

  pkg_files_optimize_load(&array);

  for (i = 0; i < array.n_pkgs; i++) {
    pkg = array.pkgs[i];
    ensure_packagefiles_available(pkg);

    if (saidread == PKG_FILESDB_LOAD_INPROGRESS)
      progress_step(&progress);
  }

  pkg_array_destroy(&array);

  allpackagesdone = true;

  if (saidread == PKG_FILESDB_LOAD_INPROGRESS) {
    progress_done(&progress);
    printf(P_("%d file or directory currently installed.)\n",
              "%d files and directories currently installed.)\n", nfiles),
           nfiles);
    saidread = PKG_FILESDB_LOAD_DONE;
  }
}

void ensure_allinstfiles_available_quiet(void) {
  saidread = PKG_FILESDB_LOAD_DONE;
  ensure_allinstfiles_available();
}

/*
 * If mask is nonzero, will not write any file whose filenamenode
 * has any flag bits set in mask.
 */
void
write_filelist_except(struct pkginfo *pkg, struct pkgbin *pkgbin,
                      struct fileinlist *list, enum filenamenode_flags mask)
{
  struct atomic_file *file;
  struct fileinlist *node;
  const char *listfile;

  listfile = pkg_infodb_get_file(pkg, pkgbin, LISTFILE);

  file = atomic_file_new(listfile, 0);
  atomic_file_open(file);

  for (node = list; node; node = node->next) {
    if (!(mask && (node->namenode->flags & mask))) {
      fputs(node->namenode->name, file->fp);
      putc('\n', file->fp);
    }
  }

  atomic_file_sync(file);
  atomic_file_close(file);
  atomic_file_commit(file);
  atomic_file_free(file);

  dir_sync_path(pkg_infodb_get_dir());

  note_must_reread_files_inpackage(pkg);
}

/*
 * Initializes an iterator that appears to go through the file
 * list ‘files’ in reverse order, returning the namenode from
 * each. What actually happens is that we walk the list here,
 * building up a reverse list, and then peel it apart one
 * entry at a time.
 */
void
reversefilelist_init(struct reversefilelistiter *iter, struct fileinlist *files)
{
  struct fileinlist *newent;

  iter->todo = NULL;
  while (files) {
    newent= m_malloc(sizeof(struct fileinlist));
    newent->namenode= files->namenode;
    newent->next = iter->todo;
    iter->todo = newent;
    files= files->next;
  }
}

struct filenamenode *
reversefilelist_next(struct reversefilelistiter *iter)
{
  struct filenamenode *ret;
  struct fileinlist *todo;

  todo = iter->todo;
  if (!todo)
    return NULL;
  ret= todo->namenode;
  iter->todo = todo->next;
  free(todo);
  return ret;
}

/*
 * Clients must call this function to clean up the reversefilelistiter
 * if they wish to break out of the iteration before it is all done.
 * Calling this function is not necessary if reversefilelist_next has
 * been called until it returned 0.
 */
void
reversefilelist_abort(struct reversefilelistiter *iter)
{
  while (reversefilelist_next(iter));
}

struct fileiterator {
  struct filenamenode *namenode;
  int nbinn;
};

/* This must always be a prime for optimal performance.
 * This is the closest one to 2^18 (262144). */
#define BINS 262139

static struct filenamenode *bins[BINS];

struct fileiterator *
files_db_iter_new(void)
{
  struct fileiterator *iter;

  iter = m_malloc(sizeof(struct fileiterator));
  iter->namenode = NULL;
  iter->nbinn = 0;

  return iter;
}

struct filenamenode *
files_db_iter_next(struct fileiterator *iter)
{
  struct filenamenode *r= NULL;

  while (!iter->namenode) {
    if (iter->nbinn >= BINS)
      return NULL;
    iter->namenode = bins[iter->nbinn++];
  }
  r = iter->namenode;
  iter->namenode = r->next;

  return r;
}

void
files_db_iter_free(struct fileiterator *iter)
{
  free(iter);
}

void filesdbinit(void) {
  struct filenamenode *fnn;
  int i;

  for (i=0; i<BINS; i++)
    for (fnn= bins[i]; fnn; fnn= fnn->next) {
      fnn->flags= 0;
      fnn->oldhash = NULL;
      fnn->newhash = EMPTYHASHFLAG;
      fnn->filestat = NULL;
    }
}

void
files_db_reset(void)
{
  int i;

  for (i = 0; i < BINS; i++)
    bins[i] = NULL;
}

struct filenamenode *findnamenode(const char *name, enum fnnflags flags) {
  struct filenamenode **pointerp, *newnode;
  const char *orig_name = name;

  /* We skip initial slashes and ‘./’ pairs, and add our own single
   * leading slash. */
  name = path_skip_slash_dotslash(name);

  pointerp = bins + (str_fnv_hash(name) % (BINS));
  while (*pointerp) {
    /* XXX: Why is the assert needed? It's checking already added entries. */
    assert((*pointerp)->name[0] == '/');
    if (strcmp((*pointerp)->name + 1, name) == 0)
      break;
    pointerp= &(*pointerp)->next;
  }
  if (*pointerp) return *pointerp;

  if (flags & fnn_nonew)
    return NULL;

  newnode= nfmalloc(sizeof(struct filenamenode));
  newnode->packages = NULL;
  if((flags & fnn_nocopy) && name > orig_name && name[-1] == '/')
    newnode->name = name - 1;
  else {
    char *newname= nfmalloc(strlen(name)+2);
    newname[0]= '/'; strcpy(newname+1,name);
    newnode->name= newname;
  }
  newnode->flags= 0;
  newnode->next = NULL;
  newnode->divert = NULL;
  newnode->statoverride = NULL;
  newnode->oldhash = NULL;
  newnode->newhash = EMPTYHASHFLAG;
  newnode->filestat = NULL;
  newnode->trig_interested = NULL;
  *pointerp= newnode;
  nfiles++;

  return newnode;
}
