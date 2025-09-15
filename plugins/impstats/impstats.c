/* impstats.c
 * A module to periodically output statistics gathered by rsyslog.
 *
 * Copyright 2010-2025 Adiscon GmbH.
 *
 * This file is part of rsyslog.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *       -or-
 *       see COPYING.ASL20 in the source distribution
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "config.h"
#include "rsyslog.h"
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <signal.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/time.h>
#include <time.h>
#include <sys/resource.h>
#ifdef OS_LINUX
#  include <sys/types.h>
#  include <dirent.h>
#endif
#include "dirty.h"
#include "cfsysline.h"
#include "module-template.h"
#include "errmsg.h"
#include "msg.h"
#include "srUtils.h"
#include "unicode-helper.h"
#include "glbl.h"
#include "statsobj.h"
#include "prop.h"
#include "ruleset.h"
#include "parserif.h"

MODULE_TYPE_INPUT
MODULE_TYPE_NOKEEP
MODULE_CNFNAME("impstats")

/* defines */
#define DEFAULT_STATS_PERIOD (5 * 60)
#define DEFAULT_FACILITY 5 /* syslog */
#define DEFAULT_SEVERITY 6 /* info */

/* Module static data */
DEF_IMOD_STATIC_DATA;
DEFobjCurrIf(glbl) DEFobjCurrIf(prop) DEFobjCurrIf(statsobj) DEFobjCurrIf(ruleset)

typedef struct configSettings_s {
    int iStatsInterval;
    int iFacility;
    int iSeverity;
    int bJSON;
    int bCEE;
    int bPrometheus;
} configSettings_t;

struct modConfData_s {
    rsconf_t *pConf; /* our overall config object */
    int iStatsInterval;
    int iFacility;
    int iSeverity;
    int logfd; /* fd if logging to file, or -1 if closed */
    ruleset_t *pBindRuleset; /* ruleset to bind listener to (use system default if unspecified) */
    statsFmtType_t statsFmt;
    sbool bLogToSyslog;
    sbool bResetCtrs;
    sbool bBracketing;
    char *logfile;
    sbool configSetViaV2Method;
    uchar *pszBindRuleset; /* name of ruleset to bind to */
};

static modConfData_t *loadModConf = NULL;
static modConfData_t *runModConf  = NULL;
static configSettings_t cs;
static int bLegacyCnfModGlobalsPermitted;
static prop_t *pInputName = NULL;

/* module-global parameters */
static struct cnfparamdescr modpdescr[] = {
    {"interval",     eCmdHdlrInt,    0}, {"facility",      eCmdHdlrInt,    0}, {"severity", eCmdHdlrInt,    0},
    {"bracketing",   eCmdHdlrBinary, 0}, {"log.syslog",    eCmdHdlrBinary, 0}, {"resetcounters", eCmdHdlrBinary, 0},
    {"log.file",     eCmdHdlrGetWord,0}, {"format",        eCmdHdlrGetWord,0}, {"ruleset",  eCmdHdlrString, 0}
};

static struct cnfparamblk modpblk =
    { CNFPARAMBLK_VERSION, sizeof(modpdescr) / sizeof(struct cnfparamdescr), modpdescr };

/* resource use stats counters */
#ifdef OS_LINUX
static int st_openfiles;
#endif
static intctr_t st_ru_utime;
static intctr_t st_ru_stime;
static intctr_t st_ru_maxrss;
static intctr_t st_ru_minflt;
static intctr_t st_ru_majflt;
static intctr_t st_ru_inblock;
static intctr_t st_ru_oublock;
static intctr_t st_ru_nvcsw;
static intctr_t st_ru_nivcsw;

static statsobj_t *statsobj_resources;

static pthread_mutex_t hup_mutex = PTHREAD_MUTEX_INITIALIZER;

/* forward declaration for Prometheus JSON output */
static void generatePrometheusStats(void);

BEGINmodExit
    CODESTARTmodExit;
    prop.Destruct(&pInputName);
    /* release objects we used */
    objRelease(glbl, CORE_COMPONENT);
    objRelease(prop, CORE_COMPONENT);
    objRelease(statsobj, CORE_COMPONENT);
    objRelease(ruleset, CORE_COMPONENT);
ENDmodExit

