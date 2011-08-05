/*
 * Logger, configurable PMDA
 *
 * Copyright (c) 1995,2004 Silicon Graphics, Inc.  All Rights Reserved.
 * Copyright (c) 2011 Nathan Scott.  All Rights Reserved.
 * Copyright (c) 2011 Red Hat Inc.
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * Debug options
 * APPL0	configfile processing and PMNS setup
 * APPL1	loading event data from the log files
 * APPL2	interaction with PMCD
 */

#include <ctype.h>
#include <sys/stat.h>
#include "percontext.h"
#include "domain.h"
#include "event.h"
#include "util.h"

/*
 * Logger PMDA
 *
 * This PMDA is a sample that illustrates how a logger PMDA might be
 * constructed using libpcp_pmda.
 *
 * Although the metrics supported are logger, the framework is quite general,
 * and could be extended to implement a much more complex PMDA.
 *
 * Metrics
 *	logger.numclients			- number of attached clients
 *	logger.numlogfiles			- number of monitored logfiles
 *	logger.param_string			- string event data
 *	logger.perfile.{LOGFILE}.count		- observed event count
 *	logger.perfile.{LOGFILE}.bytes		- observed events size
 *	logger.perfile.{LOGFILE}.size		- logfile size
 *	logger.perfile.{LOGFILE}.path		- logfile path
 *	logger.perfile.{LOGFILE}.numclients	- number of attached
 *						  clients/logfile
 *	logger.perfile.{LOGFILE}.records	- event records/logfile
 */

#define DEFAULT_MAXMEM	(2 * 1024 * 1024)	/* 2 megabytes */
#define max(a,b)	((a > b) ? a : b)
long maxmem;

int maxfd;
fd_set fds;
static int interval_expired;
static struct timeval interval = { 2, 0 };

static int nummetrics;
static __pmnsTree *pmns;

struct dynamic_metric_info {
    int		logfile;
    int		pmid_index;
    const char *help_text;
};
static struct dynamic_metric_info *dynamic_metric_infotab;

/*
 * all metrics supported in this PMDA - one table entry for each
 */

static pmdaMetric dynamic_metrictab[] = {
/* perfile.{LOGFILE}.count */
    { NULL, 				/* m_user gets filled in later */
      { 0 /* pmid gets filled in later */, PM_TYPE_U32, PM_INDOM_NULL,
	PM_SEM_COUNTER, PMDA_PMUNITS(0,0,1,0,0,PM_COUNT_ONE) }, },
/* perfile.{LOGFILE}.bytes */
    { NULL, 				/* m_user gets filled in later */
      { 0 /* pmid gets filled in later */, PM_TYPE_U64, PM_INDOM_NULL,
	PM_SEM_COUNTER, PMDA_PMUNITS(1,0,0,PM_SPACE_BYTE,0,0) }, },
/* perfile.{LOGFILE}.size */
    { NULL, 				/* m_user gets filled in later */
      { 0 /* pmid gets filled in later */, PM_TYPE_U64, PM_INDOM_NULL,
	PM_SEM_INSTANT, PMDA_PMUNITS(1,0,0,PM_SPACE_BYTE,0,0) }, },
/* perfile.{LOGFILE}.path */
    { NULL, 				/* m_user gets filled in later */
      { 0 /* pmid gets filled in later */, PM_TYPE_STRING, PM_INDOM_NULL,
	PM_SEM_DISCRETE, PMDA_PMUNITS(0,0,0,0,0,0) }, },
/* perfile.{LOGFILE}.numclients */
    { NULL, 				/* m_user gets filled in later */
      { 0 /* pmid gets filled in later */, PM_TYPE_U32, PM_INDOM_NULL,
	PM_SEM_INSTANT, PMDA_PMUNITS(0,0,1,0,0,PM_COUNT_ONE) }, },
/* perfile.{LOGFILE}.records */
    { NULL, 				/* m_user gets filled in later */
      { 0 /* pmid gets filled in later */, PM_TYPE_EVENT, PM_INDOM_NULL,
	PM_SEM_INSTANT, PMDA_PMUNITS(0,0,0,0,0,0) }, },
/* perfile.{LOGFILE}.queuemem */
    { NULL, 				/* m_user gets filled in later */
      { 0 /* pmid gets filled in later */, PM_TYPE_U64, PM_INDOM_NULL,
	PM_SEM_INSTANT, PMDA_PMUNITS(1,0,0,PM_SPACE_BYTE,0,0) }, },
};

