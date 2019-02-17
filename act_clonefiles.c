/* clonefile on apfs
 * This file is part of jdupes; see jdupes.c for license information */

#include "jdupes.h"

/* Compile out the code if no linking support is built in */
#if ENABLE_APFS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/attr.h>
#if __MAC_OS_X_VERSION_MIN_REQUIRED < 101200
#error MAC_OS_X_VERSION_MIN_REQUIRED >= 101200
#endif
#include <copyfile.h>
#include <sys/clonefile.h>
#include <fcntl.h>
#include "act_clonefiles.h"

#define COPYFILE_FLAGS (COPYFILE_CLONE_FORCE | COPYFILE_STAT | COPYFILE_ACL | COPYFILE_SECURITY | COPYFILE_XATTR | COPYFILE_METADATA | COPYFILE_DATA)

int update_times(const char * name, time_t mtime, time_t birthtime)
{
  static struct timeval times[2];
  static struct attrlist attributes;
  int success;
  times[0].tv_sec = mtime;
  times[0].tv_usec = 0;
  times[1].tv_sec = mtime;
  times[1].tv_usec = 0;
  success = utimes(name, times);
  if (success != 0) {
    return success;
  }

  attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
  attributes.reserved = 0;
  attributes.commonattr = ATTR_CMN_CRTIME;
  attributes.dirattr = 0;
  attributes.fileattr = 0;
  attributes.forkattr = 0;
  attributes.volattr = 0;

  times[0].tv_sec = birthtime;
  times[0].tv_usec = 0;

  success = setattrlist(name, &attributes, times,
                        sizeof(struct timespec), FSOPT_NOFOLLOW);
  return success;
}