BEGINisCompatibleWithFeature
    CODESTARTisCompatibleWithFeature;
    if (eFeat == sFEATURENonCancelInputTermination) iRet = RS_RET_OK;
ENDisCompatibleWithFeature

#ifdef OS_LINUX
/* count number of open files (linux specific) */
static void countOpenFiles(void) {
    char proc_path[MAXFNAME];
    DIR *dp;
    struct dirent *files;
    st_openfiles = 0;
    snprintf(proc_path, sizeof(proc_path), "/proc/%d/fd", glblGetOurPid());
    if ((dp = opendir(proc_path)) == NULL) {
        LogError(errno, RS_RET_ERR, "impstats: error reading %s\n", proc_path);
        goto done;
    }
    while ((files = readdir(dp)) != NULL) {
        if (!strcmp(files->d_name, ".") || !strcmp(files->d_name, "..")) continue;
        st_openfiles++;
    }
    closedir(dp);
done:
    return;
}
#endif

static void initConfigSettings(void) {
    cs.iStatsInterval = DEFAULT_STATS_PERIOD;
    cs.iFacility = DEFAULT_FACILITY;
    cs.iSeverity = DEFAULT_SEVERITY;
    cs.bJSON = 0;
    cs.bCEE = 0;
    cs.bPrometheus = 0;
}

/* actually submit a message to the rsyslog core */
static void doSubmitMsg(uchar *line) {
    smsg_t *pMsg;

    if (msgConstruct(&pMsg) != RS_RET_OK) goto finalize_it;
    MsgSetInputName(pMsg, pInputName);
    /* COPY-IN variant so the core owns the message buffer */
    MsgSetRawMsg(pMsg, (uchar*)line, strlen((char*)line));
    MsgSetHOSTNAME(pMsg, glbl.GetLocalHostName(), ustrlen(glbl.GetLocalHostName()));
    MsgSetRcvFrom(pMsg, glbl.GetLocalHostNameProp());
    MsgSetRcvFromIP(pMsg, glbl.GetLocalHostIP());
    MsgSetMSGoffs(pMsg, 0);
    MsgSetRuleset(pMsg, runModConf->pBindRuleset);
    MsgSetTAG(pMsg, UCHAR_CONSTANT("rsyslogd-pstats:"), sizeof("rsyslogd-pstats:") - 1);
    pMsg->iFacility = runModConf->iFacility;
    pMsg->iSeverity = runModConf->iSeverity;
    pMsg->msgFlags = 0;

    /* stats messages are always emitted */
    submitMsg2(pMsg);
    DBGPRINTF("impstats: submit [%d,%d] msg '%s'\n",
        runModConf->iFacility, runModConf->iSeverity, line);
finalize_it:
    return;
}

/* log stats message to file; limited error handling done */
static void doLogToFile(const char *ln, const size_t lenLn) {
    struct iovec iov[4];
    ssize_t nwritten;
    ssize_t nexpect;
    time_t t;
    char timebuf[32];

    pthread_mutex_lock(&hup_mutex);

    if (lenLn == 0) goto done;
    if (runModConf->logfd == -1) {
        runModConf->logfd = open(runModConf->logfile,
            O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
            S_IRUSR | S_IWUSR);
        if (runModConf->logfd == -1) {
            DBGPRINTF("impstats: error opening stats file %s\n", runModConf->logfile);
            goto done;
        } else {
            DBGPRINTF("impstats: opened stats file %s\n", runModConf->logfile);
        }
    }
    time(&t);
    iov[0].iov_base = ctime_r(&t, timebuf);
    iov[0].iov_len = nexpect = strlen((char*)iov[0].iov_base) - 1; /* strip \n */
    iov[1].iov_base = (void *)": ";
    iov[1].iov_len = 2;
    nexpect += 2;
    iov[2].iov_base = (void *)ln;
    iov[2].iov_len = lenLn;
    nexpect += lenLn;
    iov[3].iov_base = (void *)"\n";
    iov[3].iov_len = 1;
    nexpect++;
    nwritten = writev(runModConf->logfd, iov, 4);
    if (nwritten != nexpect) {
        DBGPRINTF("error writing stats file %s, nwritten %lld, expected %lld\n",
            runModConf->logfile, (long long)nwritten, (long long)nexpect);
    }
done:
    pthread_mutex_unlock(&hup_mutex);
    return;
}