static char *dynamic_nametab[] = {
/* perfile.{LOGFILE}.count */
    "count",
/* perfile.{LOGFILE}.bytes */
    "bytes",
/* perfile.{LOGFILE}.size */
    "size",
/* perfile.{LOGFILE}.path */
    "path",
/* perfile.{LOGFILE}.numclients */
    "numclients",
/* perfile.{LOGFILE}.records */
    "records",
/* perfile.{LOGFILE}.queuemem */
    "queuemem",
};

static const char *dynamic_helptab[] = {
/* perfile.{LOGFILE}.count */
    "The cumulative number of events seen for this logfile.",
/* perfile.{LOGFILE}.bytes */
    "Cumulative number of bytes in events seen for this logfile.",
/* perfile.{LOGFILE}.size */
    "The current size of this logfile.",
/* perfile.{LOGFILE}.path */
    "The path for this logfile.",
/* perfile.{LOGFILE}.numclients */
    "The number of attached clients for this logfile.",
/* perfile.{LOGFILE}.records */
    "Event records for this logfile.",
/* perfile.{LOGFILE}.queuemem */
    "Amount of memory used for event data.",
};

static pmdaMetric static_metrictab[] = {
/* numclients */
    { NULL, 
      { PMDA_PMID(0,0), PM_TYPE_U32, PM_INDOM_NULL, PM_SEM_DISCRETE, 
    	PMDA_PMUNITS(0,0,1,0,0,PM_COUNT_ONE) }, },
/* numlogfiles */
    { NULL,
      { PMDA_PMID(0,1), PM_TYPE_U32, PM_INDOM_NULL, PM_SEM_DISCRETE, 
    	PMDA_PMUNITS(0,0,1,0,0,PM_COUNT_ONE) }, },
/* param_string */
    { NULL,
      { PMDA_PMID(0,2), PM_TYPE_STRING, PM_INDOM_NULL, PM_SEM_INSTANT,
	PMDA_PMUNITS(0,0,0,0,0,0) }, },
/* perfile.maxmem */
    { NULL, 				/* m_user gets filled in later */
      { PMDA_PMID(0,3), PM_TYPE_U64, PM_INDOM_NULL, PM_SEM_DISCRETE,
	PMDA_PMUNITS(1,0,0,PM_SPACE_BYTE,0,0) }, },
};

static pmdaMetric *metrictab;

void
logger_end_contextCallBack(int ctx)
{
    ctx_end(ctx);
}

static int
logger_profile(__pmProfile *prof, pmdaExt *pmda)
{
    ctx_active(pmda->e_context);
    return 0;
}

static void
logger_reload(void)
{
    struct stat pathstat;
    int i, fd, sts;

    for (i = 0; i < numlogfiles; i++) {
	if (logfiles[i].pid > 0)	/* process pipe */
	    goto events;
	if (stat(logfiles[i].pathname, &pathstat) < 0) {
	    if (logfiles[i].fd >= 0) {
		close(logfiles[i].fd);
		logfiles[i].fd = -1;
	    }
	    memset(&logfiles[i].pathstat, 0, sizeof(logfiles[i].pathstat));
	} else {
	    /* reopen if no descriptor before, or log rotated (new file) */
	    if (logfiles[i].fd < 0 ||
	        logfiles[i].pathstat.st_ino != pathstat.st_ino ||
		logfiles[i].pathstat.st_dev != pathstat.st_dev) {
		if (logfiles[i].fd < 0)
		    close(logfiles[i].fd);
		fd = open(logfiles[i].pathname, O_RDONLY|O_NONBLOCK);
		if (fd < 0 && logfiles[i].fd >= 0)	/* log once */
		    __pmNotifyErr(LOG_ERR, "open: %s - %s",
				logfiles[i].pathname, strerror(errno));
		logfiles[i].fd = fd;
	    } else {
		if ((S_ISREG(pathstat.st_mode)) &&
		    (memcmp(&logfiles[i].pathstat.st_mtime, &pathstat.st_mtime,
			    sizeof(pathstat.st_mtime))) == 0)
		    continue;
	    }
	    logfiles[i].pathstat = pathstat;
events:
	    do {
		sts = event_create(i);
	    } while (sts != 0);
	}
    }
}

