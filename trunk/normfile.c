#include <limits.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { Null, Lim, Dot1, Dot2, DName, Fin } state_t;

typedef struct {
    state_t nf_st;
    const char *nf_buf;
} normfile_t;

#ifdef UNIT_TEST
static void st_print(state_t st)
{
    const char *s;

    switch(st) {
	case Null: s = "Null"; break;
	case Lim: s = "Lim"; break;
	case Dot1: s = "Dot1"; break;
	case Dot2: s = "Dot2"; break;
	case DName: s = "DName"; break;
	case Fin: s = "Fin"; break;
    }

    printf("%s\n", s);
}
#endif

state_t lim_ev(const normfile_t *nf, const char **cp, char **dp)
{
    const char *c;
    char *d;
    state_t st;

    c = *cp;
    d = *dp;
    switch(nf->nf_st) {
	case Lim:
	    ++c;
	    st = Lim;
	    break;
	case Dot2:
	    /* remove last dir elem from d */
	    while(d >= nf->nf_buf && *d != '/') --d;
	    if(d != nf->nf_buf) --d;
	    if(*d != '/') ++d;
	    ++c;
	    st = Lim;
	    break;
	case Dot1:
	    ++c;
	    st = Lim;
	    break;
	case DName:
	default:
	    ++c;
	    st = Lim;
	    break;

    }
    *cp = c;
    *dp = d;

    return st;
}

state_t dot_ev(const normfile_t *nf, const char **cp, char **dp)
{
    const char *c;
    char *d;
    state_t st;

    c = *cp;
    d = *dp;
    switch(nf->nf_st) {
	case Null:
	    st = Dot1;
	    ++c;
	    break;
	case Dot1:
	    st = Dot2;
	    ++c;
	    break;
	case Dot2:
	    *d = '.';
	    ++d;
	    *d = '.';
	    ++d;
	    *d = *c;
	    ++c;
	    ++d;
	    st = DName;
	    break;
	case Fin:
	case Lim:
	    ++c;
	    st = Dot1;
	    break;
	case DName:
	default:
	    *d = *c;
	    ++c;
	    ++d;
	    st = DName;
	    break;
    }
    *cp = c;
    *dp = d;

    return st;
}

state_t char_ev(const normfile_t *nf, const char **cp, char **dp)
{
    const char *c;
    char *d;
    state_t st;

    c = *cp;
    d = *dp;
    switch(nf->nf_st) {
	case Fin:
	case Lim:
	    *d = '/';
	    ++d;
	    *d = *c;
	    ++c;
	    ++d;
	    st = DName;
	    break;
	case Dot2:
	    *d = '/';
	    ++d;
	    *d = '.';
	    ++d;
	    *d = '.';
	    ++d;
	    *d = *c;
	    ++c;
	    ++d;
	    st = DName;
	    break;
	case Dot1:
	    *d = '/';
	    ++d;
	    *d = '.';
	    ++d;
	    *d = *c;
	    ++d;
	    ++c;
	    st = DName;
	    break;
	case Null:
	case DName:
	    *d = *c;
	    ++d;
	    ++c;
	    st = DName;
	    break;
    }
    *cp = c;
    *dp = d;

    return st;
}

state_t null_ev(const normfile_t *nf, const char **cp, char **dp)
{
    const char *c;
    char *d;
    state_t st;

    c = *cp;
    d = *dp;
    switch(nf->nf_st) {
	case Fin:
	case Null:
	case Dot1:
	case DName:
	    *d = *c;
	    st = Fin;
	    break;
	case Lim:
	    ++d;
	    *d = *c;
	    st = Fin;
	    break;
	case Dot2:
	    while(d != nf->nf_buf && *d != '/') --d;
	    if(d == nf->nf_buf) {
		++d;
	    }
	    *d = *c;
	    st = Fin;
	    break;
    }

    *cp = c;
    *dp = d;

    return st;
}
char *normalize(const char *path, char *normpath)
{
    const char *c;
    char *d;
    state_t st;
    normfile_t nf;
    char buf[PATH_MAX];

    if(realpath(path, normpath) != NULL) {
	return normpath;
    }

    nf.nf_st = Null;
    nf.nf_buf = normpath;

    c = path;
    if(*c != '/') {
	getcwd(buf, PATH_MAX);
	d = buf + strlen(buf);
	*d = '/';
	++d;
	strcat(d, path);
	c = buf;
    }
    d = normpath;
    st = Null;
    while(st != Fin) {
	switch(*c) {
	    case '/':
		st = lim_ev(&nf, &c, &d);
		break;
	    case '.':
		st = dot_ev(&nf, &c, &d);
		break;
	    case '\0':
		st = null_ev(&nf, &c, &d);
		break;
	    default:
		st = char_ev(&nf, &c, &d);
		break;
	}
	nf.nf_st = st;
    }

    return normpath;
}

#ifdef UNIT_TEST
int main(int argc, char **argv)
{
    char buf[PATH_MAX];
    int rv;

    rv = normalize(argv[1], buf);
    printf("<%s> -> <%s>\n", argv[1], buf);

    return rv;
}
#endif