/* submit a line to our log destinations. Line must be fully formatted */
static rsRetVal submitLine(const char *const ln, const size_t lenLn) {
    DEFiRet;
    if (runModConf->bLogToSyslog) doSubmitMsg((uchar*)ln);
    if (runModConf->logfile != NULL) doLogToFile(ln, lenLn);
    RETiRet;
}

/* callback for statsobj */
static rsRetVal doStatsLine(void __attribute__((unused)) * usrptr, const char *const str) {
    DEFiRet;
    iRet = submitLine(str, strlen(str));
    RETiRet;
}

/* generate statistics messages */
static void generateStatsMsgs(void) {
    struct rusage ru;
    int r;

    r = getrusage(RUSAGE_SELF, &ru);
    if (r != 0) {
        DBGPRINTF("impstats: getrusage() failed with error %d, zeroing out\n", errno);
        memset(&ru, 0, sizeof(ru));
    }
#ifdef OS_LINUX
    countOpenFiles();
#endif
    st_ru_utime = ru.ru_utime.tv_sec * 1000000 + ru.ru_utime.tv_usec;
    st_ru_stime = ru.ru_stime.tv_sec * 1000000 + ru.ru_stime.tv_usec;
    st_ru_maxrss = ru.ru_maxrss;
    st_ru_minflt = ru.ru_minflt;
    st_ru_majflt = ru.ru_majflt;
    st_ru_inblock= ru.ru_inblock;
    st_ru_oublock= ru.ru_oublock;
    st_ru_nvcsw = ru.ru_nvcsw;
    st_ru_nivcsw = ru.ru_nivcsw;

    if (runModConf->statsFmt == statsFmt_Prometheus) {
#ifdef DEBUG_PROM
        DBGPRINTF("impstats: statsFmt=%d (Prometheus=%d) -> entering generatePrometheusStats()\n",
            (int)runModConf->statsFmt, (int)statsFmt_Prometheus);
#endif
        generatePrometheusStats();
    } else {
        statsobj.GetAllStatsLines(doStatsLine, NULL, runModConf->statsFmt, runModConf->bResetCtrs);
    }
}

BEGINbeginCnfLoad
    CODESTARTbeginCnfLoad;
    loadModConf = pModConf;
    pModConf->pConf = pConf;

    /* init our settings */
    loadModConf->configSetViaV2Method = 0;
    loadModConf->iStatsInterval = DEFAULT_STATS_PERIOD;
    loadModConf->iFacility = DEFAULT_FACILITY;
    loadModConf->iSeverity = DEFAULT_SEVERITY;
    loadModConf->statsFmt = statsFmt_Legacy;
    loadModConf->logfd = -1;
    loadModConf->logfile = NULL;
    loadModConf->pszBindRuleset = NULL;
    loadModConf->bLogToSyslog = 1;
    loadModConf->bBracketing = 0;
    loadModConf->bResetCtrs = 0;

    bLegacyCnfModGlobalsPermitted = 1;
    initConfigSettings();
ENDbeginCnfLoad