static int
logger_fetch(int numpmid, pmID pmidlist[], pmResult **resp, pmdaExt *pmda)
{
    ctx_active(pmda->e_context);
    return pmdaFetch(numpmid, pmidlist, resp, pmda);
}

static int
valid_pmid(unsigned int cluster, unsigned int item)
{
    if (cluster != 0 || (item < 0 || item > nummetrics)) {
	if (pmDebug & DBG_TRACE_APPL0)
	    __pmNotifyErr(LOG_ERR, "%s: PM_ERR_PMID (cluster %u, item %u)\n",
		      __FUNCTION__, cluster, item);
	return PM_ERR_PMID;
    }
    return 0;
}

/*
 * callback provided to pmdaFetch
 */
static int
logger_fetchCallBack(pmdaMetric *mdesc, unsigned int inst, pmAtomValue *atom)
{
    __pmID_int *idp = (__pmID_int *)&(mdesc->m_desc.pmid);
    int		sts;

    if ((sts = valid_pmid(idp->cluster, idp->item)) < 0)
	return sts;

    sts = PMDA_FETCH_STATIC;
    if (idp->item < 4) {
	switch (idp->item) {
	  case 0:			/* logger.numclients */
	    atom->ul = ctx_get_num();
	    break;
	  case 1:			/* logger.numlogfiles */
	    atom->ul = numlogfiles;
	    break;
	  case 2:			/* logger.param_string */
	    sts = PMDA_FETCH_NOVALUES;
	    break;
	  case 3:			/* logger.maxmem */
	    atom->ull = (unsigned long long)maxmem;
	    break;
	  default:
	    return PM_ERR_PMID;
	}
    }
    else {
	struct dynamic_metric_info *pinfo;

	if ((pinfo = ((mdesc != NULL) ? mdesc->m_user : NULL)) == NULL)
	    return PM_ERR_PMID;

	switch (pinfo->pmid_index) {
	  case 0:			/* perfile.{LOGFILE}.count */
	    atom->ul = logfiles[pinfo->logfile].count;
	    break;
	  case 1:			/* perfile.{LOGFILE}.bytes */
	    atom->ull = logfiles[pinfo->logfile].bytes;
	    break;
	  case 2:			/* perfile.{LOGFILE}.size */
	    atom->ull = logfiles[pinfo->logfile].pathstat.st_size;
	    break;
	  case 3:			/* perfile.{LOGFILE}.path */
	    atom->cp = logfiles[pinfo->logfile].pathname;
	    break;
	  case 4:			/* perfile.{LOGFILE}.numclients */
	    atom->ul = event_get_clients_per_logfile(pinfo->logfile);
	    break;
	  case 5:			/* perfile.{LOGFILE}.records */
	    if ((sts = event_fetch(&atom->vbp, pinfo->logfile)) != 0)
		return sts;
	    sts = atom->vbp == NULL ? PMDA_FETCH_NOVALUES : PMDA_FETCH_STATIC;
	    break;
	  case 6:			/* perfile.{LOGFILE}.queuemem */
	    atom->ull = logfiles[pinfo->logfile].queuesize;
	    break;
	  default:
	    return PM_ERR_PMID;
	}
    }
    return sts;
}

static int
logger_store(pmResult *result, pmdaExt *pmda)
{
    int		i, j, sts;
    pmValueSet	*vsp;
    __pmID_int	*idp;

    ctx_active(pmda->e_context);

    for (i = 0; i < result->numpmid; i++) {
	vsp = result->vset[i];
	idp = (__pmID_int *)&vsp->pmid;

	if ((sts = valid_pmid(idp->cluster, idp->item)) < 0)
	    return sts;
	else {
	    struct dynamic_metric_info *pinfo = NULL;
	    const char *filter;

	    for (j = 0; j < pmda->e_nmetrics; j++) {
		if (vsp->pmid == pmda->e_metrics[j].m_desc.pmid) {
		    pinfo = pmda->e_metrics[j].m_user;
		    break;
		}
	    }
	    if (pinfo == NULL)
		return PM_ERR_PMID;
	    if (pinfo->pmid_index != 5)
		return PM_ERR_PERMISSION;
	    if (vsp->numval != 1 || vsp->valfmt != PM_VAL_SPTR)
		return PM_ERR_CONV;

	    filter = vsp->vlist[0].value.pval->vbuf;
	    if ((sts = event_regex(filter)) < 0)
		return sts;
	}
    }
    return 0;
}