extern void clonefiles(file_t *files)
{
  static file_t *tmpfile;
  static file_t *srcfile;
  static file_t *curfile;
  static file_t ** restrict dupelist;
  static unsigned int counter;
  static unsigned int max = 0;
  static unsigned int x = 0;
  static size_t name_len = 0;
  static int i, success, was_cloned;
  static copyfile_state_t cpstat;
  static file_t clonedfile;

  LOUD(fprintf(stderr, "Running clonefiles\n");)
  curfile = files;

  while (curfile) {
    if (ISFLAG(curfile->flags, F_HAS_DUPES)) {
      counter = 1;
      tmpfile = curfile->duplicates;
      while (tmpfile) {
       counter++;
       tmpfile = tmpfile->duplicates;
      }

      if (counter > max) max = counter;
    }

    curfile = curfile->next;
  }

  max++;

  dupelist = (file_t**) malloc(sizeof(file_t*) * max);

  if (!dupelist) oom("clonefiles() dupelist");

  while (files) {
    if (ISFLAG(files->flags, F_HAS_DUPES)) {
      counter = 1;
      dupelist[counter] = files;

      tmpfile = files->duplicates;

      while (tmpfile) {
       counter++;
       dupelist[counter] = tmpfile;
       tmpfile = tmpfile->duplicates;
      }

      /* Link every file to the first file */

      x = 2;
      srcfile = dupelist[1];
      if (!ISFLAG(flags, F_HIDEPROGRESS)) {
        printf("[SRC] "); fwprint(stdout, srcfile->d_name, 1);
      }
      for (; x <= counter; x++) {
        /* Can't hard link files on different devices */
        if (srcfile->device != dupelist[x]->device) {
          fprintf(stderr, "warning: hard link target on different device, not linking:\n-//-> ");
          fwprint(stderr, dupelist[x]->d_name, 1);
          continue;
        } else {
          /* The devices for the files are the same, but we still need to skip
            * anything that is already hard linked (-L and -H both set) */
          if (srcfile->inode == dupelist[x]->inode) {
            /* Don't show == arrows when not matching against other hard links */
            if (ISFLAG(flags, F_CONSIDERHARDLINKS))
              if (!ISFLAG(flags, F_HIDEPROGRESS)) {
                printf("-==-> "); fwprint(stdout, dupelist[x]->d_name, 1);
              }
          continue;
          }
        }

        /* Do not attempt to hard link files for which we don't have write access */
        if (access(dupelist[x]->d_name, W_OK) != 0)
        {
          fprintf(stderr, "warning: clonefile target is a read-only file, not clonefile:\n-//-> ");
          fwprint(stderr, dupelist[x]->d_name, 1);
          continue;
        }

        /* Check file pairs for modification before linking */
        /* Safe linking: don't actually delete until the link succeeds */
        i = file_has_changed(srcfile);
        if (i) {
          fprintf(stderr, "warning: source file modified since scanned; changing source file:\n[SRC] ");
          fwprint(stderr, dupelist[x]->d_name, 1);
          LOUD(fprintf(stderr, "file_has_changed: %d\n", i);)
          srcfile = dupelist[x];
          continue;
        }
        if (file_has_changed(dupelist[x])) {
          fprintf(stderr, "warning: target file modified since scanned, not clonefile:\n-//-> ");
          fwprint(stderr, dupelist[x]->d_name, 1);
          continue;
        }

        /* Make sure the name will fit in the buffer before trying */
        name_len = strlen(dupelist[x]->d_name) + 14;
        if (name_len > PATHBUF_SIZE) continue;
        /* Assemble a temporary file name */
        strcpy(tempname, dupelist[x]->d_name);
        strcat(tempname, ".__jdupes__.tmp");
        /* Rename the source file to the temporary name */
        i = rename(dupelist[x]->d_name, tempname);
        if (i != 0) {
          fprintf(stderr, "warning: cannot move clonefile target to a temporary name, not linking:\n-//-> ");
          fwprint(stderr, dupelist[x]->d_name, 1);
          /* Just in case the rename succeeded yet still returned an error, roll back the rename */
          rename(tempname, dupelist[x]->d_name);
          continue;
        }

        /* Create the desired hard link with the original file's name */
        errno = 0;
        success = 0;
        cpstat = copyfile_state_alloc();
        errno = copyfile(srcfile->d_name, dupelist[x]->d_name, cpstat, COPYFILE_FLAGS);
        if (errno == 0) {
          if ((copyfile_state_get(cpstat, COPYFILE_STATE_WAS_CLONED, &was_cloned) == 0) && was_cloned) {
            success = 1;
          } else {
            /* clone the file, this time using clonefileat(2) instead of copyfile(3) */
            unlink(dupelist[x]->d_name);
            errno = clonefileat(AT_FDCWD, srcfile->d_name, AT_FDCWD, dupelist[x]->d_name, 0);
            if (errno == 0) {
              success = 1;
            }
          }
        }
        copyfile_state_free(cpstat);

        if (success) {
          if (!ISFLAG(flags, F_HIDEPROGRESS)) {
            printf("----> ");
            fwprint(stdout, dupelist[x]->d_name, 1);
          }
        } else {
          /* The clonefile failed. Warn the user and put the link target back */
          if (!ISFLAG(flags, F_HIDEPROGRESS)) {
            printf("-//-> "); fwprint(stdout, dupelist[x]->d_name, 1);
          }
          fprintf(stderr, "warning: unable to clonefile '"); fwprint(stderr, dupelist[x]->d_name, 0);
          fprintf(stderr, "' -> '"); fwprint(stderr, srcfile->d_name, 0);
          fprintf(stderr, "': %s\n", strerror(errno));
          i = rename(tempname, dupelist[x]->d_name);
          if (i != 0) {
            fprintf(stderr, "error: cannot rename temp file back to original\n");
            fprintf(stderr, "original: "); fwprint(stderr, dupelist[x]->d_name, 1);
            fprintf(stderr, "current:  "); fwprint(stderr, tempname, 1);
          }
          continue;
        }

        /* Remove temporary file to clean up; if we can't, reverse the linking */
        i = remove(tempname);
        if (i != 0) {
          /* If the temp file can't be deleted, there may be a permissions problem
           * so reverse the process and warn the user */
          fprintf(stderr, "\nwarning: can't delete temp file, reverting: ");
          fwprint(stderr, tempname, 1);
          i = remove(dupelist[x]->d_name);
          /* This last error really should not happen, but we can't assume it won't */
          if (i != 0) fprintf(stderr, "\nwarning: couldn't remove clonefile to restore original file\n");
          else {
            i = rename(tempname, dupelist[x]->d_name);
            if (i != 0) {
              fprintf(stderr, "\nwarning: couldn't revert the file to its original name\n");
              fprintf(stderr, "original: "); fwprint(stderr, dupelist[x]->d_name, 1);
              fprintf(stderr, "current:  "); fwprint(stderr, tempname, 1);
            }
          }
        }

        clonedfile.flags = 0;
        clonedfile.d_name = dupelist[x]->d_name;
        if (getfilestats(&clonedfile) != 0) {
          fprintf(stderr, "\nwarning: cant stat clonedfile\n");
          fwprint(stderr, clonedfile.d_name, 1);
          continue;
        }
        if (clonedfile.mode != dupelist[x]->mode) {
          chmod(clonedfile.d_name, dupelist[x]->mode);
        }
        update_times(clonedfile.d_name, dupelist[x]->mtime, dupelist[x]->birthtime);
      }
      if (!ISFLAG(flags, F_HIDEPROGRESS)) printf("\n");
    }
    files = files->next;
  }

  free(dupelist);
  return;
}
#endif /* ENABLE_APFS */