BEGINsetModCnf
    struct cnfparamvals *pvals = NULL;
    char *mode;
    int i;

    CODESTARTsetModCnf;
    pvals = nvlstGetParams(lst, &modpblk, NULL);
    if (pvals == NULL) {
        LogError(0, RS_RET_MISSING_CNFPARAMS,
            "error processing module config parameters [module(...)]");
        ABORT_FINALIZE(RS_RET_MISSING_CNFPARAMS);
    }
    if (Debug) {
        DBGPRINTF("module (global) param blk for impstats:\n");
        cnfparamsPrint(&modpblk, pvals);
    }
    for (i = 0; i < modpblk.nParams; ++i) {
        if (!pvals[i].bUsed) continue;
        if (!strcmp(modpblk.descr[i].name, "interval")) {
            loadModConf->iStatsInterval = (int)pvals[i].val.d.n;
        } else if (!strcmp(modpblk.descr[i].name, "facility")) {
            loadModConf->iFacility = (int)pvals[i].val.d.n;
        } else if (!strcmp(modpblk.descr[i].name, "severity")) {
            loadModConf->iSeverity = (int)pvals[i].val.d.n;
        } else if (!strcmp(modpblk.descr[i].name, "bracketing")) {
            loadModConf->bBracketing = (sbool)pvals[i].val.d.n;
        } else if (!strcmp(modpblk.descr[i].name, "log.syslog")) {
            loadModConf->bLogToSyslog = (sbool)pvals[i].val.d.n;
        } else if (!strcmp(modpblk.descr[i].name, "resetcounters")) {
            loadModConf->bResetCtrs = (sbool)pvals[i].val.d.n;
        } else if (!strcmp(modpblk.descr[i].name, "log.file")) {
            loadModConf->logfile = es_str2cstr(pvals[i].val.d.estr, NULL);
        } else if (!strcmp(modpblk.descr[i].name, "format")) {
            mode = es_str2cstr(pvals[i].val.d.estr, NULL);
            if (!strcasecmp(mode, "json")) {
                loadModConf->statsFmt = statsFmt_JSON;
            } else if (!strcasecmp(mode, "json-elasticsearch")) {
                loadModConf->statsFmt = statsFmt_JSON_ES;
            } else if (!strcasecmp(mode, "cee")) {
                loadModConf->statsFmt = statsFmt_CEE;
            } else if (!strcasecmp(mode, "legacy")) {
                loadModConf->statsFmt = statsFmt_Legacy;
            } else if (!strcasecmp(mode, "prometheus")) {
                loadModConf->statsFmt = statsFmt_Prometheus;
            } else {
                LogError(0, RS_RET_ERR, "impstats: invalid format %s", mode);
            }
            free(mode);
        } else if (!strcmp(modpblk.descr[i].name, "ruleset")) {
            loadModConf->pszBindRuleset = (uchar*)es_str2cstr(pvals[i].val.d.estr, NULL);
        } else {
            DBGPRINTF("impstats: program error, non-handled param '%s' in beginCnfLoad\n",
                modpblk.descr[i].name);
        }
    }
    if (loadModConf->pszBindRuleset != NULL && loadModConf->bLogToSyslog == 0) {
        parser_warnmsg(
            "impstats: log.syslog set to \"off\" but ruleset specified - with "
            "these settings, the ruleset will never be used, because regular syslog "
            "processing is turned off - ruleset parameter is ignored");
        free(loadModConf->pszBindRuleset);
        loadModConf->pszBindRuleset = NULL;
    }
    loadModConf->configSetViaV2Method = 1;
    bLegacyCnfModGlobalsPermitted = 0;
finalize_it:
    if (pvals != NULL) cnfparamvalsDestruct(pvals, &modpblk);
ENDsetModCnf

BEGINendCnfLoad
    CODESTARTendCnfLoad;
    if (!loadModConf->configSetViaV2Method) {
        loadModConf->iStatsInterval = cs.iStatsInterval;
        loadModConf->iFacility = cs.iFacility;
        loadModConf->iSeverity = cs.iSeverity;
        if (cs.bCEE == 1) {
            loadModConf->statsFmt = statsFmt_CEE;
        } else if (cs.bJSON == 1) {
            loadModConf->statsFmt = statsFmt_JSON;
        } else {
            loadModConf->statsFmt = statsFmt_Legacy;
        }
    }
ENDendCnfLoad

/* Resolve ruleset name if set */
static rsRetVal checkRuleset(modConfData_t *modConf) {
    ruleset_t *pRuleset;
    rsRetVal localRet;
    DEFiRet;

    modConf->pBindRuleset = NULL; /* default ruleset */
    if (modConf->pszBindRuleset == NULL) FINALIZE;

    localRet = ruleset.GetRuleset(modConf->pConf, &pRuleset, modConf->pszBindRuleset);
    if (localRet == RS_RET_NOT_FOUND) {
        LogError(0, NO_ERRCODE,
            "impstats: ruleset '%s' not found - using default ruleset instead",
            modConf->pszBindRuleset);
    }
    CHKiRet(localRet);
    modConf->pBindRuleset = pRuleset;

finalize_it:
    RETiRet;
}

/* to use HUP, we need to have an instanceData type */
typedef struct _instanceData {
    int dummy;
} instanceData;