/* Ensure potential PMNS name can be used as a PCP namespace entry. */
static int
valid_pmns_name(char *name)
{
    if (!isalpha(name[0]))
	return 0;
    for (; *name != '\0'; name++)
	if (!isalnum(*name) && *name != '_')
	    return 0;
    return 1;
}

/*
 * Handle the config file.
 */
static int
read_config(const char *filename)
{
    FILE	       *configFile;
    struct EventFileData *data;
    int			sts = 0;
    size_t		len;
    char		line[MAXPATHLEN * 2];
    char	       *ptr, *name, *restrict;

    configFile = fopen(filename, "r");
    if (configFile == NULL) {
	fprintf(stderr, "%s: %s: %s\n", __FUNCTION__, filename,
		strerror(errno));
	return -1;
    }

    while (!feof(configFile)) {
	if (fgets(line, sizeof(line), configFile) == NULL) {
	    if (feof(configFile)) {
		break;
	    }
	    else {
		fprintf(stderr, "%s: fgets failed: %s\n", __FUNCTION__,
			strerror(errno));
		sts = -1;
		break;
	    }
	}

	/* fgets() puts the '\n' at the end of the buffer.  Remove
	 * it.  If it isn't there, that must mean that the line is
	 * longer than our buffer. */
	len = strlen(line);
	if (len == 0) {			/* Ignore empty string. */
	    continue;
	}
	else if (line[len - 1] != '\n') { /* String must be too long */
	    fprintf(stderr, "%s: config file line too long: %s\n",
		    __FUNCTION__, line);
	    sts = -1;
	    break;
	}
	line[len - 1] = '\0';		/* Remove the '\n'. */

	/* Strip all trailing whitespace. */
	rstrip(line);

	/* If the string is now empty or a comment, just ignore the line. */
	len = strlen(line);
	if (len == 0) {
	    continue;
	} else if (line[0] == '#') {
	    continue;
	}

	/* Skip past all leading whitespace to find the start of
	 * NAME. */
	ptr = name = lstrip(line);

	/* Now we need to split the line into 3 parts: NAME, ACCESS
	 * and PATHNAME.  NAME can't have whitespace in it, so look
	 * for the first non-whitespace. */
	while (*ptr != '\0' && ! isspace(*ptr)) {
	    ptr++;
	}
	/* If we're at the end, we didn't find any whitespace, so
	 * we've only got a NAME, with no ACCESS/PATHNAME. */
	if (*ptr == '\0') {
	    fprintf(stderr, "%s: badly formatted config file line: %s\n",
		    __FUNCTION__, line);
	    sts = -1;
	    break;
	}
	/* Terminate NAME at the 1st whitespace. */
	*ptr++ = '\0';

	/* Make sure NAME isn't too long. */
	if (strlen(name) > MAXPATHLEN) {
	    fprintf(stderr, "%s: NAME is too long: %s\n",
		    __FUNCTION__, name);
	    sts = -1;
	    break;
	}

	/* Make sure NAME is valid. */
	if (valid_pmns_name(name) == 0) {
	    fprintf(stderr, "%s: NAME isn't a valid PMNS name: %s\n",
		    __FUNCTION__, name);
	    sts = -1;
	    break;
	}

	/* Skip past any extra whitespace between NAME and ACCESS */
	ptr = restrict = lstrip(ptr);

	/* Look for the next whitespace, and that terminate ACCESS */
	while (*ptr != '\0' && ! isspace(*ptr)) {
	    ptr++;
	}

	/* If we're at the end, we didn't find any whitespace, so
	 * we've only got NAME and ACCESS with no/PATHNAME. */
	if (*ptr == '\0') {
	    fprintf(stderr, "%s: badly formatted config file line: %s\n",
		    __FUNCTION__, line);
	    sts = -1;
	    break;
	}
	/* Terminate ACCESS at the 1st whitespace. */
	*ptr++ = '\0';

	/* Skip past any extra whitespace between ACCESS and PATHNAME */
	ptr = lstrip(ptr);

	/* Make sure PATHNAME (the rest of the line) isn't too long. */
	if (strlen(ptr) > MAXPATHLEN) {
	    fprintf(stderr, "%s: PATHNAME is too long: %s\n",
		    __FUNCTION__, ptr);
	    sts = -1;
	    break;
	}

	/* Now we've got a reasonable NAME/ACCESS/PATHNAME.  Save them. */
	numlogfiles++;
	logfiles = realloc(logfiles, numlogfiles*sizeof(struct EventFileData));
	if (logfiles == NULL) {
	    fprintf(stderr, "%s: realloc failed: %s\n", __FUNCTION__,
		    strerror(errno));
	    sts = -1;
	    break;
	}
	data = &logfiles[numlogfiles - 1];
	memset(data, 0, sizeof(*data));
	data->restricted = (restrict[0] == 'y' || restrict[0] == 'Y');
	strncpy(data->pmnsname, name, sizeof(data->pmnsname));
	strncpy(data->pathname, ptr, sizeof(data->pathname));
	/* remaining fields filled in after pmdaInit() is called. */

	if (pmDebug & DBG_TRACE_APPL0)
	    __pmNotifyErr(LOG_INFO, "%s: saw logfile %s (%s)\n", __FUNCTION__,
		      data->pathname, data->pmnsname);
    }
    if (sts != 0) {
	free(logfiles);
	logfiles = NULL;
	numlogfiles = 0;
    }

    fclose(configFile);
    return sts;
}

