#ifndef _FTP_H_INCLUDE
#define _FTP_H_INCLUDE

#include <sys/types.h>
#include <time.h>

/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * <phk@login.dknet.dk> wrote this file.  As long as you retain this notice you
 * can do whatever you want with this stuff. If we meet some day, and you think
 * this stuff is worth it, you can buy me a beer in return.   Poul-Henning Kamp
 * ----------------------------------------------------------------------------
 *
 * Major Changelog:
 *
 * Jordan K. Hubbard
 * 17 Jan 1996
 *
 * Turned inside out. Now returns xfers as new file ids, not as a special
 * `state' of FTP_t
 *
 * $Id$
 */

/* Internal housekeeping data structure for FTP sessions */
typedef struct {
    enum { init, isopen } con_state;
    int		fd_ctrl;
    int		binary;
    int		passive;
    int		addrtype;
    char	*host;
    char	*file;
    int		errno;
    int		seek;
} *FTP_t;

/* Exported routines - deal only with FILE* type */
extern FILE	*ftpLogin(char *host, char *user, char *passwd, int port);
extern int	ftpChdir(FILE *fp, char *dir);
extern int	ftpErrno(FILE *fp);
extern size_t	ftpGetSize(FILE *fp, char *file);
extern FILE	*ftpGet(FILE *fp, char *file);
extern FILE	*ftpPut(FILE *fp, char *file);
extern int	ftpBinary(FILE *fp, int status);
extern int	ftpPassive(FILE *fp, int status);
extern int	ftpRestart(FILE *fp, int where);
extern FILE	*ftpGetURL(char *url, char *user, char *passwd);
extern FILE	*ftpPutURL(char *url, char *user, char *passwd);
extern time_t	ftpModtime(FILE *fp, char *s);

#endif	/* _FTP_H_INCLUDE */