BEGINdoHUP
    CODESTARTdoHUP;
    DBGPRINTF("impstats: received HUP\n");
    pthread_mutex_lock(&hup_mutex);
    if (runModConf->logfd != -1) {
        DBGPRINTF("impstats: closing log file due to HUP\n");
        close(runModConf->logfd);
        runModConf->logfd = -1;
    }
    pthread_mutex_unlock(&hup_mutex);
ENDdoHUP

BEGINcheckCnf
    CODESTARTcheckCnf;
    if (pModConf->iStatsInterval == 0) {
        LogError(0, NO_ERRCODE,
            "impstats: stats interval zero not permitted, using default of %d seconds",
            DEFAULT_STATS_PERIOD);
        pModConf->iStatsInterval = DEFAULT_STATS_PERIOD;
    }
    checkRuleset(pModConf);
ENDcheckCnf

BEGINactivateCnf
    rsRetVal localRet;
    CODESTARTactivateCnf;
    runModConf = pModConf;
    DBGPRINTF("impstats: stats interval %d seconds, reset %d, logToSyslog %d, logFile %s\n",
        runModConf->iStatsInterval,
        runModConf->bResetCtrs,
        runModConf->bLogToSyslog,
        runModConf->logfile == NULL ? "deactivated" : (char*)runModConf->logfile);

    localRet = statsobj.EnableStats();
    if (localRet != RS_RET_OK) {
        LogError(0, localRet, "impstats: error enabling statistics gathering");
        ABORT_FINALIZE(RS_RET_NO_RUN);
    }

    /* initialize our own counters */
    CHKiRet(statsobj.Construct(&statsobj_resources));
    CHKiRet(statsobj.SetName(statsobj_resources, (uchar*)"resource-usage"));
    CHKiRet(statsobj.SetOrigin(statsobj_resources, (uchar*)"impstats"));
    CHKiRet(statsobj.AddCounter(statsobj_resources, UCHAR_CONSTANT("utime"),
        ctrType_IntCtr, CTR_FLAG_NONE, &st_ru_utime));
    CHKiRet(statsobj.AddCounter(statsobj_resources, UCHAR_CONSTANT("stime"),
        ctrType_IntCtr, CTR_FLAG_NONE, &st_ru_stime));
    CHKiRet(statsobj.AddCounter(statsobj_resources, UCHAR_CONSTANT("maxrss"),
        ctrType_IntCtr, CTR_FLAG_NONE, &st_ru_maxrss));
    CHKiRet(statsobj.AddCounter(statsobj_resources, UCHAR_CONSTANT("minflt"),
        ctrType_IntCtr, CTR_FLAG_NONE, &st_ru_minflt));
    CHKiRet(statsobj.AddCounter(statsobj_resources, UCHAR_CONSTANT("majflt"),
        ctrType_IntCtr, CTR_FLAG_NONE, &st_ru_majflt));
    CHKiRet(statsobj.AddCounter(statsobj_resources, UCHAR_CONSTANT("inblock"),
        ctrType_IntCtr, CTR_FLAG_NONE, &st_ru_inblock));
    CHKiRet(statsobj.AddCounter(statsobj_resources, UCHAR_CONSTANT("oublock"),
        ctrType_IntCtr, CTR_FLAG_NONE, &st_ru_oublock));
    CHKiRet(statsobj.AddCounter(statsobj_resources, UCHAR_CONSTANT("nvcsw"),
        ctrType_IntCtr, CTR_FLAG_NONE, &st_ru_nvcsw));
    CHKiRet(statsobj.AddCounter(statsobj_resources, UCHAR_CONSTANT("nivcsw"),
        ctrType_IntCtr, CTR_FLAG_NONE, &st_ru_nivcsw));
#ifdef OS_LINUX
    CHKiRet(statsobj.AddCounter(statsobj_resources, UCHAR_CONSTANT("openfiles"),
        ctrType_Int, CTR_FLAG_NONE, &st_openfiles));
#endif
    CHKiRet(statsobj.ConstructFinalize(statsobj_resources));

finalize_it:
    if (iRet != RS_RET_OK) {
        LogError(0, iRet, "impstats: error activating module");
        iRet = RS_RET_NO_RUN;
    }
ENDactivateCnf

BEGINfreeCnf
    CODESTARTfreeCnf;
    if (runModConf->logfd != -1) close(runModConf->logfd);
    free(runModConf->logfile);
    free(runModConf->pszBindRuleset);