static void
usage(void)
{
    fprintf(stderr,
	"Usage: %s [options] configfile\n\n"
	"Options:\n"
	"  -d domain    use domain (numeric) for metrics domain of PMDA\n"
	"  -l logfile   write log into logfile rather than the default\n"
	"  -m memory    maximum memory used per logfile (default %ld bytes)\n"
	"  -s interval  default delay between iterations (default %d sec)\n",
		pmProgname, maxmem, (int)interval.tv_sec);
    exit(1);
}

static int
logger_pmid(const char *name, pmID *pmid, pmdaExt *pmda)
{
    ctx_active(pmda->e_context);
    if (pmDebug & DBG_TRACE_APPL0)
	__pmNotifyErr(LOG_INFO, "%s: name %s\n", __FUNCTION__,
		  (name == NULL) ? "NULL" : name);
    return pmdaTreePMID(pmns, name, pmid);
}

static int
logger_name(pmID pmid, char ***nameset, pmdaExt *pmda)
{
    ctx_active(pmda->e_context);
    if (pmDebug & DBG_TRACE_APPL0)
	__pmNotifyErr(LOG_INFO, "%s: pmid 0x%x\n", __FUNCTION__, pmid);
    return pmdaTreeName(pmns, pmid, nameset);
}

static int
logger_children(const char *name, int traverse, char ***kids, int **sts,
		pmdaExt *pmda)
{
    ctx_active(pmda->e_context);
    if (pmDebug & DBG_TRACE_APPL0)
	__pmNotifyErr(LOG_INFO, "%s: name %s\n", __FUNCTION__,
		  (name == NULL) ? "NULL" : name);
    return pmdaTreeChildren(pmns, name, traverse, kids, sts);
}

static int
logger_text(int ident, int type, char **buffer, pmdaExt *pmda)
{
    int numstatics = sizeof(static_metrictab)/sizeof(static_metrictab[0]);

    ctx_active(pmda->e_context);

    if ((type & PM_TEXT_PMID) == PM_TEXT_PMID) {
	/* Lookup pmid in the metric table. */
	int item = pmid_item(ident);

	/* If the PMID item was for a dynamic metric... */
	if (item >= numstatics && item < nummetrics
	    /* and the PMID matches... */
	    && metrictab[item].m_desc.pmid == (pmID)ident
	    /* and we've got user data... */
	    && metrictab[item].m_user != NULL) {
	    struct dynamic_metric_info *pinfo = metrictab[item].m_user;

	    /* Return the correct help text. */
	    *buffer = (char *)pinfo->help_text;
	    return 0;
	}
    }
    return pmdaText(ident, type, buffer, pmda);
}