ENDfreeCnf

BEGINrunInput
    CODESTARTrunInput;
    while (glbl.GetGlobalInputTermState() == 0) {
        srSleep(runModConf->iStatsInterval, 0); /* seconds, micro seconds */
        DBGPRINTF("impstats: woke up, generating messages\n");
        if (runModConf->bBracketing) submitLine("BEGIN", sizeof("BEGIN") - 1);
        generateStatsMsgs();
        if (runModConf->bBracketing) submitLine("END", sizeof("END") - 1);
    }
ENDrunInput

BEGINwillRun
    CODESTARTwillRun;
ENDwillRun

BEGINafterRun
    CODESTARTafterRun;
ENDafterRun

BEGINqueryEtryPt
    CODESTARTqueryEtryPt;
    CODEqueryEtryPt_STD_IMOD_QUERIES;
    CODEqueryEtryPt_STD_CONF2_QUERIES;
    CODEqueryEtryPt_STD_CONF2_setModCnf_QUERIES;
    CODEqueryEtryPt_IsCompatibleWithFeature_IF_OMOD_QUERIES;
    CODEqueryEtryPt_doHUP;
ENDqueryEtryPt

static rsRetVal resetConfigVariables(uchar __attribute__((unused)) * pp, void __attribute__((unused)) * pVal) {
    initConfigSettings();
    return RS_RET_OK;
}

BEGINmodInit()
    CODESTARTmodInit;
    *ipIFVersProvided = CURR_MOD_IF_VERSION; /* support the current interface spec */
    CODEmodInit_QueryRegCFSLineHdlr
    DBGPRINTF("impstats version %s loading\n", VERSION);
    initConfigSettings();
    CHKiRet(objUse(glbl, CORE_COMPONENT));
    CHKiRet(objUse(prop, CORE_COMPONENT));
    CHKiRet(objUse(statsobj, CORE_COMPONENT));
    CHKiRet(objUse(ruleset, CORE_COMPONENT));
    /* legacy aliases */
    CHKiRet(regCfSysLineHdlr2((uchar*)"pstatsinterval", 0, eCmdHdlrInt, NULL, &cs.iStatsInterval,
        STD_LOADABLE_MODULE_ID, &bLegacyCnfModGlobalsPermitted));
    CHKiRet(regCfSysLineHdlr2((uchar*)"pstatinterval", 0, eCmdHdlrInt, NULL, &cs.iStatsInterval,
        STD_LOADABLE_MODULE_ID, &bLegacyCnfModGlobalsPermitted));
    CHKiRet(regCfSysLineHdlr2((uchar*)"pstatfacility", 0, eCmdHdlrInt, NULL, &cs.iFacility,
        STD_LOADABLE_MODULE_ID, &bLegacyCnfModGlobalsPermitted));
    CHKiRet(regCfSysLineHdlr2((uchar*)"pstatseverity", 0, eCmdHdlrInt, NULL, &cs.iSeverity,
        STD_LOADABLE_MODULE_ID, &bLegacyCnfModGlobalsPermitted));
    CHKiRet(regCfSysLineHdlr2((uchar*)"pstatjson", 0, eCmdHdlrBinary, NULL, &cs.bJSON,
        STD_LOADABLE_MODULE_ID, &bLegacyCnfModGlobalsPermitted));
    CHKiRet(regCfSysLineHdlr2((uchar*)"pstatcee", 0, eCmdHdlrBinary, NULL, &cs.bCEE,
        STD_LOADABLE_MODULE_ID, &bLegacyCnfModGlobalsPermitted));
    CHKiRet(omsdRegCFSLineHdlr((uchar*)"resetconfigvariables", 1, eCmdHdlrCustomHandler,
        resetConfigVariables, NULL, STD_LOADABLE_MODULE_ID));
    CHKiRet(prop.Construct(&pInputName));
    CHKiRet(prop.SetString(pInputName, UCHAR_CONSTANT("impstats"), sizeof("impstats") - 1));
    CHKiRet(prop.ConstructFinalize(pInputName));
ENDmodInit