/*
 * Initialise the agent (daemon only).
 */
void 
logger_init(pmdaInterface *dp, const char *configfile)
{
    int i, j, sts, pmid_num;
    int numstatics = sizeof(static_metrictab)/sizeof(static_metrictab[0]);
    int numdynamics = sizeof(dynamic_metrictab)/sizeof(dynamic_metrictab[0]);
    pmdaMetric *pmetric;
    char name[MAXPATHLEN * 2];
    struct dynamic_metric_info *pinfo;

    /* Read and parse config file. */
    if (read_config(configfile) != 0)
	exit(1);
    if (numlogfiles == 0)
	usage();

    /* Create the dynamic metric info table based on the logfile table */
    dynamic_metric_infotab = malloc(sizeof(struct dynamic_metric_info)
				    * numdynamics * numlogfiles);
    if (dynamic_metric_infotab == NULL) {
	fprintf(stderr, "%s: allocation error: %s\n", __FUNCTION__,
		strerror(errno));
	return;
    }
    pinfo = dynamic_metric_infotab;
    for (i = 0; i < numlogfiles; i++) {
	for (j = 0; j < numdynamics; j++) {
	    pinfo->logfile = i;
	    pinfo->pmid_index = j;
	    pinfo->help_text = dynamic_helptab[j];
	    pinfo++;
	}
    }

    /* Create the metric table based on the static and dynamic metric tables */
    nummetrics = numstatics + (numlogfiles * numdynamics);
    metrictab = malloc(sizeof(pmdaMetric) * nummetrics);
    if (metrictab == NULL) {
	free(dynamic_metric_infotab);
	fprintf(stderr, "%s: allocation error: %s\n", __FUNCTION__,
		strerror(errno));
	return;
    }
    memcpy(metrictab, static_metrictab, sizeof(static_metrictab));
    pmetric = &metrictab[numstatics];
    pmid_num = numstatics;
    pinfo = dynamic_metric_infotab;
    for (i = 0; i < numlogfiles; i++) {
	memcpy(pmetric, dynamic_metrictab, sizeof(dynamic_metrictab));
	for (j = 0; j < numdynamics; j++) {
	    pmetric[j].m_desc.pmid = PMDA_PMID(0, pmid_num);
	    pmetric[j].m_user = pinfo++;
	    pmid_num++;
	}
	pmetric += numdynamics;
    }

    if (dp->status != 0)
	return;

    dp->version.four.fetch = logger_fetch;
    dp->version.four.store = logger_store;
    dp->version.four.profile = logger_profile;
    dp->version.four.pmid = logger_pmid;
    dp->version.four.name = logger_name;
    dp->version.four.children = logger_children;
    dp->version.four.text = logger_text;

    pmdaSetFetchCallBack(dp, logger_fetchCallBack);
    pmdaSetEndContextCallBack(dp, logger_end_contextCallBack);

    pmdaInit(dp, NULL, 0, metrictab, nummetrics);

    /* Create the dynamic PMNS tree and populate it. */
    if ((sts = __pmNewPMNS(&pmns)) < 0) {
	__pmNotifyErr(LOG_ERR, "%s: failed to create new pmns: %s\n",
			pmProgname, pmErrStr(sts));
	pmns = NULL;
	return;
    }
    pmetric = &metrictab[numstatics];
    for (i = 0; i < numlogfiles; i++) {
	for (j = 0; j < numdynamics; j++) {
	    snprintf(name, sizeof(name), "logger.perfile.%s.%s",
		     logfiles[i].pmnsname, dynamic_nametab[j]);
	    __pmAddPMNSNode(pmns, pmetric[j].m_desc.pmid, name);
	}
	pmetric += numdynamics;
    }
    /* for reverse (pmid->name) lookups */
    pmdaTreeRebuildHash(pmns, (numlogfiles * numdynamics));

    /* metric table is ready, update each logfile with the proper pmid */
    for (i = 0; i < numlogfiles; i++)
	logfiles[i].pmid = metrictab[2].m_desc.pmid;

    /* initialise the event and client tracking code */
    event_init();
}

static void
interval_timer(int sig, void *ptr)
{
    interval_expired = 1;
}

void
loggerMain(pmdaInterface *dispatch)
{
    fd_set		readyfds;
    int			nready, pmcdfd;

    pmcdfd = __pmdaInFd(dispatch);
    if (pmcdfd > maxfd)
	maxfd = pmcdfd;

    FD_ZERO(&fds);
    FD_SET(pmcdfd, &fds);

    /* arm interval timer */
    if (__pmAFregister(&interval, NULL, interval_timer) < 0) {
	__pmNotifyErr(LOG_ERR, "registering event interval handler");
	exit(1);
    }

    for (;;) {
	memcpy(&readyfds, &fds, sizeof(readyfds));
	nready = select(maxfd+1, &readyfds, NULL, NULL, NULL);
	if (pmDebug & DBG_TRACE_APPL2)
	    __pmNotifyErr(LOG_DEBUG, "select: nready=%d interval=%d",
			  nready, interval_expired);
	if (nready < 0) {
	    if (neterror() != EINTR) {
		__pmNotifyErr(LOG_ERR, "select failure: %s", netstrerror());
		exit(1);
	    } else if (!interval_expired) {
		continue;
	    }
	}

	__pmAFblock();
	if (nready > 0 && FD_ISSET(pmcdfd, &readyfds)) {
	    if (pmDebug & DBG_TRACE_APPL0)
		__pmNotifyErr(LOG_DEBUG, "processing pmcd PDU [fd=%d]", pmcdfd);
	    if (__pmdaMainPDU(dispatch) < 0) {
		__pmAFunblock();
		exit(1);	/* fatal if we lose pmcd */
	    }
	    if (pmDebug & DBG_TRACE_APPL0)
		__pmNotifyErr(LOG_DEBUG, "completed pmcd PDU [fd=%d]", pmcdfd);
	}
	if (interval_expired) {
	    interval_expired = 0;
	    logger_reload();
	}
	__pmAFunblock();
    }
}

static void
convertUnits(char **endnum, long *maxmem)
{
    switch ((int) **endnum) {
	case 'b':
	case 'B':
		break;
	case 'k':
	case 'K':
		*maxmem *= 1024;
		break;
	case 'm':
	case 'M':
		*maxmem *= 1024 * 1024;
		break;
	case 'g':
	case 'G':
		*maxmem *= 1024 * 1024 * 1024;
		break;
    }
    (*endnum)++;
}

int
main(int argc, char **argv)
{
    static char		helppath[MAXPATHLEN];
    char		*endnum;
    pmdaInterface	desc;
    long		minmem;
    int			c, err = 0, sep = __pmPathSeparator();

    minmem = getpagesize();
    maxmem = max(minmem, DEFAULT_MAXMEM);
    __pmSetProgname(argv[0]);
    snprintf(helppath, sizeof(helppath), "%s%c" "logger" "%c" "help",
		pmGetConfig("PCP_PMDAS_DIR"), sep, sep);
    pmdaDaemon(&desc, PMDA_INTERFACE_5, pmProgname, LOGGER,
		"logger.log", helppath);

    while ((c = pmdaGetOpt(argc, argv, "D:d:l:m:s:?", &desc, &err)) != EOF) {
	switch (c) {
	    case 'm':
		maxmem = strtol(optarg, &endnum, 10);
		if (*endnum != '\0')
		    convertUnits(&endnum, &maxmem);
		if (*endnum != '\0' || maxmem < minmem) {
		    fprintf(stderr, "%s: invalid max memory '%s' (min=%ld)\n",
			    pmProgname, optarg, minmem);
		    err++;
		}
		break;

	    case 's':
		if (pmParseInterval(optarg, &interval, &endnum) < 0) {
		    fprintf(stderr, "%s: -s requires a time interval: %s\n",
			    pmProgname, endnum);
		    free(endnum);
		    err++;
		}
		break;

	    default:
		err++;
		break;
	}
    }

    if (err || optind != argc -1)
    	usage();

    pmdaOpenLog(&desc);
    logger_init(&desc, argv[optind]);
    pmdaConnect(&desc);
    loggerMain(&desc);
    event_shutdown();
    exit(0);
}