/* ----- Prometheus (single JSON object) builder -----
 * Produces: { "timedate": "<ctime_r>", "stats": [ <JSON lines> ] }
 * Uses the statsobj JSON formatter to collect each line, then wraps it.
 */
typedef struct prom_ctx_s {
    es_str_t *arr; /* elastic string for the JSON array body */
    int first;     /* whether we've added the first element */
    size_t item_count;/* number of items collected (debug) */
} prom_ctx_t;

static rsRetVal collectStats_prom(void *usrptr, const char *const str) {
    prom_ctx_t *ctx = (prom_ctx_t*)usrptr;
    if (!ctx->first)
        es_addChar(&ctx->arr, ',');
    es_addBuf(&ctx->arr, str, strlen(str));
    ctx->first = 0;
    ctx->item_count++;
    return RS_RET_OK;
}

/* vi:set ai: */
static void generatePrometheusStats(void) {
    /* Update resource counters (safe/optional) */
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        st_ru_utime = ru.ru_utime.tv_sec * 1000000 + ru.ru_utime.tv_usec;
        st_ru_stime = ru.ru_stime.tv_sec * 1000000 + ru.ru_stime.tv_usec;
        st_ru_maxrss = ru.ru_maxrss;
        st_ru_minflt = ru.ru_minflt;
        st_ru_majflt = ru.ru_majflt;
        st_ru_inblock= ru.ru_inblock;
        st_ru_oublock= ru.ru_oublock;
        st_ru_nvcsw = ru.ru_nvcsw;
        st_ru_nivcsw = ru.ru_nivcsw;
    }
#ifdef OS_LINUX
    countOpenFiles();
#endif

    /* Build the stats array by collecting JSON lines.
     * IMPORTANT: es_* mutators may reallocate; always use ctx.arr after calls.
     */
    prom_ctx_t ctx = { .arr = es_newStrFromCStr("[", 1), .first = 1, .item_count = 0 };

#ifdef DEBUG_PROM
    DBGPRINTF("impstats: generatePrometheusStats() start\n");
#endif
    statsobj.GetAllStatsLines(collectStats_prom, &ctx, statsFmt_JSON, runModConf->bResetCtrs);
    es_addChar(&ctx.arr, ']');

#ifdef DEBUG_PROM
    DBGPRINTF("impstats: Prom items collected = %zu, JSON bytes = %zu\n",
        ctx.item_count, es_strlen(ctx.arr));
    if (es_strlen(ctx.arr) >= 2) {
        const char *p = es_getBufAddr(ctx.arr);
        DBGPRINTF("impstats: Prom statsArr sample: first='%c' last='%c'\n",
            p[0], p[es_strlen(ctx.arr)-1]);
    }
#endif

    /* Timestamp */
    time_t t = time(NULL);
    char timebuf[32];
#if defined(OS_LINUX) || defined(_POSIX_VERSION)
    struct tm tm_local;
    localtime_r(&t, &tm_local);
    strftime(timebuf, sizeof(timebuf), "%a %b %d %H:%M:%S %Y", &tm_local);
#else
    strftime(timebuf, sizeof(timebuf), "%a %b %d %H:%M:%S %Y", localtime(&t));
#endif
    /* Wrap into a single JSON object */
    es_str_t *finalJson = es_newStrFromCStr("{ \"timedate\": \"", strlen("{ \"timedate\": \""));
    es_addBuf(&finalJson, timebuf, strlen(timebuf));
    es_addBuf(&finalJson, "\", \"stats\": ", strlen("\", \"stats\": "));
    /* Append collected array buffer */
    es_addBuf(&finalJson, es_getBufAddr(ctx.arr), es_strlen(ctx.arr));
    es_addChar(&finalJson, '}');

#ifdef DEBUG_PROM
    DBGPRINTF("impstats: Prom final JSON length = %zu\n", es_strlen(finalJson));
#endif
    /* Convert to cstr for submit; free after submit to avoid leaks */
    char *out = es_str2cstr(finalJson, NULL);
    submitLine(out, strlen(out));
    free(out);

#ifdef DEBUG_PROM
    DBGPRINTF("impstats: Prom submit done; log.syslog=%d logfd=%d\n",
        (int)runModConf->bLogToSyslog, runModConf->logfd);
#endif

    es_deleteStr(ctx.arr);
    es_deleteStr(finalJson);
}
